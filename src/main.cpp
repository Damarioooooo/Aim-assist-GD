#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include "Sim.hpp"

using namespace geode::prelude;
using namespace assist;

$on_mod(Loaded) {
    loadConfig();
    listenForAllSettingChanges([](std::shared_ptr<SettingV3>) { loadConfig(); });
}

namespace {

// ------------------------------------------------------------ input state --

struct Input {
    bool inject = false;       // true while WE are calling handleButton (bypass our own hook)
    bool userHolding = false;  // what the player's finger is actually doing

    // click assist: a tap we are holding back for a few ticks
    int pendingDelay = -1;     // -1 = nothing pending
    int pendingTicks = 0;
    bool releaseQueued = false;
    int releaseIn = -1;

    // auto-orb: per player (0 = P1, 1 = P2)
    bool autoHeld[2] = {false, false};
    int autoHeldTicks[2] = {0, 0};
};
Input g_in;

void injectButton(GJBaseGameLayer* gl, bool down, bool isP1) {
    g_in.inject = true;
    gl->handleButton(down, 1, isP1);
    g_in.inject = false;
}

// Click assist: try "click now", "click 1 tick later", ... and return the first
// delay (in physics ticks) whose result survives the look-ahead window.
// If nothing survives we return 0 so the player's own timing is used.
int planClickDelay(PlayLayer* pl) {
    auto& sim = Sim::get();
    int window = sim.ticks(cfg.assistWindowMs / 1000.0);
    int horizon = std::max(1, sim.ticks(cfg.assistHorizonMs / 1000.0));
    if (window <= 0) return 0;

    Sim::Session s(pl, pl->m_player1, (window + horizon) * sim.stepDt);
    if (!s.ok()) return 0;

    for (int d = 0; d <= window; ++d) {
        if (!s.run(d, d + horizon, false).died) return d;
    }
    return 0;
}

// Runs right before every real physics tick of P1/P2.
void beforeStep(PlayLayer* pl, PlayerObject* p, bool isP1) {
    int idx = isP1 ? 0 : 1;

    // ---- deliver a delayed click ----
    if (isP1) {
        if (g_in.pendingDelay >= 0) {
            if (g_in.pendingTicks >= g_in.pendingDelay) {
                injectButton(pl, true, true);
                g_in.pendingDelay = -1;
                g_in.releaseIn = g_in.releaseQueued ? 2 : -1;
                g_in.releaseQueued = false;
            } else {
                ++g_in.pendingTicks;
            }
        } else if (g_in.releaseIn > 0) {
            --g_in.releaseIn;
        } else if (g_in.releaseIn == 0) {
            injectButton(pl, false, true);
            g_in.releaseIn = -1;
        }
    }

    // ---- auto orb ----
    if (!cfg.autoOrb) return;

    if (g_in.autoHeld[idx]) {
        ++g_in.autoHeldTicks[idx];
        bool dashing = p->m_isDashing; // keep holding through dash orbs
        if (g_in.autoHeldTicks[idx] >= 2 && !g_in.userHolding && !dashing) {
            injectButton(pl, false, isP1);
            g_in.autoHeld[idx] = false;
        }
        return;
    }

    float x = p->getPositionX();
    auto [first, last] = LevelCache::range(Sim::get().cache.rings, x - 80.f, x + 80.f);
    auto rect = p->getObjectRect();
    for (auto it = first; it != last; ++it) {
        GameObject* ring = *it;
        bool activated = isP1 ? ring->m_hasBeenActivated : ring->m_hasBeenActivatedP2;
        if (activated) continue;
        if (!rect.intersectsRect(ring->getObjectRect())) continue;

        injectButton(pl, true, isP1);
        g_in.autoHeld[idx] = true;
        g_in.autoHeldTicks[idx] = 0;
        break;
    }
}

void drawPath(CCDrawNode* n, CCPoint from, Sim::Result const& r, ccColor4F c) {
    CCPoint prev = from;
    for (auto const& p : r.pts) {
        n->drawSegment(prev, p, 0.9f, c);
        prev = p;
    }
    if (r.died) n->drawDot(prev, 3.5f, c);
}

} // namespace

// ---------------------------------------------------------------- hooks ----

class $modify(AssistBaseLayer, GJBaseGameLayer) {
    // Every tap/hold goes through here. Button 1 = jump.
    void handleButton(bool down, int button, bool isPlayer1) {
        if (button == 1 && isPlayer1 && !g_in.inject) g_in.userHolding = down;

        auto* pl = PlayLayer::get();
        bool active = !g_in.inject && button == 1 && isPlayer1 && pl &&
                      static_cast<GJBaseGameLayer*>(pl) == static_cast<GJBaseGameLayer*>(this) &&
                      cfg.clickAssist && allowed(pl) && pl->m_player1 && !pl->m_player1->m_isDead;

        if (!active) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        if (down) {
            int delay = planClickDelay(pl);
            if (delay > 0) {
                g_in.pendingDelay = delay;   // swallow the tap, beforeStep() will deliver it
                g_in.pendingTicks = 0;
                g_in.releaseQueued = false;
                return;
            }
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
        } else if (g_in.pendingDelay >= 0) {
            g_in.releaseQueued = true;       // finger lifted before the delayed tap fired
        } else {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
    }
};

class $modify(AssistPlayer, PlayerObject) {
    void update(float dt) {
        auto& sim = Sim::get();
        if (!sim.isFake(this)) {
            auto* pl = PlayLayer::get();
            if (pl && allowed(pl)) {
                bool p1 = this == pl->m_player1;
                bool p2 = this == pl->m_player2;
                if (p1 && dt > 0.f && dt < 0.02f) sim.stepDt = dt;
                if (p1 || p2) beforeStep(pl, this, p1);
            }
        }
        PlayerObject::update(dt);
    }
};

class $modify(AssistPlayLayer, PlayLayer) {
    struct Fields {
        CCDrawNode* node = nullptr;
    };

    void setupHasCompleted() {
        PlayLayer::setupHasCompleted();
        g_in = Input{};
        auto& sim = Sim::get();
        sim.reset();
        sim.cache.build(this);

        auto* node = CCDrawNode::create();
        m_objectLayer->addChild(node, 10000);
        m_fields->node = node;
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        g_in = Input{};
    }

    void onQuit() {
        Sim::get().reset();
        g_in = Input{};
        PlayLayer::onQuit();
    }

    // Deaths of the simulated player must not kill the real one.
    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        auto& sim = Sim::get();
        if (sim.isFake(player)) {
            sim.dead = true;
            return;
        }
        PlayLayer::destroyPlayer(player, obj);
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        drawPreview();
    }

    void drawPreview() {
        auto* node = m_fields->node;
        if (!node) return;
        node->clear();
        if (!cfg.preview || !allowed(this) || !m_player1 || m_player1->m_isDead) return;

        auto& sim = Sim::get();
        int horizon = std::max(2, sim.ticks(cfg.previewSeconds));
        Sim::Session s(this, m_player1, cfg.previewSeconds);
        if (!s.ok()) return;

        auto hold = s.run(0, horizon, true);      // press now and keep holding
        auto release = s.run(-1, horizon, true);  // let go now
        auto start = m_player1->getPosition();

        drawPath(node, start, hold, ccColor4F{0.25f, 1.f, 0.35f, 0.95f});
        drawPath(node, start, release, ccColor4F{1.f, 0.35f, 0.3f, 0.95f});
    }
};
