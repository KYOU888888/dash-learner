// Dash Learner : un bot Geometry Dash qui apprend a finir un niveau.
//
// Principe : le bot joue le niveau avec une liste d'appuis (un etat "appuye /
// relache" par tick, 240 ticks par seconde). Quand il meurt, il garde le
// meilleur essai et modifie les appuis juste avant l'endroit de la mort. Si un
// essai va plus loin, il devient le nouveau meilleur. Plus il bloque au meme
// endroit, plus il remonte loin en arriere pour corriger. Le jeu etant
// deterministe, tout ce qui est avant la zone modifiee se rejoue a l'identique.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

#include <algorithm>
#include <fstream>
#include <random>
#include <sstream>

using namespace geode::prelude;

namespace {

// ---------------------------------------------------------------- donnees

struct Brain {
    std::string key;                 // nom du fichier de sauvegarde du niveau
    std::vector<uint8_t> best;       // meilleur plan trouve (1 = appuye)
    int bestDeath = -1;              // tick de mort du meilleur plan (-1 = jamais joue)
    bool solved = false;             // niveau fini au moins une fois
    int attempts = 0;
    int stuck = 0;                   // essais sans progres d'affilee
    float bestPercent = 0.f;
    std::vector<uint8_t> current;    // plan joue pendant cet essai
};

Brain g_brain;
std::mt19937 g_rng{std::random_device{}()};

bool g_active = false;      // le bot pilote la partie en cours
bool g_botPress = false;    // l'appui en cours vient du bot (pas du joueur)
bool g_held = false;        // etat du bouton envoye par le bot
bool g_runOver = false;     // mort ou victoire deja traitee pour cet essai
int g_delayedResetsToSkip = 0;

bool settingEnabled() { return Mod::get()->getSettingValue<bool>("enabled"); }
bool settingSafe() { return Mod::get()->getSettingValue<bool>("safe-mode"); }
bool settingOverlay() { return Mod::get()->getSettingValue<bool>("show-overlay"); }
int settingSpeed() {
    return std::clamp<int>(static_cast<int>(Mod::get()->getSettingValue<int64_t>("speed")), 1, 8);
}

void setGameSpeed(float s) {
    CCDirector::sharedDirector()->getScheduler()->setTimeScale(s);
}

int randInt(int lo, int hi) { // bornes incluses
    if (hi <= lo) return lo;
    return std::uniform_int_distribution<int>(lo, hi)(g_rng);
}

// ------------------------------------------------------------ sauvegarde

std::filesystem::path brainPath(std::string const& key) {
    return Mod::get()->getSaveDir() / (key + ".txt");
}

std::string makeKey(GJGameLevel* level, bool startPos) {
    int id = level->m_levelID.value();
    std::string key;
    if (id > 0) {
        key = "level_" + std::to_string(id);
    } else {
        key = "local_";
        for (char c : std::string(level->m_levelName)) {
            key += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
        }
    }
    if (startPos) key += "_startpos";
    return key;
}

void saveBrain() {
    if (g_brain.key.empty()) return;
    std::ofstream out(brainPath(g_brain.key), std::ios::trunc);
    if (!out) return;
    out << "dashlearner1 " << (g_brain.solved ? 1 : 0) << ' ' << g_brain.bestDeath << ' '
        << g_brain.attempts << ' ' << g_brain.bestPercent << '\n';
    // On stocke les appuis comme des intervalles [debut, fin)
    auto const& p = g_brain.best;
    for (size_t i = 0; i < p.size();) {
        if (!p[i]) { ++i; continue; }
        size_t j = i;
        while (j < p.size() && p[j]) ++j;
        out << i << ' ' << j << '\n';
        i = j;
    }
}

void loadBrain(std::string const& key) {
    g_brain = Brain{};
    g_brain.key = key;
    std::ifstream in(brainPath(key));
    if (!in) return;
    std::string magic;
    int solved = 0;
    in >> magic >> solved >> g_brain.bestDeath >> g_brain.attempts >> g_brain.bestPercent;
    if (magic != "dashlearner1") { g_brain = Brain{}; g_brain.key = key; return; }
    g_brain.solved = solved != 0;
    size_t a, b;
    while (in >> a >> b) {
        if (b <= a || b > 2'000'000) continue;
        if (g_brain.best.size() < b) g_brain.best.resize(b, 0);
        std::fill(g_brain.best.begin() + a, g_brain.best.begin() + b, 1);
    }
}

// -------------------------------------------------------------- mutation

// Fabrique un nouveau plan a partir du meilleur, en modifiant la zone
// situee juste avant la mort.
std::vector<uint8_t> mutate(std::vector<uint8_t> const& base, int death, int stuck) {
    // Taille de la fenetre (en ticks) : 1/8 s au debut, jusqu'a 3 s si on bloque.
    int window = std::min(30 + stuck * 4, 720);
    if (stuck > 150 && randInt(0, 9) == 0) window = std::min(death, window * 2);

    int start = std::max(0, death - window);
    int end = death + 30;

    std::vector<uint8_t> plan = base;
    plan.resize(end, 0); // tout ce qui suit la zone est remis a "relache"

    // Les changements d'etat (debut/fin d'appui) dans la zone
    std::vector<int> edges;
    for (int i = std::max(1, start); i < end; ++i) {
        if (plan[i] != plan[i - 1]) edges.push_back(i);
    }

    int op = randInt(0, 99);

    if (op < 35 && !edges.empty()) {
        // Decaler un appui ou un relachement de quelques ticks
        int e = edges[randInt(0, static_cast<int>(edges.size()) - 1)];
        int d = randInt(1, 10) * (randInt(0, 1) ? 1 : -1);
        if (d > 0) {
            uint8_t before = plan[e - 1];
            for (int i = e; i < std::min(end, e + d); ++i) plan[i] = before;
        } else {
            uint8_t after = plan[e];
            for (int i = std::max(start, e + d); i < e; ++i) plan[i] = after;
        }
    }
    else if (op < 60) {
        // Ajouter un appui (court pour les sauts, long pour vaisseau/onde)
        int len = randInt(0, 1) ? randInt(1, 8) : randInt(8, 60);
        int pos = randInt(start, end - 1);
        for (int i = pos; i < std::min(end, pos + len); ++i) plan[i] = 1;
    }
    else if (op < 70 && !edges.empty()) {
        // Supprimer un appui
        int e = edges[randInt(0, static_cast<int>(edges.size()) - 1)];
        int i = e;
        if (!plan[i]) { // c'est un relachement : on remonte au debut de l'appui
            i = e - 1;
            while (i > start && plan[i - 1]) --i;
        }
        while (i < end && plan[i]) plan[i++] = 0;
    }
    else {
        // Reinventer toute la zone
        bool longMode = randInt(0, 1);
        bool on = randInt(0, 1);
        for (int i = start; i < end;) {
            int len = on ? (longMode ? randInt(5, 60) : randInt(1, 8))
                         : (longMode ? randInt(3, 40) : randInt(10, 80));
            for (int k = i; k < std::min(end, i + len); ++k) plan[k] = on;
            i += len;
            on = !on;
        }
    }
    return plan;
}

void prepareNextRun() {
    if (g_brain.solved || g_brain.bestDeath < 0) {
        g_brain.current = g_brain.best;
    } else {
        g_brain.current = mutate(g_brain.best, g_brain.bestDeath, g_brain.stuck);
    }
}

// Appelee a la fin de chaque essai rate
void onAttemptFailed(int deathTick, float percent) {
    g_brain.attempts++;
    if (g_brain.solved) {
        // La solution ne passe plus (niveau modifie, trigger aleatoire...) :
        // on reprend l'apprentissage a partir de ce nouvel endroit.
        g_brain.solved = false;
        g_brain.bestDeath = -1;
    }
    if (g_brain.bestDeath < 0 || deathTick > g_brain.bestDeath) {
        g_brain.best = g_brain.current;
        if (static_cast<int>(g_brain.best.size()) > deathTick + 1) g_brain.best.resize(deathTick + 1);
        g_brain.bestDeath = deathTick;
        g_brain.stuck = 0;
        saveBrain();
    } else {
        g_brain.stuck++;
    }
    g_brain.bestPercent = std::max(g_brain.bestPercent, percent);
    if (g_brain.attempts % 50 == 0) saveBrain();
    prepareNextRun();
}

bool isPlayLayer(GJBaseGameLayer* layer) {
    auto pl = PlayLayer::get();
    return pl && static_cast<GJBaseGameLayer*>(pl) == layer;
}

} // namespace

// ------------------------------------------------- entrees et ticks du jeu

class $modify(LearnerBaseLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        // Pendant que le bot joue, on ignore les clics du joueur
        if (g_active && !g_botPress && isPlayLayer(this)) return;
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (g_active && isPlayLayer(this)) {
            int t = static_cast<int>(m_gameState.m_currentProgress);
            auto const& plan = g_brain.current;
            bool want = t >= 0 && t < static_cast<int>(plan.size()) && plan[t];
            if (want != g_held) {
                g_botPress = true;
                this->handleButton(want, 1, true);
                g_botPress = false;
                g_held = want;
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

// ------------------------------------------------------------ niveau

class $modify(LearnerPlayLayer, PlayLayer) {
    struct Fields {
        CCLabelBMFont* label = nullptr;
    };

    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        g_active = false;
        g_delayedResetsToSkip = 0;
        if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

        loadBrain(makeKey(level, m_isTestMode));
        prepareNextRun();
        g_active = settingEnabled() && !m_isPlatformer && !m_isPracticeMode;
        g_held = false;
        g_runOver = false;
        setGameSpeed(g_active ? static_cast<float>(settingSpeed()) : 1.f);

        auto win = CCDirector::sharedDirector()->getWinSize();
        auto label = CCLabelBMFont::create("", "chatFont.fnt");
        label->setAnchorPoint({0.f, 1.f});
        label->setPosition({6.f, win.height - 6.f});
        label->setScale(0.6f);
        label->setZOrder(9999);
        this->addChild(label);
        m_fields->label = label;
        this->schedule(schedule_selector(LearnerPlayLayer::updateBotLabel), 0.1f);
        this->updateBotLabel(0.f);
        return true;
    }

    void updateBotLabel(float) {
        auto label = m_fields->label;
        if (!label) return;
        label->setVisible(settingOverlay());
        if (!settingOverlay()) return;

        std::ostringstream s;
        if (!g_active) {
            if (m_isPlatformer) s << "Bot : niveaux plateforme non geres";
            else if (m_isPracticeMode) s << "Bot : desactive en practice";
            else s << "Bot : OFF";
        } else if (g_brain.solved) {
            s << "Bot : niveau fini ! Rejoue la solution (" << g_brain.attempts << " essais)";
        } else {
            s << "Bot : essai " << g_brain.attempts + 1
              << "  |  record " << static_cast<int>(g_brain.bestPercent) << "%"
              << "  |  blocage " << g_brain.stuck
              << "  |  x" << settingSpeed();
        }
        label->setString(s.str().c_str());
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        if (m_isPracticeMode) {
            // Les checkpoints cassent le rejeu depuis le debut : on se retire.
            if (g_active) setGameSpeed(1.f);
            g_active = false;
        }
        if (g_active && m_player1) m_player1->releaseAllButtons();
        g_held = false;
        g_runOver = false;
    }

    void destroyPlayer(PlayerObject* player, GameObject* object) {
        // L'objet "anticheat" est detruit au lancement : ce n'est pas une mort.
        if (!g_active || object == m_anticheatSpike) {
            PlayLayer::destroyPlayer(player, object);
            return;
        }

        bool oldTestMode = m_isTestMode;
        if (settingSafe()) m_isTestMode = true;
        PlayLayer::destroyPlayer(player, object);
        m_isTestMode = oldTestMode;

        // Mort ignoree (noclip d'un autre mod, etc.)
        if (!m_player1 || !m_player1->m_isDead || g_runOver) return;
        g_runOver = true;

        onAttemptFailed(static_cast<int>(m_gameState.m_currentProgress), this->getCurrentPercent());

        // Relance immediate plutot que d'attendre l'animation d'explosion
        g_delayedResetsToSkip++;
        Ref<PlayLayer> self = this;
        queueInMainThread([self] {
            if (g_active && PlayLayer::get() == self.data()) self->resetLevel();
        });
    }

    void delayedResetLevel() {
        if (g_active && g_delayedResetsToSkip > 0) {
            g_delayedResetsToSkip--;
            return; // on a deja relance nous-memes
        }
        PlayLayer::delayedResetLevel();
    }

    void levelComplete() {
        if (g_active && !g_runOver) {
            g_runOver = true;
            g_brain.attempts++;
            if (!g_brain.solved) {
                g_brain.solved = true;
                g_brain.best = g_brain.current;
                g_brain.bestDeath = static_cast<int>(m_gameState.m_currentProgress);
                g_brain.bestPercent = 100.f;
                g_brain.stuck = 0;
                saveBrain();
                Notification::create("Dash Learner : niveau fini en " +
                    std::to_string(g_brain.attempts) + " essais !", NotificationIcon::Success)->show();
            }
            setGameSpeed(1.f);
        }

        bool oldTestMode = m_isTestMode;
        if (g_active && settingSafe()) m_isTestMode = true;
        PlayLayer::levelComplete();
        m_isTestMode = oldTestMode;
    }

    void onQuit() {
        if (g_active) saveBrain();
        g_active = false;
        g_held = false;
        setGameSpeed(1.f);
        PlayLayer::onQuit();
    }
};

// --------------------------------------------------------- menu pause

class $modify(LearnerPauseLayer, PauseLayer) {
    struct Fields {
        CCMenuItemSpriteExtra* toggleBtn = nullptr;
    };

    void customSetup() {
        PauseLayer::customSetup();

        auto menu = CCMenu::create();
        menu->setPosition({0.f, 0.f});
        menu->setID("dash-learner-menu"_spr);
        this->addChild(menu, 10);

        auto toggleSpr = ButtonSprite::create(settingEnabled() ? "Bot: ON" : "Bot: OFF",
                                              "goldFont.fnt", "GJ_button_01.png");
        toggleSpr->setScale(0.6f);
        auto toggle = CCMenuItemSpriteExtra::create(toggleSpr, this, menu_selector(LearnerPauseLayer::onBotToggle));
        toggle->setPosition({55.f, 30.f});
        menu->addChild(toggle);
        m_fields->toggleBtn = toggle;

        auto resetSpr = ButtonSprite::create("Oublier", "goldFont.fnt", "GJ_button_06.png");
        resetSpr->setScale(0.6f);
        auto reset = CCMenuItemSpriteExtra::create(resetSpr, this, menu_selector(LearnerPauseLayer::onBotForget));
        reset->setPosition({55.f, 62.f});
        menu->addChild(reset);
    }

    void onBotToggle(CCObject*) {
        bool on = !settingEnabled();
        Mod::get()->setSettingValue<bool>("enabled", on);
        if (auto btn = m_fields->toggleBtn) {
            if (auto spr = typeinfo_cast<ButtonSprite*>(btn->getNormalImage())) {
                spr->setString(on ? "Bot: ON" : "Bot: OFF");
            }
        }
        if (!on) {
            g_active = false;
            g_held = false;
            setGameSpeed(1.f);
        }
        Notification::create(on ? "Bot active : il prendra la main au prochain lancement du niveau"
                                : "Bot desactive", NotificationIcon::Info)->show();
    }

    void onBotForget(CCObject*) {
        if (g_brain.key.empty()) return;
        std::error_code ec;
        std::filesystem::remove(brainPath(g_brain.key), ec);
        std::string key = g_brain.key;
        loadBrain(key);
        prepareNextRun();
        Notification::create("Apprentissage efface pour ce niveau", NotificationIcon::Info)->show();
    }
};
