// Lobby page (MATCH_ROUNDS_UI_CHAT_PLAN 3.4): the state pill and badge, the browser rows, the players, the mode cards,
// which setting rows each format shows with their values and hints, host against member, the steppers wrapping both
// ways, the members' Ready toggle, and the map cards with the selected and the refused ones.
#include "match/hsm_lobby_scene.h"
#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;
using namespace hsm::style;

static bool Near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }
static bool Same(Rgba a, Rgba b) { return Near(a.r, b.r) && Near(a.g, b.g) && Near(a.b, b.b) && Near(a.a, b.a); }

static const std::string kDot = "\xC2\xB7";

static LobbyPlayer Player(const char* name, bool host = false, bool me = false, int team = kNoTeam, int over = kNoTeam) {
    LobbyPlayer p; p.name = name; p.host = host; p.me = me; p.team = team; p.hostOverride = over;
    return p;
}
static size_t PickableCount() { size_t n = 0; for (size_t i = 0; i < MapCount(); ++i) if (kMaps[i].pick) ++n; return n; }

static LobbyLocal InLobby(bool host, Format f = Format::FreeForAll, Gear g = Gear::Own) {
    LobbyLocal l;
    l.phase = LobbyPhase::InLobby; l.host = host; l.version = "0.2.0"; l.myName = host ? "Ann" : "Bob";
    l.config.format = f; l.config.gear = g; l.config.scoreLimit = DefaultScoreLimit(f);
    l.players = {Player("Ann", true, host), Player("Bob", false, !host)};
    return l;
}
static std::vector<LobbyRow> Ids(const LobbyScene& s) { std::vector<LobbyRow> v; for (const SettingRow& r : s.settings) v.push_back(r.id); return v; }
static const SettingRow* RowOf(const LobbyScene& s, LobbyRow id) { for (const SettingRow& r : s.settings) if (r.id == id) return &r; return nullptr; }
static const SettingRow& Need(const LobbyScene& s, LobbyRow id) {
    static const SettingRow kNone{};
    const SettingRow* r = RowOf(s, id);
    CHECK(r != nullptr);
    return r ? *r : kNone;
}

static void TestPrevNext() {
    const std::vector<int> o = {3, 5, 10};
    CHECK(NextOf(o, 3) == 5 && NextOf(o, 10) == 3 && NextOf(o, 7) == 3);
    CHECK(PrevOf(o, 5) == 3 && PrevOf(o, 10) == 5);
    CHECK(PrevOf(o, 3) == 10);                           // wraps back to the last
    CHECK(PrevOf(o, 7) == 10);                           // unknown: the last (NextOf: the first)
    CHECK(PrevOf({}, 7) == 7 && NextOf({}, 7) == 7);
    CHECK(StepOf(o, 5, 1) == 10 && StepOf(o, 5, -1) == 3 && StepOf(o, 5, 0) == 10);
}

static void TestNewRows() {
    MatchConfig b; b.format = Format::PlayersVsBots;
    CHECK(WaveBreakShown(b) == 60 && StepSetting(b, LobbyRow::WaveBreak, 1).waveBreakS == 120 && StepSetting(b, LobbyRow::WaveBreak, -1).waveBreakS == 30);
    MatchConfig z; z.format = Format::Zombies;
    CHECK(WaveBreakShown(z) == 10 && StepSetting(z, LobbyRow::WaveBreak, 1).waveBreakS == 30 && StepSetting(z, LobbyRow::WaveBreak, -1).waveBreakS == 120);
    MatchConfig f;
    CHECK(StepSetting(f, LobbyRow::ClearGround, 1).clearGroundMin == 2 && StepSetting(f, LobbyRow::ClearGround, -1).clearGroundMin == 5);
    f.clearGroundMin = 5; CHECK(StepSetting(f, LobbyRow::ClearGround, 1).clearGroundMin == 0);
    auto has = [](const MatchConfig& c, LobbyRow r) { for (LobbyRow x : SettingsFor(c)) if (x == r) return true; return false; };
    CHECK(has(b, LobbyRow::WaveBreak) && has(z, LobbyRow::WaveBreak) && !has(b, LobbyRow::ClearGround));
    MatchConfig d; d.format = Format::Duel;
    CHECK(!has(d, LobbyRow::ClearGround) && !has(d, LobbyRow::WaveBreak));
    CHECK(has(MatchConfig{}, LobbyRow::ClearGround));
    MatchConfig br; br.gear = Gear::Fists; CHECK(has(br, LobbyRow::ClearGround) && !has(br, LobbyRow::Tier) && !has(br, LobbyRow::Gear));   // Brawl: fixed strength, no gear
    MatchConfig t; t.format = Format::TwoTeams; CHECK(has(t, LobbyRow::ClearGround));
}

static void TestStepSetting() {
    MatchConfig c;                                       // free-for-all, limit 5
    CHECK(StepSetting(c, LobbyRow::Limit, 1).scoreLimit == 10);
    CHECK(StepSetting(c, LobbyRow::Limit, -1).scoreLimit == 3);
    c.scoreLimit = 50;
    CHECK(StepSetting(c, LobbyRow::Limit, 1).scoreLimit == 3);
    c.scoreLimit = 3;
    CHECK(StepSetting(c, LobbyRow::Limit, -1).scoreLimit == 50);
    CHECK((LimitOptions(Format::TwoTeams) == std::vector<int>{3, 5, 10, 15, 20, 30, 50}));
    MatchConfig d; d.format = Format::Duel; d.scoreLimit = 3;
    CHECK((LimitOptions(Format::Duel) == std::vector<int>{1, 2, 3, 5, 7, 10}));
    CHECK(StepSetting(d, LobbyRow::Limit, 1).scoreLimit == 5 && StepSetting(d, LobbyRow::Limit, -1).scoreLimit == 2);
    d.scoreLimit = 1;
    CHECK(StepSetting(d, LobbyRow::Limit, -1).scoreLimit == 10);
    d.scoreLimit = 10;
    CHECK(StepSetting(d, LobbyRow::Limit, 1).scoreLimit == 1);

    MatchConfig t;
    CHECK(StepSetting(t, LobbyRow::Time, 1).timeLimitS == 300 && StepSetting(t, LobbyRow::Time, -1).timeLimitS == 1200);
    t.timeLimitS = 1200;
    CHECK(StepSetting(t, LobbyRow::Time, 1).timeLimitS == 0 && StepSetting(t, LobbyRow::Time, -1).timeLimitS == 900);

    MatchConfig r;                                       // respawn 5
    CHECK((kRespawnOptions == std::vector<int>{3, 5, 10, 15, 20}));
    CHECK(StepSetting(r, LobbyRow::Respawn, 1).respawnDelayS == 10 && StepSetting(r, LobbyRow::Respawn, -1).respawnDelayS == 3);
    r.respawnDelayS = 3;
    CHECK(StepSetting(r, LobbyRow::Respawn, -1).respawnDelayS == 20);
    r.respawnDelayS = 20;
    CHECK(StepSetting(r, LobbyRow::Respawn, 1).respawnDelayS == 3);

    MatchConfig g;                                       // own gear
    CHECK(StepSetting(g, LobbyRow::Gear, 1).gear == Gear::RandomTier && StepSetting(g, LobbyRow::Gear, -1).gear == Gear::Mirror);
    g.gear = Gear::Mirror;
    CHECK(StepSetting(g, LobbyRow::Gear, 1).gear == Gear::Own && StepSetting(g, LobbyRow::Gear, -1).gear == Gear::RandomTier);
    g.gear = Gear(7);                                    // out of range from the lobby: steps as if own
    CHECK(StepSetting(g, LobbyRow::Gear, 1).gear == Gear::RandomTier);

    MatchConfig k;                                       // tier 2
    CHECK(StepSetting(k, LobbyRow::Tier, 1).tier == 3 && StepSetting(k, LobbyRow::Tier, -1).tier == 1);
    k.tier = 7;
    CHECK(StepSetting(k, LobbyRow::Tier, 1).tier == 0 && StepSetting(k, LobbyRow::Tier, -1).tier == 6);
    k.tier = 0;
    CHECK(StepSetting(k, LobbyRow::Tier, -1).tier == 7);
    k.tier = 9;
    CHECK(StepSetting(k, LobbyRow::Tier, 1).tier == 0);

    MatchConfig f;
    CHECK(StepSetting(f, LobbyRow::FriendlyFire, 1).friendlyFire && StepSetting(f, LobbyRow::FriendlyFire, -1).friendlyFire);
    f.friendlyFire = true;
    CHECK(!StepSetting(f, LobbyRow::FriendlyFire, 1).friendlyFire);

    MatchConfig m; m.scoreLimit = 7; m.tier = 4; m.gear = Gear::Mirror; m.timeLimitS = 600; m.respawnDelayS = 15;
    MatchConfig same = StepSetting(m, LobbyRow::MyTeam, 1);
    CHECK(same.scoreLimit == 7 && same.tier == 4 && same.gear == Gear::Mirror && same.timeLimitS == 600 && same.respawnDelayS == 15 && !same.friendlyFire);
    MatchConfig only = StepSetting(m, LobbyRow::Tier, 1);  // a step changes its own setting alone
    CHECK(only.tier == 5 && only.scoreLimit == 7 && only.gear == Gear::Mirror && only.timeLimitS == 600 && only.respawnDelayS == 15);
}

static void TestTeamPick() {
    CHECK(StepTeamPick(kNoTeam, 2, 1) == 0 && StepTeamPick(0, 2, 1) == 1 && StepTeamPick(1, 2, 1) == kNoTeam);
    CHECK(StepTeamPick(kNoTeam, 2, -1) == 1 && StepTeamPick(1, 2, -1) == 0 && StepTeamPick(0, 2, -1) == kNoTeam);
    CHECK(StepTeamPick(kNoTeam, 3, -1) == 2 && StepTeamPick(2, 3, -1) == 1 && StepTeamPick(2, 3, 1) == kNoTeam);
    CHECK(StepTeamPick(2, 2, -1) == 1 && StepTeamPick(2, 2, 1) == kNoTeam);   // a pick the format lacks is Auto
    CHECK(StepTeamPick(5, 2, -1) == 1 && StepTeamPick(5, 3, -1) == 2);
    CHECK(StepTeamPick(kNoTeam, 0, -1) == kNoTeam && StepTeamPick(kNoTeam, 0, 1) == kNoTeam);
}

static void TestSelectFormat() {
    MatchConfig c; c.scoreLimit = 20; c.timeLimitS = 600; c.gear = Gear::Mirror; c.tier = 5; c.respawnDelayS = 10; c.friendlyFire = true;
    MatchConfig d = SelectFormat(c, Format::Duel);
    CHECK(d.format == Format::Duel && d.scoreLimit == 3);
    CHECK(d.timeLimitS == 600 && d.gear == Gear::Mirror && d.tier == 5 && d.respawnDelayS == 10 && d.friendlyFire);
    CHECK(SelectFormat(c, Format::TwoTeams).scoreLimit == 10);
    MatchConfig same = SelectFormat(c, Format::FreeForAll);
    CHECK(same.format == Format::FreeForAll && same.scoreLimit == 20);   // the card already chosen: nothing changes
}

static void TestPill() {
    LobbyLocal l; l.version = "0.2.0";
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.pill.text == "Steam not ready" && Same(s.pill.color, kWarn));
    CHECK(!s.idle && !s.in);
    l.phase = LobbyPhase::Idle;
    s = BuildLobbyScene(l);
    CHECK(s.pill.text == "Not in a lobby" && Same(s.pill.color, kIron) && s.idle && !s.in);
    l.phase = LobbyPhase::Creating;
    s = BuildLobbyScene(l);
    CHECK(s.pill.text == "Creating a lobby" && Same(s.pill.color, kWarn) && !s.idle && !s.in);
    l.phase = LobbyPhase::Joining;
    s = BuildLobbyScene(l);
    CHECK(s.pill.text == "Joining a lobby" && Same(s.pill.color, kWarn) && !s.idle && !s.in);

    LobbyLocal h = InLobby(true);
    s = BuildLobbyScene(h);
    CHECK(s.pill.text == "Hosting " + kDot + " friends" && Same(s.pill.color, kOk) && s.in && !s.idle);
    h.publicLobby = true;
    CHECK(BuildLobbyScene(h).pill.text == "Hosting " + kDot + " public");
    LobbyLocal m = InLobby(false);
    s = BuildLobbyScene(m);
    CHECK(s.pill.text == "In Ann's lobby" && Same(s.pill.color, kOk));
    m.players = {Player("Bob", false, true)};             // the owner not in the list yet
    CHECK(BuildLobbyScene(m).pill.text == "In a lobby");
    m.players = {Player("", true), Player("Bob", false, true)};
    CHECK(BuildLobbyScene(m).pill.text == "In a lobby");
}

static void TestHeader() {
    LobbyLocal l; l.phase = LobbyPhase::Idle; l.version = "0.2.0"; l.myName = "Ann"; l.status = "3 public lobbies found";
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.me == "Ann");
    CHECK(s.badge == "v0.2.0 " + kDot + " protocol " + std::to_string(int(kProtocolRevision)));
    CHECK(s.status == "3 public lobbies found");
    l.version = "1.4.2";
    CHECK(BuildLobbyScene(l).badge == "v1.4.2 " + kDot + " protocol " + std::to_string(int(kProtocolRevision)));
    l.phase = LobbyPhase::Joining; l.status = "Joining lobby...";
    CHECK(BuildLobbyScene(l).status == "Joining lobby...");
    LobbyLocal in = InLobby(true);
    in.status = "Hosting - invite friends, then start any fight";
    CHECK(BuildLobbyScene(in).status.empty());            // in a lobby the pill says it
}

static void TestBrowser() {
    LobbyLocal l; l.phase = LobbyPhase::Idle; l.version = "0.2.0";
    LobbyFound ok; ok.name = "Ann's fight"; ok.map = "Yard"; ok.mode = "Duel"; ok.players = 2; ok.compatible = true;
    LobbyFound old; old.name = "Old"; old.players = 1; old.compatible = false; old.reason = "different mod version (host 0.1.0, you 0.2.0)";
    LobbyFound bare; bare.players = 3; bare.compatible = false;
    LobbyFound full; full.name = "Full"; full.map = "Pit"; full.players = 8; full.compatible = true;
    LobbyFound modeOnly; modeOnly.name = "M"; modeOnly.mode = "Zombies"; modeOnly.players = 5; modeOnly.compatible = true;
    l.found = {ok, old, bare, full, modeOnly};
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 5);
    if (s.browser.size() == 5) {
        CHECK(s.browser[0].name == "Ann's fight" && s.browser[0].players == "2/8" && s.browser[0].detail == "Yard " + kDot + " Duel");
        CHECK(s.browser[0].joinable && s.browser[0].note.text == "Join" && Same(s.browser[0].note.color, kOk));
        CHECK(!s.browser[1].joinable && s.browser[1].note.text == old.reason && Same(s.browser[1].note.color, kWarn));
        CHECK(s.browser[1].detail.empty() && s.browser[1].players == "1/8");
        CHECK(s.browser[2].name == "Unnamed lobby" && s.browser[2].note.text == "Incompatible" && !s.browser[2].joinable);
        CHECK(!s.browser[3].joinable && s.browser[3].note.text == "Full" && Same(s.browser[3].note.color, kWarn) && s.browser[3].detail == "Pit");
        CHECK(s.browser[4].joinable && s.browser[4].detail == "Zombies" && s.browser[4].players == "5/8");
    }
    l.found.assign(8, ok);
    CHECK(BuildLobbyScene(l).browser.size() == kBrowserRows && kBrowserRows == 6);
    l.maxPlayers = 2;
    s = BuildLobbyScene(l);
    CHECK(!s.browser.empty() && !s.browser[0].joinable && s.browser[0].players == "2/2");
    l.phase = LobbyPhase::Joining;
    CHECK(BuildLobbyScene(l).browser.empty());
    LobbyLocal in = InLobby(true);
    in.found = {ok};
    CHECK(BuildLobbyScene(in).browser.empty());           // the browser is for finding a lobby
}

static void TestOutOfLobby() {
    LobbyLocal l; l.phase = LobbyPhase::Idle; l.version = "0.2.0"; l.players = {Player("Ann", true, true)};
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.roster.empty() && s.modes.empty() && s.settings.empty() && s.maps.empty());
    CHECK(!s.start && s.waiting.empty() && s.hostOnly.empty() && !s.balance && s.playersTitle.empty());
}

static void TestHostAgainstMember() {
    LobbyScene h = BuildLobbyScene(InLobby(true, Format::TwoTeams, Gear::RandomTier));
    CHECK(h.start && h.waiting.empty() && h.hostOnly.empty() && h.balance);
    for (const ModeCard& c : h.modes) CHECK(c.enabled);
    for (const SettingRow& r : h.settings) CHECK(r.enabled);
    for (const MapCard& m : h.maps) CHECK(m.enabled == m.allowed);
    for (const RosterRow& r : h.roster) CHECK(r.movable);

    LobbyScene m = BuildLobbyScene(InLobby(false, Format::TwoTeams, Gear::RandomTier));
    CHECK(!m.start && m.waiting == "Waiting for the host" && m.hostOnly == "Only the host can change these." && !m.balance);
    CHECK(m.modes.size() == kModeCardCount && m.settings.size() == h.settings.size() && m.maps.size() == PickableCount());
    for (const ModeCard& c : m.modes) CHECK(!c.enabled);
    for (const SettingRow& r : m.settings) CHECK(r.enabled == (r.id == LobbyRow::MyTeam));   // their own team only
    for (const MapCard& c : m.maps) CHECK(!c.enabled);
    for (const RosterRow& r : m.roster) CHECK(!r.movable);

    CHECK(!BuildLobbyScene(InLobby(true, Format::FreeForAll)).balance);
    CHECK(!BuildLobbyScene(InLobby(true, Format::PlayersVsBots)).balance);
    CHECK(BuildLobbyScene(InLobby(true, Format::ThreeTeams)).balance);
}

static void TestRowsPerFormat() {
    using V = std::vector<LobbyRow>;
    using enum LobbyRow;
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::FreeForAll))) == (V{Limit, Time, Respawn, Gear, Tier, Damage, ClearGround, Sync}));   // 0.8.0-pre20: the tier is strength, own gear too
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::FreeForAll, Gear::RandomTier))) == (V{Limit, Time, Respawn, Gear, Tier, Damage, ClearGround, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::FreeForAll, Gear::Mirror))) == (V{Limit, Time, Respawn, Gear, Tier, Damage, ClearGround, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::TwoTeams))) == (V{Limit, Time, Respawn, Gear, Tier, FriendlyFire, Damage, ClearGround, MyTeam, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::ThreeTeams, Gear::Mirror))) == (V{Limit, Time, Respawn, Gear, Tier, FriendlyFire, Damage, ClearGround, MyTeam, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(false, Format::ThreeTeams))) == (V{Limit, Time, Respawn, Gear, Tier, FriendlyFire, Damage, ClearGround, MyTeam, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::Duel))) == (V{Limit, Time, Respawn, Gear, Tier, Damage, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::Duel, Gear::RandomTier))) == (V{Limit, Time, Respawn, Gear, Tier, Damage, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::PlayersVsBots))) == (V{Time, Gear, Tier, WaveBreak, FriendlyFire, Damage, Sync}));
    CHECK(Ids(BuildLobbyScene(InLobby(true, Format::Zombies, Gear::RandomTier))) == (V{Time, Gear, Tier, WaveBreak, FriendlyFire, Damage, Sync}));   // 0.8.0-pre20: the bot formats take a gear setting
}

static void TestLabelsAndValues() {
    LobbyLocal l = InLobby(true, Format::FreeForAll, Gear::RandomTier);
    l.config.scoreLimit = 15; l.config.tier = 3; l.config.respawnDelayS = 10;
    LobbyScene s = BuildLobbyScene(l);
    CHECK(Need(s, LobbyRow::Limit).label == "Score limit" && Need(s, LobbyRow::Limit).value == "15");
    CHECK(Need(s, LobbyRow::Time).label == "Time limit" && Need(s, LobbyRow::Time).value == "off");
    CHECK(Need(s, LobbyRow::Respawn).label == "Respawn delay" && Need(s, LobbyRow::Respawn).value == "10 s");
    CHECK(Need(s, LobbyRow::Gear).label == "Gear" && Need(s, LobbyRow::Gear).value == "Random by tier");
    CHECK(Need(s, LobbyRow::Tier).label == "Gear tier" && Need(s, LobbyRow::Tier).value == "tier 3 " + kDot + " budget 100");
    l.config.timeLimitS = 600; l.config.gear = Gear::Mirror; l.config.tier = 7;
    s = BuildLobbyScene(l);
    CHECK(Need(s, LobbyRow::Time).value == "10 min" && Need(s, LobbyRow::Gear).value == "Everyone identical");
    CHECK(Need(s, LobbyRow::Tier).value == "tier 7 " + kDot + " budget 888");
    l.config.gear = Gear::Own;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Gear).value == "Own loadouts");
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Tier).value == "tier 7");                       // own gear: a strength, no gear budget
    // 0.9.0: the sync row, per player (owner sync) by default; a step flips it either way.
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Sync).label == "Sync" && Need(BuildLobbyScene(l), LobbyRow::Sync).value == "Per player");
    CHECK(StepSetting(l.config, LobbyRow::Sync, 1).hostPhysics && StepSetting(l.config, LobbyRow::Sync, -1).hostPhysics);
    l.config.hostPhysics = true;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Sync).value == "Host physics" && !StepSetting(l.config, LobbyRow::Sync, 1).hostPhysics);
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Sync).hint.find("one ping late") != std::string::npos);
    l.config.hostPhysics = false;

    LobbyLocal d = InLobby(true, Format::Duel);
    d.config.respawnDelayS = 3;
    s = BuildLobbyScene(d);
    CHECK(Need(s, LobbyRow::Limit).label == "Rounds to win" && Need(s, LobbyRow::Limit).value == "3");
    CHECK(Need(s, LobbyRow::Respawn).label == "Round break" && Need(s, LobbyRow::Respawn).value == "3 s");

    LobbyLocal b = InLobby(true, Format::PlayersVsBots);
    b.config.tier = 4;
    s = BuildLobbyScene(b);
    CHECK(Need(s, LobbyRow::Tier).label == "Starting tier" && Need(s, LobbyRow::Tier).value == "tier 4");
    CHECK(Need(s, LobbyRow::FriendlyFire).label == "Friendly fire" && Need(s, LobbyRow::FriendlyFire).value == "Off");
    b.config.friendlyFire = true;
    CHECK(Need(BuildLobbyScene(b), LobbyRow::FriendlyFire).value == "On");

    CHECK(TierBudget(0) == 0 && TierBudget(1) == 15 && TierBudget(2) == 50 && TierBudget(3) == 100);
    CHECK(TierBudget(4) == 250 && TierBudget(5) == 444 && TierBudget(6) == 666 && TierBudget(7) == 888);
    CHECK(TierBudget(-1) == 0 && TierBudget(9) == 888);
}

static std::string GearHint(Format f, Gear g) { return Need(BuildLobbyScene(InLobby(true, f, g)), LobbyRow::Gear).hint; }

static void TestGearHint() {
    for (Format f : {Format::FreeForAll, Format::TwoTeams, Format::ThreeTeams, Format::Duel})
        CHECK(GearHint(f, Gear::Own) == "Everyone fights in their own loadout.");
    for (Format f : {Format::FreeForAll, Format::TwoTeams, Format::ThreeTeams}) {
        CHECK(GearHint(f, Gear::RandomTier) == "Fresh random gear for each fighter every life.");
        CHECK(GearHint(f, Gear::Mirror) == "One random loadout for everyone, until the rematch.");   // RegearFor: Mirror rolls at a rematch
    }
    CHECK(GearHint(Format::Duel, Gear::RandomTier) == "Fresh random gear for each fighter every round.");
    CHECK(GearHint(Format::Duel, Gear::Mirror) == "One random loadout for everyone, new each round.");
    // Survival and Zombies (0.8.0-pre20): the same three choices; the loadout is rolled at the start and kept for the run.
    for (Format f : {Format::PlayersVsBots, Format::Zombies}) {
        CHECK(GearHint(f, Gear::Own) == "Everyone fights in their own loadout.");
        CHECK(GearHint(f, Gear::RandomTier) == "A random loadout for each player at the start, kept for the whole run.");
        CHECK(GearHint(f, Gear::Mirror) == "One random loadout for everyone, kept for the whole run.");
        CHECK(Need(BuildLobbyScene(InLobby(true, f, Gear::Mirror)), LobbyRow::Gear).value == "Everyone identical");
    }
}

static void TestOtherHints() {
    auto hint = [](Format f, LobbyRow id, auto tweak) { LobbyLocal l = InLobby(true, f, Gear::RandomTier); tweak(l.config); return Need(BuildLobbyScene(l), id).hint; };
    auto none = [](MatchConfig&) {};
    auto clock = [](MatchConfig& c) { c.timeLimitS = 300; };
    auto ff = [](MatchConfig& c) { c.friendlyFire = true; };
    CHECK(hint(Format::FreeForAll, LobbyRow::Limit, none) == "The first to this many kills wins.");
    CHECK(hint(Format::TwoTeams, LobbyRow::Limit, none) == "The first team to this many kills wins.");
    CHECK(hint(Format::ThreeTeams, LobbyRow::Limit, none) == "The first team to this many kills wins.");
    CHECK(hint(Format::Duel, LobbyRow::Limit, none) == "The first to win this many rounds takes the match.");
    CHECK(hint(Format::FreeForAll, LobbyRow::Time, none) == "No clock: play to the limit.");
    CHECK(hint(Format::Duel, LobbyRow::Time, clock) == "When it runs out the leader wins; a tie goes to sudden death.");
    CHECK(hint(Format::Zombies, LobbyRow::Time, none) == "No clock: hold out as long as you can.");
    CHECK(hint(Format::PlayersVsBots, LobbyRow::Time, clock) == "Players win if they hold out to the end (one wave cleared).");
    CHECK(hint(Format::Duel, LobbyRow::Respawn, none) == "The pause between the end of a round and the next.");
    CHECK(hint(Format::TwoTeams, LobbyRow::Respawn, none) == "How long the fallen wait to come back.");
    CHECK(hint(Format::FreeForAll, LobbyRow::Respawn, none) == "How long the fallen wait to come back.");
    CHECK(hint(Format::Zombies, LobbyRow::Tier, none) == "Zombies start at this rank and rise one every 3 waves. Also every player's strength and the random gear's budget.");
    CHECK(hint(Format::PlayersVsBots, LobbyRow::Tier, none) == "Bots start at this rank and rise one every 2 waves. Also every player's strength and the random gear's budget.");
    CHECK(hint(Format::Duel, LobbyRow::Tier, none) == "Sets every player's strength; the random gear is rolled within this budget.");
    auto own = [](MatchConfig& c) { c.gear = Gear::Own; };
    CHECK(hint(Format::Duel, LobbyRow::Tier, own) == "Sets every player's strength (muscle and grip).");
    CHECK(hint(Format::PlayersVsBots, LobbyRow::Tier, own) == "Bots start at this rank and rise one every 2 waves. Also every player's strength.");
    CHECK(hint(Format::TwoTeams, LobbyRow::FriendlyFire, none) == "Teammates cannot hurt each other.");
    CHECK(hint(Format::ThreeTeams, LobbyRow::FriendlyFire, ff) == "Hits on teammates count.");
    CHECK(hint(Format::PlayersVsBots, LobbyRow::FriendlyFire, none) == "Players cannot hurt each other.");
    CHECK(hint(Format::Zombies, LobbyRow::FriendlyFire, ff) == "Players can hurt each other.");
}

static void TestMyTeam() {
    LobbyLocal l = InLobby(false, Format::TwoTeams);
    LobbyScene s = BuildLobbyScene(l);
    CHECK(Need(s, LobbyRow::MyTeam).label == "My team" && Need(s, LobbyRow::MyTeam).value == "Auto");
    CHECK(Need(s, LobbyRow::MyTeam).hint == "Auto puts you on the smallest team.");
    l.players[1].team = 1;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).value == "Blue");
    l.players[1].team = 2;                                   // Green, picked in three teams: Auto here
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).value == "Auto");
    l.config.format = Format::ThreeTeams;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).value == "Green");
    l.players[1].hostOverride = 0;
    s = BuildLobbyScene(l);
    CHECK(Need(s, LobbyRow::MyTeam).value == "Green" && Need(s, LobbyRow::MyTeam).hint == "The host moved you to Red.");
    l.players[1].hostOverride = 3;                           // no fourth team: no move
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).hint == "Auto puts you on the smallest team.");
    l.players[1].me = false;                                 // not in the list yet
    CHECK(Need(BuildLobbyScene(l), LobbyRow::MyTeam).value == "Auto");
}

static void TestRoster() {
    LobbyLocal l = InLobby(true, Format::TwoTeams);
    LobbyPlayer cy = Player("Cy", false, false, 0, 1); cy.ready = true;
    l.players = {Player("Ann", true, true, 1), Player("", false, false, 2), cy};
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.playersTitle == "PLAYERS 3/8" && s.roster.size() == 3);
    if (s.roster.size() == 3) {
        const RosterRow& a = s.roster[0];
        CHECK(a.name == "Ann" && a.host && a.me && a.team == "Blue" && Same(a.swatch, TeamColor(1)) && !a.moved && !a.ready);
        const RosterRow& b = s.roster[1];
        CHECK(b.name == "?" && !b.host && !b.me && b.team == "Auto" && Near(b.swatch.a, 0) && !b.moved);
        const RosterRow& c = s.roster[2];
        CHECK(c.team == "Blue" && c.moved && c.ready && Same(c.swatch, TeamColor(1)));   // the host's move wins
    }
    l.players[2].hostOverride = 2;                           // a move to a team this format lacks: their pick stands
    s = BuildLobbyScene(l);
    if (s.roster.size() == 3) CHECK(s.roster[2].team == "Red" && !s.roster[2].moved && Same(s.roster[2].swatch, TeamColor(0)));

    LobbyScene member = BuildLobbyScene(InLobby(false, Format::TwoTeams));   // the host is someone else
    if (member.roster.size() == 2) CHECK(member.roster[0].host && !member.roster[0].me && !member.roster[1].host && member.roster[1].me);

    LobbyLocal f = InLobby(true, Format::FreeForAll);
    f.players = {Player("Ann", true, true, 1, 0)};
    s = BuildLobbyScene(f);
    if (s.roster.size() == 1) CHECK(s.roster[0].team.empty() && Near(s.roster[0].swatch.a, 0) && !s.roster[0].moved && !s.roster[0].movable);
    f.maxPlayers = 4;
    CHECK(BuildLobbyScene(f).playersTitle == "PLAYERS 1/4");
    LobbyLocal z = InLobby(true, Format::Zombies);
    z.players = {Player("Ann", true, true, 1, 1)};
    s = BuildLobbyScene(z);
    if (s.roster.size() == 1) CHECK(s.roster[0].team.empty() && !s.roster[0].moved && !s.roster[0].movable);
}

// A member marks themselves ready (their row's dot); the toggle reads and frames their own ready only. The host starts
// the match instead.
static void TestReady() {
    LobbyLocal l = InLobby(false);                           // Bob, a member
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.ready && !s.readyOn && s.readyLabel == "Mark me ready");
    l.players[1].ready = true;
    s = BuildLobbyScene(l);
    CHECK(s.ready && s.readyOn && s.readyLabel == "Ready " + kDot + " click to undo");
    if (s.roster.size() == 2) CHECK(s.roster[1].me && s.roster[1].ready && !s.roster[0].ready);
    l.players[0].ready = true; l.players[1].ready = false;   // the host's ready is not ours
    s = BuildLobbyScene(l);
    CHECK(s.ready && !s.readyOn && s.readyLabel == "Mark me ready");
    l.players = {Player("Ann", true)};                       // our own row not listed yet
    s = BuildLobbyScene(l);
    CHECK(s.ready && !s.readyOn && s.readyLabel == "Mark me ready");

    LobbyLocal h = InLobby(true);
    h.players[0].ready = true;
    s = BuildLobbyScene(h);
    CHECK(!s.ready && !s.readyOn && s.readyLabel.empty() && s.start);

    for (LobbyPhase p : {LobbyPhase::Offline, LobbyPhase::Idle, LobbyPhase::Creating, LobbyPhase::Joining}) {
        LobbyLocal o = InLobby(false);
        o.phase = p; o.players[1].ready = true;
        s = BuildLobbyScene(o);
        CHECK(!s.ready && !s.readyOn && s.readyLabel.empty());
    }
}

static void TestModes() {
    LobbyScene s = BuildLobbyScene(InLobby(true, Format::Duel));
    CHECK(s.modes.size() == kModeCardCount);
    const Format order[] = {Format::FreeForAll, Format::Duel, Format::TwoTeams, Format::ThreeTeams, Format::PlayersVsBots, Format::Zombies};
    const char* names[] = {"FREE-FOR-ALL", "DUEL", "2 TEAMS", "3 TEAMS", "SURVIVAL", "ZOMBIES"};
    for (size_t i = 0; i < s.modes.size() && i < kFormatCount; ++i) {
        CHECK(s.modes[i].format == order[i] && s.modes[i].name == names[i] && !s.modes[i].description.empty());
        CHECK(s.modes[i].selected == (order[i] == Format::Duel));
    }
    if (s.modes.size() > 1)
        CHECK(s.modes[1].description == "Rounds to the last one standing. Everybody respawns together; new gear each round with random or identical gear.");
    CHECK(std::string(FormatBlurb(Format::FreeForAll)) == "Everyone for themselves. Every kill scores; the first to the limit wins.");
    CHECK(std::string(FormatBlurb(Format::TwoTeams)) == "Red against Blue. Every kill scores for the team; the first team to the limit wins.");
    CHECK(std::string(FormatBlurb(Format::ThreeTeams)) == "Red, Blue and Green. Every kill scores for the team; the first team to the limit wins.");
    CHECK(std::string(FormatBlurb(Format::PlayersVsBots)) == "Everyone against waves of bots that rank up. The fallen return when a wave is cleared.");
    CHECK(std::string(FormatBlurb(Format::Zombies)) == "Zombie waves in the Abyss, one zombie at a time per player. The fallen return when a wave is cleared.");
}

// The Brawl card: free-for-all with bare fists. A mode card (format free-for-all, gear Fists), not a format; no Gear or
// Tier row; picking it and leaving it restore the settings.
static void TestBrawl() {
    LobbyScene s = BuildLobbyScene(InLobby(true, Format::FreeForAll, Gear::Fists));
    CHECK(s.modes.size() == kModeCardCount && kModeCardCount == size_t(kFormatCount) + 2);
    const ModeCard& card = s.modes[s.modes.size() - 2];                       // Brawl, then Progression
    CHECK(card.brawl && card.format == Format::FreeForAll && card.name == "BRAWL" && card.selected && card.enabled && !card.progression);
    CHECK(card.description == "Free-for-all with bare fists: everyone spawns naked, no weapons. First to the limit wins.");
    int selected = 0, brawls = 0;
    for (const ModeCard& m : s.modes) { selected += m.selected; brawls += m.brawl; }
    CHECK(selected == 1 && brawls == 1 && !s.modes[0].selected);              // the free-for-all card is not also lit
    CHECK(Ids(s) == (std::vector<LobbyRow>{LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::Damage, LobbyRow::ClearGround, LobbyRow::Sync}));   // no Gear, no Tier
    CHECK(RowOf(s, LobbyRow::Gear) == nullptr && RowOf(s, LobbyRow::Tier) == nullptr);
    LobbyScene own = BuildLobbyScene(InLobby(true));
    CHECK(own.modes[0].selected && !own.modes[own.modes.size() - 2].selected && RowOf(own, LobbyRow::Gear) != nullptr);
    LobbyScene member = BuildLobbyScene(InLobby(false, Format::FreeForAll, Gear::Fists));
    CHECK(member.modes[member.modes.size() - 2].selected && !member.modes[member.modes.size() - 2].enabled);
    LobbyLocal prog = InLobby(true, Format::FreeForAll, Gear::Fists); prog.progression = true;
    CHECK(!BuildLobbyScene(prog).modes[BuildLobbyScene(prog).modes.size() - 2].selected);   // the Progression card wins

    MatchConfig c; c.format = Format::TwoTeams; c.scoreLimit = 30; c.gear = Gear::Mirror; c.tier = 6; c.timeLimitS = 600;
    MatchConfig b = SelectMode(c, Format::FreeForAll, true);
    CHECK(IsBrawl(b) && b.gear == Gear::Fists && b.tier == kBrawlTier && b.scoreLimit == DefaultScoreLimit(Format::FreeForAll) && b.timeLimitS == 600);
    CHECK(Sanitize(b).gear == Gear::Fists && Sanitize(b).tier == kBrawlTier);
    b.tier = 6; CHECK(Sanitize(b).tier == kBrawlTier); b.tier = kBrawlTier;   // Brawl's tier is fixed
    b.scoreLimit = 20;
    CHECK(SelectMode(b, Format::FreeForAll, true).scoreLimit == 20);          // the card again changes nothing
    MatchConfig back = SelectMode(b, Format::FreeForAll, false);              // the free-for-all card: same format, other rules
    CHECK(!IsBrawl(back) && back.format == Format::FreeForAll && back.gear == Gear::Own && back.tier == MatchConfig{}.tier && back.scoreLimit == 5);
    MatchConfig duel = SelectMode(b, Format::Duel, false);
    CHECK(duel.format == Format::Duel && duel.gear == Gear::Own && duel.tier == MatchConfig{}.tier && duel.scoreLimit == DefaultScoreLimit(Format::Duel));
    CHECK(SelectMode(c, Format::TwoTeams, false).scoreLimit == 30 && SelectMode(c, Format::TwoTeams, false).gear == Gear::Mirror);   // unchanged
    CHECK(SelectMode(c, Format::Duel, false).gear == Gear::Mirror);                                                                    // as SelectFormat
    // The Gear row never lands on Fists, whichever way it is stepped.
    MatchConfig g; g.gear = Gear::Mirror;
    CHECK(StepSetting(g, LobbyRow::Gear, 1).gear == Gear::Own && StepSetting(StepSetting(g, LobbyRow::Gear, -1), LobbyRow::Gear, -1).gear == Gear::Own);
    g.gear = Gear::Fists;
    CHECK(StepSetting(g, LobbyRow::Gear, 1).gear == Gear::RandomTier && StepSetting(g, LobbyRow::Gear, -1).gear == Gear::Mirror);
}

static size_t IndexOf(const char* display) { for (size_t i = 0; i < MapCount(); ++i) if (std::string(kMaps[i].display) == display) return i; return MapCount(); }

static const MapCard* Card(const LobbyScene& s, size_t index) { for (const MapCard& m : s.maps) if (m.index == index) return &m; return nullptr; }

static void TestMaps() {
    LobbyLocal l = InLobby(true, Format::FreeForAll);
    l.mapPick = "/Game/Maps/Arenas/Map_Arena_Yard";
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.maps.size() == PickableCount());                    // Cutting, Tavern and Smithery are not offered
    size_t yard = IndexOf("Yard"), tavern = IndexOf("Tavern"), abyss = IndexOf("Abyss"), alley = IndexOf("Alley");
    CHECK(yard < MapCount() && tavern < MapCount() && abyss < MapCount() && alley < MapCount());
    if (s.maps.size() != PickableCount() || yard >= MapCount() || tavern >= MapCount() || abyss >= MapCount() || alley >= MapCount()) return;
    CHECK(!Card(s, tavern) && !Card(s, IndexOf("Cutting")) && !Card(s, IndexOf("Smithery")) && Card(s, abyss));
    for (const MapCard& m : s.maps) {
        CHECK(m.name == kMaps[m.index].display);
        CHECK(m.selected == (m.index == yard));
        CHECK(m.allowed == (m.index != abyss));                  // the Abyss is Zombies only
    }
    CHECK(Card(s, yard)->tag == "Arena");
    CHECK(Card(s, abyss)->tag == "Large level " + kDot + " no spawn points: ring spawn");

    l.config = SelectFormat(l.config, Format::Duel);
    l.mapPick = "/Game/Maps/Map_Hub_Tavern_Frank";              // not pickable: the first arena instead
    s = BuildLobbyScene(l);
    CHECK(Card(s, alley)->selected);
    CHECK(!Card(s, abyss)->allowed && Card(s, abyss)->reason == "Duel needs an arena" && !Card(s, abyss)->enabled);
    CHECK(Card(s, yard)->allowed && Card(s, yard)->enabled && Card(s, yard)->reason.empty());

    l.config = SelectFormat(l.config, Format::Zombies);
    s = BuildLobbyScene(l);
    for (const MapCard& m : s.maps) {
        CHECK(m.allowed == (m.index == abyss) && m.selected == (m.index == abyss));
        if (m.index != abyss) CHECK(m.reason == "Zombies needs the Abyss");
    }
    CHECK(std::string(MapRefusal(Format::FreeForAll)) == "Not for this mode");
}

// Item 8: another player on a different mod build is named, and the host cannot start until it matches or the
// override is on. Builds not known yet say nothing.
static void TestBuildMismatch() {
    LobbyLocal h = InLobby(true);
    h.myBuild = "0.6.4";
    h.players[1].build = "0.6.4";
    LobbyScene s = BuildLobbyScene(h);
    CHECK(s.start && !s.buildMismatch && s.waiting.empty());
    h.players[1].build = "";                          // Bob's member data has not come yet
    s = BuildLobbyScene(h);
    CHECK(s.start && !s.buildMismatch);
    h.players[1].build = "0.6.3";
    s = BuildLobbyScene(h);
    CHECK(!s.start && s.buildMismatch && s.waiting == "Bob on 0.6.3 - update");
    h.players[0].build = "9.9.9";                     // my own row never counts against me
    CHECK(BuildLobbyScene(h).waiting == "Bob on 0.6.3 - update");
    h.allowBuildMismatch = true;
    s = BuildLobbyScene(h);
    CHECK(s.start && s.buildMismatch && s.waiting == "Bob on 0.6.3 - update (allowed: allow_build_mismatch=1)");
    LobbyLocal m = InLobby(false);                    // a member sees the host's build named instead of "Waiting"
    m.myBuild = "0.6.3";
    m.players[0].build = "0.6.4";
    s = BuildLobbyScene(m);
    CHECK(!s.start && s.buildMismatch && s.waiting == "Ann on 0.6.4 - update");
    m.players[0].build = "0.6.3";
    CHECK(BuildLobbyScene(m).waiting == "Waiting for the host");
}

static void TestDamage() {
    DamageRange r;                                         // until the slider is read: 0..200%, stock 100
    MatchConfig c;
    CHECK(c.damagePct == kDamageStock && Sanitize(c).damagePct == kDamageStock);
    c.damagePct = 5000;
    CHECK(Sanitize(c).damagePct == kDamageMaxPct);
    c.damagePct = kDamageStock;
    CHECK(StepSetting(c, LobbyRow::Damage, 1, r).damagePct == 110 && StepSetting(c, LobbyRow::Damage, -1, r).damagePct == 90);
    c.damagePct = 110;
    CHECK(StepSetting(c, LobbyRow::Damage, -1, r).damagePct == kDamageStock);   // back on the stock value: stored as stock
    c.damagePct = 200;
    CHECK(StepSetting(c, LobbyRow::Damage, 1, r).damagePct == 200);              // the slider's top: no wrap
    c.damagePct = 0;
    CHECK(StepSetting(c, LobbyRow::Damage, -1, r).damagePct == 0);
    MatchConfig only = StepSetting(MatchConfig{}, LobbyRow::Damage, 1, r);
    CHECK(only.tier == MatchConfig{}.tier && only.scoreLimit == MatchConfig{}.scoreLimit);   // a step changes damage alone

    LobbyLocal l = InLobby(true, Format::FreeForAll);
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Damage).label == "Player damage");
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Damage).value == "100% (stock)" && Need(BuildLobbyScene(l), LobbyRow::Damage).enabled);
    l.config.damagePct = 150;
    CHECK(Need(BuildLobbyScene(l), LobbyRow::Damage).value == "150%");
    LobbyLocal m = InLobby(false, Format::FreeForAll);
    CHECK(!Need(BuildLobbyScene(m), LobbyRow::Damage).enabled);                 // the host's alone
    LobbyLocal prog = InLobby(true, Format::FreeForAll); prog.progression = true;
    CHECK(Ids(BuildLobbyScene(prog)) == (std::vector<LobbyRow>{LobbyRow::Damage}));   // co-op takes damage too
}

static void TestModdedMaps() {
    LobbyLocal l = InLobby(true, Format::FreeForAll);
    l.modMaps = {{"/Game/Maps/Custom/Arena_Bog", "Arena Bog", "BogMap_P.pak"}, {"/Game/Maps/Custom/Plain", "Plain", ""}};
    l.mapPick = "/Game/Maps/Arenas/Map_Arena_Yard";
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.maps.size() == PickableCount() + 2);
    if (s.maps.size() != PickableCount() + 2) return;
    const MapCard& bog = s.maps[PickableCount()];
    const MapCard& plain = s.maps[PickableCount() + 1];
    CHECK(bog.modded && bog.index == MapCount() && bog.path == "/Game/Maps/Custom/Arena_Bog" && bog.name == "Arena Bog");
    CHECK(bog.tag == "Modded map \xC2\xB7 BogMap_P.pak" && plain.tag == "Modded map" && plain.index == MapCount() + 1);
    CHECK(bog.allowed && bog.enabled && !bog.selected && !plain.selected);
    size_t selectedStock = 0;
    for (const MapCard& m : s.maps) if (!m.modded && m.selected) ++selectedStock;
    CHECK(selectedStock == 1);                                     // the stock pick stays marked while no custom map is picked
    // Picking a custom map: it alone is selected (the object-name form of the pick counts), and START waits for the hash.
    l.mapPick = "/game/maps/custom/arena_bog.Arena_Bog";
    s = BuildLobbyScene(l);
    CHECK(s.maps[PickableCount()].selected && !s.maps[PickableCount() + 1].selected);
    selectedStock = 0;
    for (const MapCard& m : s.maps) if (!m.modded && m.selected) ++selectedStock;
    CHECK(selectedStock == 0);
    CHECK(s.start);
    l.modPickPending = true; s = BuildLobbyScene(l);
    CHECK(!s.start && s.waiting == "Checking the map pack");
    l.mapPick = "/Game/Maps/Arenas/Map_Arena_Yard"; s = BuildLobbyScene(l);   // a stock pick ignores the flag
    CHECK(s.start);
    // A member sees them, disabled; Zombies refuses them (the Abyss is its map); Progression shows no maps.
    LobbyLocal m = InLobby(false); m.modMaps = l.modMaps; m.mapPick = "/Game/Maps/Custom/Plain";
    s = BuildLobbyScene(m);
    CHECK(s.maps.size() == PickableCount() + 2 && !s.maps[PickableCount()].enabled && s.maps[PickableCount() + 1].selected);
    LobbyLocal z = InLobby(true, Format::Zombies); z.modMaps = l.modMaps; z.mapPick = "/Game/Maps/Custom/Plain";
    s = BuildLobbyScene(z);
    CHECK(s.maps.size() == PickableCount() + 2 && !s.maps[PickableCount()].allowed && !s.maps[PickableCount()].enabled &&
          !s.maps[PickableCount()].reason.empty() && !s.maps[PickableCount() + 1].selected);
    LobbyLocal pr = InLobby(true); pr.modMaps = l.modMaps; pr.progression = true;
    CHECK(BuildLobbyScene(pr).maps.empty());
}

static void TestGroupsAndSections() {
    LobbyScene s = BuildLobbyScene(InLobby(true));
    CHECK(s.modes.size() == kModeCardCount);
    for (size_t i = 0; i < s.modes.size(); ++i) CHECK(s.modes[i].group == ModeGroupAt(i));
    for (const ModeCard& c : s.modes) {
        const bool coop = c.progression || (!c.brawl && IsBotFormat(c.format));
        CHECK((c.group == ModeGroup::Coop) == coop);
    }
    CHECK(ModeGroupAt(0) == ModeGroup::Versus && ModeGroupAt(size_t(kFormatCount)) == ModeGroup::Versus &&
          ModeGroupAt(size_t(kFormatCount) + 1) == ModeGroup::Coop);
    CHECK(SectionOf(LobbyRow::Limit) == LobbySection::Match && SectionOf(LobbyRow::Damage) == LobbySection::Match);
    CHECK(SectionOf(LobbyRow::MyTeam) == LobbySection::Teams && SectionOf(LobbyRow::FriendlyFire) == LobbySection::Teams);
    CHECK(SectionOf(LobbyRow::Gear) == LobbySection::Gear && SectionOf(LobbyRow::Tier) == LobbySection::Gear);
    CHECK(SectionOf(LobbyRow::ClearGround) == LobbySection::World && SectionOf(LobbyRow::Sync) == LobbySection::World);
    CHECK(std::string(SectionName(LobbySection::Match)) == "MATCH" && std::string(SectionName(LobbySection::World)) == "WORLD");
    for (size_t i = 0; i <= size_t(LobbyRow::Sync); ++i) CHECK(SectionOf(LobbyRow(i)) < LobbySection::Count);
}

static void TestSummary() {
    LobbyScene s = BuildLobbyScene(InLobby(true, Format::TwoTeams, Gear::RandomTier));
    CHECK(s.summary.rfind("2 TEAMS " + kDot + " ", 0) == 0);
    CHECK(s.summary.find("Random by tier, tier ") != std::string::npos);
    LobbyScene own = BuildLobbyScene(InLobby(true));
    CHECK(own.summary.find("Own loadouts") != std::string::npos && own.summary.find("tier") == std::string::npos);
    LobbyScene brawl = BuildLobbyScene(InLobby(true, Format::FreeForAll, Gear::Fists));
    CHECK(brawl.summary.rfind("BRAWL", 0) == 0 && brawl.summary.find("loadouts") == std::string::npos);
    LobbyLocal pr = InLobby(false); pr.progression = true;
    CHECK(BuildLobbyScene(pr).summary.rfind("PROGRESSION", 0) == 0);
    LobbyLocal idle; idle.phase = LobbyPhase::Idle;
    CHECK(BuildLobbyScene(idle).summary.empty());
}

static void TestFilters() {
    LobbyLocal l; l.phase = LobbyPhase::Idle; l.version = "0.2.0";
    LobbyFound duel; duel.name = "D"; duel.mode = "Duel"; duel.players = 2; duel.compatible = true;
    LobbyFound full; full.name = "F"; full.mode = "Free-for-all"; full.players = 8; full.compatible = true;
    LobbyFound old; old.name = "O"; old.mode = "Duel"; old.players = 5; old.compatible = false;
    l.found = {duel, full, old};
    LobbyScene s = BuildLobbyScene(l);
    CHECK(s.filters.size() == kFilterChips && s.browser.size() == 3 && s.browserCount == "3 lobbies");
    for (const FilterChip& c : s.filters) CHECK(!c.on);
    CHECK(s.filters[0].label == "Any mode");
    // The mode chip steps through every mode and wraps to any.
    BrowserFilter f;
    for (size_t i = 0; i < BrowserModes().size(); ++i) f = ToggleFilter(f, 0);
    CHECK(f.mode == 0);
    f = ToggleFilter(f, 0); f = ToggleFilter(f, 0);
    CHECK(BrowserModes()[f.mode] == "Duel");
    l.filter = f;
    s = BuildLobbyScene(l);
    CHECK(s.filters[0].on && s.filters[0].label == "Duel" && s.browser.size() == 2 && s.browserCount == "2 of 3 lobbies shown");
    l.filter = ToggleFilter(l.filter, 2);                 // compatible only
    s = BuildLobbyScene(l);
    CHECK(s.filters[2].on && s.browser.size() == 1 && s.browser[0].name == "D");
    l.filter = {};
    l.filter = ToggleFilter(l.filter, 1);                 // has room
    s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 2 && s.browser[0].name == "D" && s.browser[1].name == "O");
    l.filter = {};
    l.filter = ToggleFilter(l.filter, 3);                 // most players first
    s = BuildLobbyScene(l);
    CHECK(s.browser.size() == 3 && s.browser[0].name == "F" && s.browser[1].name == "O" && s.browser[2].name == "D");
    CHECK(s.browser[0].source == 1 && s.browser[1].source == 2 && s.browser[2].source == 0);   // what JOIN joins
    CHECK(s.browserEmpty.empty());
    CHECK(s.browserCount == "3 lobbies");
    l.filter = ToggleFilter(l.filter, 3);
    CHECK(!l.filter.mostPlayers && ToggleFilter(l.filter, 9).mode == l.filter.mode);
    // Every browser mode is a name a host can publish.
    for (Format m : kModeOrder) {
        bool listed = false;
        for (const std::string& n : BrowserModes()) listed = listed || n == ModeName(m, Gear::Own);
        CHECK(listed);
    }
    l.filter = {}; l.filter.mode = 7;                     // Zombies: none listed
    s = BuildLobbyScene(l);
    CHECK(s.browser.empty() && s.browserEmpty == "No lobby matches these filters." && s.browserCount == "0 of 3 lobbies shown");
    l.found.clear(); l.filter = {};
    s = BuildLobbyScene(l);
    CHECK(s.browserCount.empty() && !s.browserEmpty.empty() && s.browserEmpty != "No lobby matches these filters.");
    LobbyLocal in = InLobby(true);
    CHECK(BuildLobbyScene(in).filters.empty());
}

int main() {
    TestPrevNext();
    TestStepSetting();
    TestNewRows();
    TestTeamPick();
    TestSelectFormat();
    TestPill();
    TestHeader();
    TestBrowser();
    TestOutOfLobby();
    TestHostAgainstMember();
    TestRowsPerFormat();
    TestLabelsAndValues();
    TestGearHint();
    TestOtherHints();
    TestMyTeam();
    TestRoster();
    TestReady();
    TestModes();
    TestBrawl();
    TestMaps();
    TestModdedMaps();
    TestBuildMismatch();
    TestDamage();
    TestGroupsAndSections();
    TestSummary();
    TestFilters();
    if (g_fail) { std::printf("%d lobby scene checks failed\n", g_fail); return 1; }
    std::printf("all lobby scene tests passed\n");
    return 0;
}
