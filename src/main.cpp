#include <Geode/Geode.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/utils/file.hpp>
#include <gdr/gdr.hpp>
#include <algorithm>
#include <sstream>

using namespace geode::prelude;

// ---------------------------------------------------------------------------
// Macro data
// ---------------------------------------------------------------------------
struct Click { int down; int up; };

// gdr2 reader types (from the GDReplayFormat library)
struct KInput : gdr::Input<KInput> {
    KInput() = default;
    KInput(int frame, int button, bool player2, bool down)
        : Input(frame, button, player2, down) {}
};
struct KReplay : gdr::Replay<KReplay, KInput> {
    KReplay() : Replay("Kaze", 1) {}
};

namespace Kaze {
    inline std::vector<Click> macro;
    inline std::vector<bool> used;
    inline std::vector<std::pair<int, bool>> queue; // frame, isDown
    inline bool playing = false;
    inline bool injecting = false;
    inline int frame = 0;
    inline int swallowed = 0;
    inline int window = 10;

    inline std::vector<std::filesystem::path> files;
    inline int fileIdx = 0;

    inline std::filesystem::path macroDir() {
        auto dir = Mod::get()->getSaveDir() / "macros";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        return dir;
    }

    inline void scanFiles() {
        files.clear();
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(macroDir(), ec)) {
            if (!e.is_regular_file()) continue;
            auto ext = e.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".gdr" || ext == ".gdr2" || ext == ".txt") files.push_back(e.path());
        }
        std::sort(files.begin(), files.end());
        if (fileIdx >= (int)files.size()) fileIdx = 0;
    }

    inline void resetRun() {
        frame = 0;
        swallowed = 0;
        queue.clear();
        used.assign(macro.size(), false);
    }

    // turns a list of (frame, down) events of the player-1 jump button into press/release pairs
    inline std::vector<Click> pairUp(std::vector<std::pair<int, bool>> ev) {
        std::stable_sort(ev.begin(), ev.end(),
            [](auto& a, auto& b) { return a.first < b.first; });
        std::vector<Click> out;
        int pending = -1;
        for (auto& [f, d] : ev) {
            if (d) {
                if (pending >= 0) out.push_back({pending, f});
                pending = f;
            } else if (pending >= 0) {
                out.push_back({pending, f});
                pending = -1;
            }
        }
        if (pending >= 0) out.push_back({pending, pending + 2});
        return out;
    }

    // .txt : one jump per line, "120" or "120 125"
    inline bool loadTxt(std::filesystem::path const& path) {
        auto res = file::readString(path);
        if (!res) return false;
        std::vector<Click> out;
        std::stringstream ss(res.unwrap());
        std::string line;
        while (std::getline(ss, line)) {
            std::stringstream ls(line);
            int d = -1, u = -1;
            if (!(ls >> d)) continue;
            if (!(ls >> u)) u = d + 2;
            out.push_back({d, u});
        }
        macro = out;
        return !macro.empty();
    }

    // .gdr : JSON replay, inputs = [{ "frame":N, "btn":1, "2p":false, "down":true }, ...]
    inline bool loadGdr(std::filesystem::path const& path) {
        auto res = file::readString(path);
        if (!res) return false;
        auto parsed = matjson::parse(res.unwrap());
        if (!parsed) return false;
        auto root = parsed.unwrap();
        if (!root.contains("inputs")) return false;
        auto arr = root["inputs"].asArray();
        if (!arr) return false;
        std::vector<std::pair<int, bool>> ev;
        for (auto& in : arr.unwrap()) {
            int btn = in["btn"].asInt().unwrapOr(1);
            bool p2 = in["2p"].asBool().unwrapOr(false);
            if (btn != 1 || p2) continue; // only player 1 jump
            ev.push_back({
                (int)in["frame"].asInt().unwrapOr(0),
                in["down"].asBool().unwrapOr(false)
            });
        }
        macro = pairUp(ev);
        return !macro.empty();
    }

    // .gdr2 : binary replay, read with the gdr library
    inline bool loadGdr2(std::filesystem::path const& path) {
        auto res = file::readBinary(path);
        if (!res) return false;
        auto bytes = res.unwrap();
        auto replay = KReplay::importData(bytes);
        std::vector<std::pair<int, bool>> ev;
        for (auto& in : replay.inputs) {
            if (in.button != 1 || in.player2) continue; // only player 1 jump
            ev.push_back({(int)in.frame, (bool)in.down});
        }
        macro = pairUp(ev);
        return !macro.empty();
    }

    inline bool load(std::filesystem::path const& path) {
        auto ext = path.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        bool ok = false;
        if (ext == ".gdr2") ok = loadGdr2(path);
        else if (ext == ".gdr") ok = loadGdr(path);
        else ok = loadTxt(path);
        resetRun();
        return ok;
    }
}

// ---------------------------------------------------------------------------
// Kaze popup
// ---------------------------------------------------------------------------
class KazePopup : public geode::Popup {
protected:
    ButtonSprite* m_playSpr = nullptr;
    CCLabelBMFont* m_frames = nullptr;
    CCLabelBMFont* m_status = nullptr;
    CCLabelBMFont* m_fileName = nullptr;

    bool init() {
        if (!Popup::init(300.f, 250.f)) return false;
        this->setTitle("Kaze");
        auto size = m_mainLayer->getContentSize();
        auto menu = CCMenu::create();
        menu->setPosition(0, 0);
        m_mainLayer->addChild(menu);

        Kaze::scanFiles();

        // file selector  [<]  name  [>]
        m_fileName = CCLabelBMFont::create("", "chatFont.fnt");
        m_fileName->setPosition(size.width / 2, size.height - 55);
        m_fileName->setScale(0.8f);
        m_mainLayer->addChild(m_fileName);

        auto left = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("<"), this, menu_selector(KazePopup::onPrev));
        left->setPosition(30, size.height - 55);
        menu->addChild(left);
        auto right = CCMenuItemSpriteExtra::create(
            ButtonSprite::create(">"), this, menu_selector(KazePopup::onNext));
        right->setPosition(size.width - 30, size.height - 55);
        menu->addChild(right);

        // 1. Import macro
        auto imp = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Import Macro"), this, menu_selector(KazePopup::onImport));
        imp->setPosition(size.width / 2 - 55, size.height - 95);
        menu->addChild(imp);

        auto folder = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Folder"), this, menu_selector(KazePopup::onFolder));
        folder->setPosition(size.width / 2 + 85, size.height - 95);
        menu->addChild(folder);

        // 2. Play macro (toggle)
        m_playSpr = ButtonSprite::create(Kaze::playing ? "Play: ON" : "Play: OFF");
        auto play = CCMenuItemSpriteExtra::create(m_playSpr, this, menu_selector(KazePopup::onPlay));
        play->setPosition(size.width / 2, size.height - 135);
        menu->addChild(play);

        // 3. Click frames  [-] N [+]
        auto lbl = CCLabelBMFont::create("Click Frames", "bigFont.fnt");
        lbl->setScale(0.5f);
        lbl->setPosition(size.width / 2, size.height - 170);
        m_mainLayer->addChild(lbl);

        m_frames = CCLabelBMFont::create(std::to_string(Kaze::window).c_str(), "goldFont.fnt");
        m_frames->setPosition(size.width / 2, size.height - 195);
        m_mainLayer->addChild(m_frames);

        auto minus = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("-"), this, menu_selector(KazePopup::onMinus));
        minus->setPosition(size.width / 2 - 60, size.height - 195);
        menu->addChild(minus);
        auto plus = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("+"), this, menu_selector(KazePopup::onPlus));
        plus->setPosition(size.width / 2 + 60, size.height - 195);
        menu->addChild(plus);

        m_status = CCLabelBMFont::create("", "chatFont.fnt");
        m_status->setPosition(size.width / 2, 18);
        m_status->setScale(0.8f);
        m_mainLayer->addChild(m_status);

        refreshFile();
        setStatus(fmt::format("{} jumps loaded", Kaze::macro.size()));
        return true;
    }

    void setStatus(std::string const& s) { m_status->setString(s.c_str()); }

    void refreshFile() {
        if (Kaze::files.empty()) {
            m_fileName->setString("No macros - press Folder");
        } else {
            m_fileName->setString(Kaze::files[Kaze::fileIdx].filename().string().c_str());
        }
    }

    void onPrev(CCObject*) {
        if (Kaze::files.empty()) return;
        Kaze::fileIdx = (Kaze::fileIdx - 1 + (int)Kaze::files.size()) % (int)Kaze::files.size();
        refreshFile();
    }
    void onNext(CCObject*) {
        if (Kaze::files.empty()) return;
        Kaze::fileIdx = (Kaze::fileIdx + 1) % (int)Kaze::files.size();
        refreshFile();
    }

    void onImport(CCObject*) {
        Kaze::scanFiles();
        refreshFile();
        if (Kaze::files.empty()) return setStatus("Put macros in the Folder first");
        bool ok = Kaze::load(Kaze::files[Kaze::fileIdx]);
        setStatus(ok ? fmt::format("{} jumps loaded", Kaze::macro.size())
                     : std::string("Could not read that macro"));
    }

    void onFolder(CCObject*) {
        file::openFolder(Kaze::macroDir());
    }

    void onPlay(CCObject*) {
        Kaze::playing = !Kaze::playing;
        m_playSpr->setString(Kaze::playing ? "Play: ON" : "Play: OFF");
        Kaze::resetRun();
    }

    void onMinus(CCObject*) { Kaze::window = std::max(1, Kaze::window - 1); refreshFrames(); }
    void onPlus(CCObject*)  { Kaze::window = std::min(60, Kaze::window + 1); refreshFrames(); }
    void refreshFrames() { m_frames->setString(std::to_string(Kaze::window).c_str()); }

public:
    static KazePopup* create() {
        auto ret = new KazePopup();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }
};

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
class $modify(KazePause, PauseLayer) {
    void customSetup() {
        PauseLayer::customSetup();
        auto win = CCDirector::get()->getWinSize();
        auto menu = CCMenu::create();
        auto btn = CCMenuItemSpriteExtra::create(
            ButtonSprite::create("Kaze"), this, menu_selector(KazePause::onKaze));
        menu->addChild(btn);
        menu->setPosition(win.width - 50.f, win.height / 2);
        this->addChild(menu);
    }
    void onKaze(CCObject*) { KazePopup::create()->show(); }
};

class $modify(KazePlay, PlayLayer) {
    void resetLevel() {
        PlayLayer::resetLevel();
        Kaze::resetRun();
    }
};

class $modify(KazeGame, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        if (!Kaze::playing || Kaze::injecting || !isPlayer1 || Kaze::macro.empty()) {
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        if (!down) {
            if (Kaze::swallowed > 0) { Kaze::swallowed--; return; }
            return GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
        // nearest unused macro jump inside the click window
        int best = -1, bestDist = 1 << 30;
        for (size_t i = 0; i < Kaze::macro.size(); i++) {
            if (Kaze::used[i]) continue;
            int d = std::abs(Kaze::macro[i].down - Kaze::frame);
            if (d <= Kaze::window && d < bestDist) { best = (int)i; bestDist = d; }
        }
        if (best < 0) return GJBaseGameLayer::handleButton(down, button, isPlayer1);

        Kaze::used[best] = true;
        auto c = Kaze::macro[best];
        if (c.down > Kaze::frame) {
            // jump is coming up: hold the click and fire it on the recorded frame
            Kaze::queue.push_back({c.down, true});
            Kaze::queue.push_back({c.up, false});
            Kaze::swallowed++;
            return;
        }
        // already at/after the frame: jump now, release on schedule
        GJBaseGameLayer::handleButton(true, button, isPlayer1);
        Kaze::queue.push_back({std::max(c.up, Kaze::frame + 1), false});
        Kaze::swallowed++;
    }

    void processCommands(float dt) {
        GJBaseGameLayer::processCommands(dt);
        if (!Kaze::playing) return;
        Kaze::frame++;
        for (auto it = Kaze::queue.begin(); it != Kaze::queue.end();) {
            if (it->first <= Kaze::frame) {
                Kaze::injecting = true;
                GJBaseGameLayer::handleButton(it->second, 1, true);
                Kaze::injecting = false;
                it = Kaze::queue.erase(it);
            } else ++it;
        }
    }
};
