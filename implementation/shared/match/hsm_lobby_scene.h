// Lobby page (MATCH_ROUNDS_UI_CHAT_PLAN 3.4): what the multiplayer page shows, as data. A header with the state pill
// and the version badge; left, the lobby browser (not in a lobby) or the players (in one); middle, the mode cards and
// the setting rows with their steppers and one-line hints for the current format; right, the map cards and START
// MATCH. Members see everything the host chose, disabled, except their own team, and mark themselves ready (the
// host starts the match instead). The page (ui/menu.cpp) fills a LobbyLocal from the Steam lobby and maps the scene
// onto widgets; the steppers' arithmetic is here too.
#pragma once
#include "match/hsm_hud_scene.h"
#include "match/hsm_map_list.h"
#include "session/hsm_build.h"
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace hsm {

enum class LobbyPhase : uint8_t { Offline, Idle, Creating, Joining, InLobby };   // game::Lobby::State

struct LobbyPlayer {
    std::string name;
    bool host = false, ready = false, me = false;
    int team = kNoTeam;                 // their own pick (Auto: kNoTeam)
    int hostOverride = kNoTeam;         // where the host moved them
    std::string build;                  // their mod build (session/hsm_build.h), empty until their member data comes
};

// The public-lobby list's filters (the page's chips, in this order): mode, room, compatibility, sorting. All off: every
// lobby Steam returned, in its order.
struct BrowserFilter {
    uint8_t mode = 0;                   // 0: any; else the kBrowserModes entry shown
    bool hasRoom = false, compatibleOnly = false, mostPlayers = false;
};

struct LobbyFound {                     // a public lobby from Find
    std::string name, map, mode;
    int players = 0;
    bool compatible = false;
    std::string reason;                 // why not, when !compatible
};

struct LobbyModMap { std::string path, display, pak; };   // a custom map from a mod pak (session/hsm_modmaps.h); pak empty when unknown

struct LobbyLocal {
    LobbyPhase phase = LobbyPhase::Offline;
    bool host = false;                  // we own the lobby
    bool publicLobby = false;           // it is listed publicly (else friends only)
    std::string myName, status, version;
    std::string myBuild;                // this mod's build (ModBuildString)
    bool allowBuildMismatch = false;    // allow_build_mismatch=1: the host may start with other builds in
    MatchConfig config;
    DamageRange damage;                 // the game's own damage slider here (game::VanillaDamageRange)
    std::string mapPick;                // the host's pick (package path), empty until picked
    bool progression = false;           // the host chose the Progression card (LobbyData hsm_prog): the stock hub, no match
    std::vector<LobbyModMap> modMaps;   // the custom maps found on this machine (modded maps, after the stock ones)
    bool modPickPending = false;        // the pick is a modded map whose pak is still being hashed: START waits
    std::vector<LobbyPlayer> players;
    std::vector<LobbyFound> found;
    BrowserFilter filter;
    int maxPlayers = 8;
};

// MyTeam stays last: ui/menu.cpp makes one stepper per row up to it.
enum class LobbyRow : uint8_t { Limit, Time, Respawn, Gear, Tier, FriendlyFire, Damage, WaveBreak, ClearGround, MyTeam, Sync };   // Sync last: the menu builds rows in this order

struct SettingRow { LobbyRow id = LobbyRow::Limit; std::string label, value, hint; bool enabled = false; };
// The mode cards' groups on the MODE tab (Party joins them with the party modes).
enum class ModeGroup : uint8_t { Versus, Coop };
struct ModeCard { Format format = Format::FreeForAll; std::string name, description; bool selected = false, enabled = false; bool progression = false; bool brawl = false;
                  ModeGroup group = ModeGroup::Versus; };   // progression: the extra last card; brawl: free-for-all with gear = Fists
struct MapCard { size_t index = 0; std::string name, tag, reason; bool allowed = false, selected = false, enabled = false;
                 bool modded = false; std::string path; };   // modded: index is MapCount() + the custom map's place, path its package path
struct BrowserRow { std::string name, players, detail; Span note; bool joinable = false; size_t source = 0; };   // source: its index in LobbyLocal::found
struct RosterRow {
    std::string name, team;             // team: the formats with teams only
    Rgba swatch;                        // a == 0: none
    bool host = false, ready = false, moved = false, me = false, movable = false;
};

inline constexpr size_t kBrowserRows = 6;

// The RULES tab's sections, in page order; each row sits in one.
enum class LobbySection : uint8_t { Match, Teams, Gear, World, Count };
inline LobbySection SectionOf(LobbyRow r) {
    switch (r) {
        case LobbyRow::FriendlyFire: case LobbyRow::MyTeam: return LobbySection::Teams;
        case LobbyRow::Gear: case LobbyRow::Tier: return LobbySection::Gear;
        case LobbyRow::WaveBreak: case LobbyRow::ClearGround: case LobbyRow::Sync: return LobbySection::World;
        default: return LobbySection::Match;
    }
}
inline const char* SectionName(LobbySection s) {
    switch (s) {
        case LobbySection::Teams: return "TEAMS";
        case LobbySection::Gear: return "GEAR";
        case LobbySection::World: return "WORLD";
        default: return "MATCH";
    }
}

// The browser's mode chip steps through these (0: any).
inline const std::vector<std::string>& BrowserModes() {
    static const std::vector<std::string> m = {"Any mode", "Free-for-all", "Duel", "2 teams", "3 teams", "Brawl", "Survival", "Zombies"};
    return m;
}
inline constexpr size_t kFilterChips = 4;   // mode, has room, compatible only, most players first
struct FilterChip { std::string label; bool on = false; };
inline std::vector<FilterChip> FilterChips(const BrowserFilter& f) {
    const auto& modes = BrowserModes();
    return {{modes[f.mode < modes.size() ? f.mode : 0], f.mode != 0}, {"Has room", f.hasRoom},
            {"Compatible only", f.compatibleOnly}, {"Most players first", f.mostPlayers}};
}
// A chip clicked: the mode steps on (wrapping to any), the others turn on or off.
inline BrowserFilter ToggleFilter(BrowserFilter f, size_t chip) {
    switch (chip) {
        case 0: f.mode = uint8_t((f.mode + 1) % BrowserModes().size()); break;
        case 1: f.hasRoom = !f.hasRoom; break;
        case 2: f.compatibleOnly = !f.compatibleOnly; break;
        case 3: f.mostPlayers = !f.mostPlayers; break;
        default: break;
    }
    return f;
}
inline bool PassesFilter(const LobbyFound& l, const BrowserFilter& f, int maxPlayers) {
    const auto& modes = BrowserModes();
    if (f.mode != 0 && f.mode < modes.size() && l.mode != modes[f.mode]) return false;
    if (f.hasRoom && l.players >= maxPlayers) return false;
    if (f.compatibleOnly && !l.compatible) return false;
    return true;
}
inline constexpr size_t kModeCardCount = kFormatCount + 2;   // the formats, then Brawl and Progression

struct LobbyScene {
    Span pill;
    std::string me, badge, status;      // status: not in a lobby only (in one, the pill says it)
    bool idle = false;                  // Host, Find, the browser and the idle hint
    std::vector<BrowserRow> browser;
    std::vector<FilterChip> filters;    // kFilterChips, while idle
    std::string browserEmpty;           // why the list is empty (nothing found, or the filters hide it all)
    std::string browserCount;           // "3 of 7 lobbies shown" (filters on), "7 lobbies", "" (none found)
    bool in = false;                    // the players, Invite and Leave, the modes, the settings, the maps
    std::string playersTitle;
    std::vector<RosterRow> roster;
    bool ready = false;                 // the Ready toggle: members (the host starts the match instead)
    bool readyOn = false;               // we are marked ready: the toggle is framed, our row shows the dot
    std::string readyLabel;
    bool balance = false;               // Auto-balance: the host, in the formats with teams
    std::vector<ModeCard> modes;        // the grid's reading order, two to a row
    std::vector<SettingRow> settings;
    std::string hostOnly;               // members: "Only the host can change these."
    std::vector<MapCard> maps;          // the pickable maps, kMaps order; none for Progression
    bool start = false;                 // START MATCH: the host, while every known build matches (or the override is on)
    std::string waiting;                // members: "Waiting for the host"; anyone: the build warning, which replaces it
    std::string summary;                // beside START: the mode, the map and the gear, one line
    bool buildMismatch = false;         // another player is on a different build (waiting says who)
};

// ------------------------------------------------------------------ steppers

// The option before `cur`, wrapping; an unknown value steps to the last (NextOf steps it to the first).
inline int PrevOf(const std::vector<int>& options, int cur) {
    if (options.empty()) return cur;
    auto it = std::find(options.begin(), options.end(), cur);
    return it == options.end() || it == options.begin() ? options.back() : *(it - 1);
}
inline int StepOf(const std::vector<int>& options, int cur, int dir) { return dir < 0 ? PrevOf(options, cur) : NextOf(options, cur); }

inline std::vector<int> LimitOptions(Format f) {
    if (f == Format::Duel) return {1, 2, 3, 5, 7, 10};                 // rounds to win
    return {3, 5, 10, 15, 20, 30, 50};
}
inline const std::vector<int> kTimeOptions = {0, 300, 600, 900, 1200};
inline const std::vector<int> kRespawnOptions = {3, 5, 10, 15, 20};
// The wave break in seconds the match runs (0 = the format's own: Survival 60, Zombies 10).
inline int WaveBreakShown(const MatchConfig& c) { return c.waveBreakS ? c.waveBreakS : c.format == Format::Zombies ? int(Match::kZombieBreakMs / 1000) : int(Match::kWaveBreakMs / 1000); }

// The format and team pick go their own ways (SelectFormat, StepTeamPick); every other row steps here, by +1 or -1.
// A value out of range steps from where Sanitize would put it. damage: the game's own slider (the Damage row's range).
inline MatchConfig StepSetting(MatchConfig c, LobbyRow s, int dir, const DamageRange& damage = DamageRange{}) {
    bool back = dir < 0;
    switch (s) {
        case LobbyRow::Limit: c.scoreLimit = uint16_t(StepOf(LimitOptions(c.format), c.scoreLimit, dir)); break;
        case LobbyRow::Time: c.timeLimitS = uint16_t(StepOf(kTimeOptions, c.timeLimitS, dir)); break;
        case LobbyRow::Respawn: c.respawnDelayS = uint16_t(StepOf(kRespawnOptions, c.respawnDelayS, dir)); break;
        case LobbyRow::Gear: {
            uint8_t g = uint8_t(c.gear) < kGearRowCount ? uint8_t(c.gear) : 0;   // Fists (the Brawl card) is no stop of the row
            c.gear = Gear((g + (back ? kGearRowCount - 1 : 1)) % kGearRowCount);
            break;
        }
        case LobbyRow::Tier: c.tier = uint8_t((std::min<uint8_t>(c.tier, 7) + (back ? 7 : 1)) % 8); break;
        case LobbyRow::FriendlyFire: c.friendlyFire = !c.friendlyFire; break;
        case LobbyRow::Damage: c.damagePct = StepDamage(c.damagePct, dir, damage); break;
        case LobbyRow::WaveBreak: {
            std::vector<int> o(std::begin(kWaveBreakOptions), std::end(kWaveBreakOptions));
            c.waveBreakS = uint16_t(StepOf(o, WaveBreakShown(c), dir)); break;
        }
        case LobbyRow::ClearGround: {
            std::vector<int> o(std::begin(kClearGroundOptions), std::end(kClearGroundOptions));
            c.clearGroundMin = uint8_t(StepOf(o, c.clearGroundMin, dir)); break;
        }
        case LobbyRow::MyTeam: break;
        case LobbyRow::Sync: c.hostPhysics = !c.hostPhysics; break;
    }
    return c;
}

// A new format starts at its own default limit (the other settings stay); the same format changes nothing.
inline MatchConfig SelectFormat(MatchConfig c, Format f) {
    if (c.format == f) return c;
    c.format = f;
    c.scoreLimit = DefaultScoreLimit(f);
    return c;
}

// A mode card picked: the Brawl card is free-for-all with bare fists (gear Fists, kBrawlTier, free-for-all's default limit);
// the format cards are SelectFormat, and leaving Brawl puts the gear and tier back at their defaults (even for the
// free-for-all card: the same format, other rules).
inline MatchConfig SelectMode(MatchConfig c, Format f, bool brawl) {
    if (brawl) {
        if (IsBrawl(c)) return c;
        c.format = Format::FreeForAll;
        c.scoreLimit = DefaultScoreLimit(c.format);
        c.gear = Gear::Fists;
        c.tier = kBrawlTier;
        return c;
    }
    if (c.gear == Gear::Fists) {                     // leaving Brawl
        c.gear = Gear::Own;
        c.tier = MatchConfig{}.tier;
        if (c.format == f) { c.scoreLimit = DefaultScoreLimit(f); return c; }
    }
    return SelectFormat(c, f);
}

// Forward: Auto, team 0, ..., the last team, Auto (NextTeamPick). Back the other way. A pick the format has no team
// for counts as Auto, as ResolveTeams counts it.
inline int8_t StepTeamPick(int8_t cur, int teams, int dir) {
    if (dir >= 0) return NextTeamPick(cur, teams);
    if (teams <= 0) return kNoTeam;
    if (cur < 0 || cur >= teams) return int8_t(teams - 1);
    return cur == 0 ? kNoTeam : int8_t(cur - 1);
}

// ------------------------------------------------------------------ text

inline bool HasTeams(Format f) { return f == Format::TwoTeams || f == Format::ThreeTeams; }

inline constexpr const char* kBrawlBlurb = "Free-for-all with bare fists: everyone spawns naked, no weapons. First to the limit wins.";

inline const char* FormatBlurb(Format f) {
    switch (f) {
        case Format::Duel: return "Rounds to the last one standing. Everybody respawns together; new gear each round with random or identical gear.";
        case Format::TwoTeams: return "Red against Blue. Every kill scores for the team; the first team to the limit wins.";
        case Format::ThreeTeams: return "Red, Blue and Green. Every kill scores for the team; the first team to the limit wins.";
        case Format::PlayersVsBots: return "Everyone against waves of bots that rank up. The fallen return when a wave is cleared.";
        case Format::Zombies: return "Zombie waves in the Abyss, one zombie at a time per player. The fallen return when a wave is cleared.";
        case Format::FreeForAll: break;
    }
    return "Everyone for themselves. Every kill scores; the first to the limit wins.";
}

inline const char* MapTag(const MapInfo& m) {
    if (m.abyss) return "Large level \xC2\xB7 no spawn points: ring spawn";
    return m.bigArena ? "Large level" : "Arena";
}

inline const char* MapRefusal(Format f) {
    if (f == Format::Zombies) return "Zombies needs the Abyss";
    if (f == Format::Duel) return "Duel needs an arena";
    return "Not for this mode";
}

inline const char* GearName(Gear g) {
    if (g == Gear::RandomTier) return "Random by tier";
    if (g == Gear::Mirror) return "Everyone identical";
    return "Own loadouts";
}

inline int TierBudget(int tier) { return kTierBudget[std::clamp(tier, 0, 7)]; }

namespace lobby_detail {
inline bool SamePath(std::string_view a, std::string_view pick) {
    const std::string_view b = map_detail::StripObjectName(pick);
    if (a.empty() || a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = char(x + 32);
        if (y >= 'A' && y <= 'Z') y = char(y + 32);
        if (x != y) return false;
    }
    return true;
}

inline bool ValidTeam(int t, Format f) { return HasTeams(f) && t >= 0 && t < TeamCount(f); }
inline int EffectiveTeam(const LobbyPlayer& p, Format f) {
    return ValidTeam(p.hostOverride, f) ? p.hostOverride : ValidTeam(p.team, f) ? p.team : kNoTeam;
}

inline SettingRow Row(const LobbyLocal& l, LobbyRow s) {
    const MatchConfig& c = l.config;
    Format f = c.format;
    bool duel = f == Format::Duel, bots = IsBotFormat(f);
    SettingRow r;
    r.id = s;
    r.enabled = l.host;
    switch (s) {
        case LobbyRow::Limit:
            r.label = duel ? "Rounds to win" : "Score limit";
            r.value = std::to_string(c.scoreLimit);
            r.hint = duel ? "The first to win this many rounds takes the match."
                   : HasTeams(f) ? "The first team to this many kills wins." : "The first to this many kills wins.";
            break;
        case LobbyRow::Time:
            r.label = "Time limit";
            r.value = TimeLabel(c.timeLimitS);
            if (bots) r.hint = c.timeLimitS ? "Players win if they hold out to the end (one wave cleared)." : "No clock: hold out as long as you can.";
            else r.hint = c.timeLimitS ? "When it runs out the leader wins; a tie goes to sudden death." : "No clock: play to the limit.";
            break;
        case LobbyRow::Respawn:
            r.label = duel ? "Round break" : "Respawn delay";
            r.value = std::to_string(c.respawnDelayS) + " s";
            r.hint = duel ? "The pause between the end of a round and the next." : "How long the fallen wait to come back.";
            break;
        case LobbyRow::Gear:
            r.label = "Gear";
            r.value = GearName(c.gear);
            if (c.gear == Gear::RandomTier) r.hint = bots ? "A random loadout for each player at the start, kept for the whole run."
                                                    : duel ? "Fresh random gear for each fighter every round." : "Fresh random gear for each fighter every life.";
            else if (c.gear == Gear::Mirror) r.hint = bots ? "One random loadout for everyone, kept for the whole run."
                                                  : duel ? "One random loadout for everyone, new each round." : "One random loadout for everyone, until the rematch.";
            else r.hint = "Everyone fights in their own loadout.";
            break;
        case LobbyRow::Tier:
            r.label = bots ? "Starting tier" : "Gear tier";
            r.value = "tier " + std::to_string(int(c.tier));
            if (c.gear != Gear::Own) r.value += " \xC2\xB7 budget " + std::to_string(TierBudget(c.tier));
            // 0.8.0-pre20: the tier is every player's strength (muscle and grip, forced: ForcedWeight), with any gear.
            r.hint = f == Format::Zombies ? "Zombies start at this rank and rise one every 3 waves. "
                   : bots ? "Bots start at this rank and rise one every 2 waves. " : "";
            r.hint += c.gear == Gear::Own ? (bots ? "Also every player's strength." : "Sets every player's strength (muscle and grip).")
                                          : (bots ? "Also every player's strength and the random gear's budget." : "Sets every player's strength; the random gear is rolled within this budget.");
            break;
        case LobbyRow::FriendlyFire:
            r.label = "Friendly fire";
            r.value = c.friendlyFire ? "On" : "Off";
            if (bots) r.hint = c.friendlyFire ? "Players can hurt each other." : "Players cannot hurt each other.";
            else r.hint = c.friendlyFire ? "Hits on teammates count." : "Teammates cannot hurt each other.";
            break;
        case LobbyRow::Damage:
            r.label = "Player damage";
            r.value = DamageLabel(c.damagePct, l.damage);
            r.hint = "How hard hits land on every player, the game's own slider for everyone. Yours is back after the session.";
            break;
        case LobbyRow::WaveBreak: {
            const int s = WaveBreakShown(c);
            r.label = "Wave break";
            r.value = s == 120 ? "2 min" : std::to_string(s) + " s";
            r.hint = "The pause between waves. Hold G to vote to skip it.";
            break;
        }
        case LobbyRow::ClearGround:
            r.label = "Clear ground";
            r.value = c.clearGroundMin ? "Every " + std::to_string(c.clearGroundMin) + " min" : "Off";
            r.hint = c.clearGroundMin ? "Dropped weapons and loose armour are cleared on this timer, to keep the lag down. Held and worn gear stays."
                                      : "Dropped weapons and loose armour stay until the next round.";
            break;
        case LobbyRow::MyTeam: {
            r.label = "My team";
            r.enabled = true;                                        // everyone picks their own
            const LobbyPlayer* me = nullptr;
            for (const LobbyPlayer& p : l.players) if (p.me) me = &p;
            r.value = TeamLabel(me && ValidTeam(me->team, f) ? me->team : kNoTeam);
            r.hint = me && ValidTeam(me->hostOverride, f) ? "The host moved you to " + TeamLabel(me->hostOverride) + "."
                                                          : std::string("Auto puts you on the smallest team.");
            break;
        }
        case LobbyRow::Sync:
            r.label = "Sync";
            r.value = c.hostPhysics ? "Host physics" : "Per player";
            r.hint = c.hostPhysics ? "The host runs every fighter's physics, so every screen shows the same fight. Your own moves show about one ping late. From the next map."
                                   : "Each player runs their own fighter: no delay on your own moves, but hits and grabs can differ between screens. From the next map.";
            break;
    }
    return r;
}

}  // namespace lobby_detail

// The rows the format uses, in page order. Every format has Damage (Progression co-op shows it alone).
inline std::vector<LobbyRow> SettingsFor(const MatchConfig& c) {
    Format f = c.format;
    if (IsBotFormat(f)) return {LobbyRow::Time, LobbyRow::Gear, LobbyRow::Tier, LobbyRow::WaveBreak, LobbyRow::FriendlyFire, LobbyRow::Damage, LobbyRow::Sync};   // endless, back at a wave clear in the run's loadout
    if (IsBrawl(c)) return {LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::Damage, LobbyRow::ClearGround, LobbyRow::Sync};   // everyone bare at kBrawlTier's strength
    std::vector<LobbyRow> v = {LobbyRow::Limit, LobbyRow::Time, LobbyRow::Respawn, LobbyRow::Gear};
    v.push_back(LobbyRow::Tier);                     // the tier is every player's strength, with own gear too
    if (HasTeams(f)) v.push_back(LobbyRow::FriendlyFire);
    v.push_back(LobbyRow::Damage);
    if (f != Format::Duel) v.push_back(LobbyRow::ClearGround);
    if (HasTeams(f)) v.push_back(LobbyRow::MyTeam);
    v.push_back(LobbyRow::Sync);
    return v;
}

inline constexpr Format kModeOrder[kFormatCount] = {Format::FreeForAll, Format::Duel, Format::TwoTeams,
                                                    Format::ThreeTeams, Format::PlayersVsBots, Format::Zombies};

// The group of the i-th mode card (kModeOrder, then Brawl, then Progression): known before any scene, so the page can
// build each card in its group's grid.
inline ModeGroup ModeGroupAt(size_t i) {
    if (i < size_t(kFormatCount)) return IsBotFormat(kModeOrder[i]) ? ModeGroup::Coop : ModeGroup::Versus;
    return i == size_t(kFormatCount) ? ModeGroup::Versus : ModeGroup::Coop;   // Brawl; Progression
}

inline LobbyScene BuildLobbyScene(const LobbyLocal& l) {
    using namespace lobby_detail;
    LobbyScene s;
    s.me = l.myName;
    s.badge = "v" + l.version + " \xC2\xB7 protocol " + std::to_string(int(kProtocolRevision));
    switch (l.phase) {
        case LobbyPhase::Offline: s.pill = {"Steam not ready", style::kWarn}; break;
        case LobbyPhase::Idle: s.pill = {"Not in a lobby", style::kIron}; break;
        case LobbyPhase::Creating: s.pill = {"Creating a lobby", style::kWarn}; break;
        case LobbyPhase::Joining: s.pill = {"Joining a lobby", style::kWarn}; break;
        case LobbyPhase::InLobby: {
            if (l.host) { s.pill = {l.publicLobby ? "Hosting \xC2\xB7 public" : "Hosting \xC2\xB7 friends", style::kOk}; break; }
            std::string owner;
            for (const LobbyPlayer& p : l.players) if (p.host) owner = p.name;
            s.pill = {owner.empty() ? std::string("In a lobby") : "In " + owner + "'s lobby", style::kOk};
            break;
        }
    }
    s.idle = l.phase == LobbyPhase::Idle;
    s.in = l.phase == LobbyPhase::InLobby;
    if (!s.in) s.status = l.status;
    if (s.idle) {
        s.filters = FilterChips(l.filter);
        std::vector<size_t> shown;
        for (size_t i = 0; i < l.found.size(); ++i) if (PassesFilter(l.found[i], l.filter, l.maxPlayers)) shown.push_back(i);
        if (l.filter.mostPlayers)
            std::stable_sort(shown.begin(), shown.end(), [&](size_t a, size_t b) { return l.found[a].players > l.found[b].players; });
        if (!l.found.empty())
            s.browserCount = shown.size() == l.found.size() ? std::to_string(l.found.size()) + (l.found.size() == 1 ? " lobby" : " lobbies")
                           : std::to_string(shown.size()) + " of " + std::to_string(l.found.size()) + " lobbies shown";
        if (l.found.empty()) s.browserEmpty = "No public lobbies yet. REFRESH looks again.";
        else if (shown.empty()) s.browserEmpty = "No lobby matches these filters.";
        for (size_t at : shown) {
            if (s.browser.size() >= kBrowserRows) break;
            const LobbyFound& f = l.found[at];
            BrowserRow r;
            r.source = at;
            r.name = f.name.empty() ? "Unnamed lobby" : f.name;
            r.players = std::to_string(f.players) + "/" + std::to_string(l.maxPlayers);
            r.detail = f.map;
            if (!f.mode.empty()) r.detail += (r.detail.empty() ? "" : " \xC2\xB7 ") + f.mode;
            if (!f.compatible) r.note = {f.reason.empty() ? std::string("Incompatible") : f.reason, style::kWarn};
            else if (f.players >= l.maxPlayers) r.note = {"Full", style::kWarn};
            else { r.note = {"Join", style::kOk}; r.joinable = true; }
            s.browser.push_back(r);
        }
    }
    if (!s.in) return s;

    const MatchConfig& c = l.config;
    Format f = c.format;
    s.playersTitle = "PLAYERS " + std::to_string(l.players.size()) + "/" + std::to_string(l.maxPlayers);
    for (const LobbyPlayer& p : l.players) {
        RosterRow r;
        r.name = p.name.empty() ? "?" : p.name;
        r.host = p.host; r.ready = p.ready; r.me = p.me;
        if (HasTeams(f)) {
            int t = EffectiveTeam(p, f);
            r.team = TeamLabel(t);
            r.swatch = TeamColor(t);                                // none for Auto
            r.moved = ValidTeam(p.hostOverride, f);
            r.movable = l.host;
        }
        s.roster.push_back(r);
    }
    if (!l.host) {
        s.ready = true;
        for (const LobbyPlayer& p : l.players) if (p.me) s.readyOn = p.ready;
        s.readyLabel = s.readyOn ? "Ready \xC2\xB7 click to undo" : "Mark me ready";
    }
    s.balance = l.host && HasTeams(f);
    for (Format m : kModeOrder) s.modes.push_back({m, Upper(FormatName(m)), FormatBlurb(m), m == f && !IsBrawl(c) && !l.progression, l.host});
    {
        ModeCard b;
        b.format = Format::FreeForAll;
        b.name = Upper(ModeName(Format::FreeForAll, Gear::Fists));
        b.description = kBrawlBlurb;
        b.selected = IsBrawl(c) && !l.progression; b.enabled = l.host; b.brawl = true;
        s.modes.push_back(b);
    }
    {
        ModeCard p;
        p.name = "PROGRESSION (CO-OP)";
        p.description = "Stock Progression in the Tavern, together. Free Mode off; your own save is untouched.";
        p.selected = l.progression; p.enabled = l.host; p.progression = true;
        s.modes.push_back(p);
    }
    for (size_t i = 0; i < s.modes.size(); ++i) s.modes[i].group = ModeGroupAt(i);
    if (!l.progression) for (LobbyRow id : SettingsFor(c)) s.settings.push_back(Row(l, id));
    else s.settings.push_back(Row(l, LobbyRow::Damage));   // co-op fights take player damage too
    if (!l.host) s.hostOnly = "Only the host can change these.";
    size_t customChosen = l.modMaps.size();   // the picked custom map (none: l.modMaps.size())
    if (f != Format::Zombies)
        for (size_t i = 0; i < l.modMaps.size(); ++i) if (lobby_detail::SamePath(l.modMaps[i].path, l.mapPick)) customChosen = i;
    const size_t chosen = customChosen < l.modMaps.size() ? MapCount() : EffectiveMap(f, l.mapPick);
    for (size_t i = 0; i < MapCount() && !l.progression; ++i) {
        if (!kMaps[i].pick) continue;
        MapCard m;
        m.index = i;
        m.name = kMaps[i].display;
        m.tag = MapTag(kMaps[i]);
        m.allowed = MapAllowsFormat(kMaps[i], f);
        if (!m.allowed) m.reason = MapRefusal(f);
        m.selected = i == chosen;
        m.enabled = l.host && m.allowed;
        s.maps.push_back(m);
    }
    for (size_t i = 0; i < l.modMaps.size() && !l.progression; ++i) {   // modded maps follow the stock ones
        MapCard m;
        m.index = MapCount() + i;
        m.modded = true;
        m.path = l.modMaps[i].path;
        m.name = l.modMaps[i].display;
        m.tag = l.modMaps[i].pak.empty() ? "Modded map" : "Modded map \xC2\xB7 " + l.modMaps[i].pak;
        m.allowed = f != Format::Zombies;                              // the Abyss is the only Zombies map
        if (!m.allowed) m.reason = MapRefusal(f);
        m.selected = i == customChosen;
        m.enabled = l.host && m.allowed;
        s.maps.push_back(m);
    }
    std::vector<PeerBuild> others;
    for (const LobbyPlayer& p : l.players) if (!p.me) others.push_back({p.name, p.build});
    const std::string warn = BuildWarning(l.myBuild, others);
    s.buildMismatch = !warn.empty();
    s.start = l.host && MayStartMatch(l.myBuild, others, l.allowBuildMismatch);
    const bool modWait = l.host && customChosen < l.modMaps.size() && l.modPickPending;
    if (modWait) s.start = false;
    // The line beside START: what will be played.
    if (l.progression) s.summary = "PROGRESSION \xC2\xB7 the Tavern, together";
    else {
        s.summary = Upper(ModeName(f, c.gear));
        for (const MapCard& m : s.maps) if (m.selected) s.summary += " \xC2\xB7 " + m.name;
        if (!IsBrawl(c)) s.summary += " \xC2\xB7 " + std::string(GearName(c.gear));
        if (!IsBrawl(c) && c.gear != Gear::Own) s.summary += ", tier " + std::to_string(int(c.tier));
    }
    if (!l.host) s.waiting = "Waiting for the host";
    if (modWait) s.waiting = "Checking the map pack";
    if (s.buildMismatch) s.waiting = l.host && l.allowBuildMismatch ? warn + " (allowed: allow_build_mismatch=1)" : warn;
    return s;
}

}  // namespace hsm
