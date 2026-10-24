#pragma once
#include <Geode/Geode.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace assist {
using namespace geode::prelude;

// ---------------------------------------------------------------- config ---

struct Config {
    bool enabled = true;
    bool practiceOnly = true;
    bool preview = true;
    bool clickAssist = true;
    bool autoOrb = true;
    double previewSeconds = 0.8;
    double assistWindowMs = 40;
    double assistHorizonMs = 500;
};
inline Config cfg;

inline void loadConfig() {
    auto* m = Mod::get();
    cfg.enabled         = m->getSettingValue<bool>("enabled");
    cfg.practiceOnly    = m->getSettingValue<bool>("practice-only");
    cfg.preview         = m->getSettingValue<bool>("preview");
    cfg.clickAssist     = m->getSettingValue<bool>("click-assist");
    cfg.autoOrb         = m->getSettingValue<bool>("auto-orb");
    cfg.previewSeconds  = m->getSettingValue<double>("preview-seconds");
    cfg.assistWindowMs  = m->getSettingValue<double>("assist-window-ms");
    cfg.assistHorizonMs = m->getSettingValue<double>("assist-horizon-ms");
}

// Only active in practice mode / editor playtest unless the user opts out.
inline bool allowed(PlayLayer* pl) {
    if (!pl || !cfg.enabled) return false;
    if (cfg.practiceOnly && !pl->m_isPracticeMode && !pl->m_isTestMode) return false;
    return true;
}

// ----------------------------------------------------------- object cache --

// Orbs/rings that need a click. Extend this list if you want spider orbs etc.
inline bool isRing(GameObjectType t) {
    switch (t) {
        case GameObjectType::YellowJumpRing:
        case GameObjectType::PinkJumpRing:
        case GameObjectType::GravityRing:
        case GameObjectType::GreenRing:
        case GameObjectType::RedJumpRing:
        case GameObjectType::CustomRing:
        case GameObjectType::DashRing:
        case GameObjectType::GravityDashRing:
            return true;
        default:
            return false;
    }
}

// Objects that never carry an "activated" flag we need to protect.
inline bool isPlainStatic(GameObjectType t) {
    return t == GameObjectType::Solid || t == GameObjectType::Hazard ||
           t == GameObjectType::Decoration || t == GameObjectType::Slope;
}

struct LevelCache {
    std::vector<GameObject*> interactables; // sorted by x: rings, pads, portals, coins, triggers...
    std::vector<GameObject*> rings;         // sorted by x

    void clear() {
        interactables.clear();
        rings.clear();
    }

    void build(GJBaseGameLayer* gl) {
        clear();
        if (!gl || !gl->m_objects) return;
        for (auto* obj : CCArrayExt<GameObject*>(gl->m_objects)) {
            if (!obj) continue;
            auto t = obj->m_objectType;
            if (isRing(t)) rings.push_back(obj);
            if (!isPlainStatic(t)) interactables.push_back(obj);
        }
        auto byX = [](GameObject* a, GameObject* b) { return a->getPositionX() < b->getPositionX(); };
        std::sort(interactables.begin(), interactables.end(), byX);
        std::sort(rings.begin(), rings.end(), byX);
    }

    static auto range(std::vector<GameObject*> const& v, float lo, float hi) {
        auto first = std::lower_bound(v.begin(), v.end(), lo,
            [](GameObject* o, float x) { return o->getPositionX() < x; });
        auto last = std::upper_bound(first, v.end(), hi,
            [](float x, GameObject* o) { return x < o->getPositionX(); });
        return std::make_pair(first, last);
    }
};

// ------------------------------------------------------------- simulator ---

class Sim {
public:
    static Sim& get() {
        static Sim s;
        return s;
    }

    LevelCache cache;
    double stepDt = 1.0 / 240.0; // updated from the real player's update() calls
    bool dead = false;           // set by the PlayLayer::destroyPlayer hook for the fake player
    PlayerObject* fake = nullptr;

    bool isFake(PlayerObject* p) const { return p && p == fake; }

    int ticks(double seconds) const {
        return std::max(0, static_cast<int>(std::lround(seconds / stepDt)));
    }

    void reset() {
        cache.clear();
        if (fake) {
            fake->release();
            fake = nullptr;
        }
        dead = false;
    }

    bool ensureFake(GJBaseGameLayer* gl) {
        if (fake) return true;
        fake = PlayerObject::create(1, 1, gl, gl, false);
        if (!fake) return false;
        fake->retain();
        fake->setVisible(false);
        return true;
    }

    struct Result {
        std::vector<CCPoint> pts;
        bool died = false;
        int ticks = 0;
    };

    // One Session = one snapshot of the real player + the activation flags of
    // nearby objects. run() can be called many times; everything is restored
    // after each run and again in the destructor.
    class Session {
    public:
        Session(GJBaseGameLayer* gl, PlayerObject* real, double lookaheadSeconds)
            : m_gl(gl), m_sim(Sim::get()) {
            if (!gl || !real || !m_sim.ensureFake(gl)) return;
            m_cp = PlayerCheckpoint::create();
            if (!m_cp) return;
            m_cp->retain();
            real->saveToCheckpoint(m_cp);
            m_savedP1 = gl->m_player1;

            // Snapshot "has been activated" flags in the part of the level we may touch.
            float x = real->getPositionX();
            float hi = x + static_cast<float>(1300.0 * lookaheadSeconds) + 200.f;
            auto [first, last] = LevelCache::range(m_sim.cache.interactables, x - 150.f, hi);
            for (auto it = first; it != last; ++it) {
                m_flags.push_back({*it, (*it)->m_hasBeenActivated, (*it)->m_hasBeenActivatedP2});
            }
            m_ok = true;
        }

        ~Session() {
            restoreFlags();
            if (m_gl && m_savedP1) m_gl->m_player1 = m_savedP1;
            if (m_cp) m_cp->release();
        }

        Session(Session const&) = delete;
        Session& operator=(Session const&) = delete;

        bool ok() const { return m_ok; }

        // clickTick: tick index at which "jump" is pressed and then held.
        //            0 = press now, N = press after N ticks, -1 = never press (release now).
        Result run(int clickTick, int tickCount, bool record) {
            Result r;
            if (!m_ok) return r;

            restoreFlags();
            m_sim.fake->loadFromCheckpoint(m_cp);
            m_sim.dead = false;
            float dt = static_cast<float>(m_sim.stepDt);

            m_gl->m_player1 = m_sim.fake;
            for (int i = 0; i < tickCount; ++i) {
                if (i == clickTick) m_sim.fake->pushButton(PlayerButton::Jump);
                else if (i == 0) m_sim.fake->releaseButton(PlayerButton::Jump);

                m_sim.fake->update(dt);
                m_gl->checkCollisions(m_sim.fake, dt, false);

                if (record) r.pts.push_back(m_sim.fake->getPosition());
                if (m_sim.dead) {
                    r.died = true;
                    break;
                }
                ++r.ticks;
            }
            m_gl->m_player1 = m_savedP1;
            restoreFlags();
            return r;
        }

    private:
        struct Flags {
            GameObject* obj;
            bool a;
            bool b;
        };

        void restoreFlags() {
            for (auto& f : m_flags) {
                f.obj->m_hasBeenActivated = f.a;
                f.obj->m_hasBeenActivatedP2 = f.b;
            }
        }

        GJBaseGameLayer* m_gl = nullptr;
        Sim& m_sim;
        PlayerCheckpoint* m_cp = nullptr;
        PlayerObject* m_savedP1 = nullptr;
        std::vector<Flags> m_flags;
        bool m_ok = false;
    };

private:
    Sim() = default;
};

} // namespace assist
