// Session orchestration on the game thread, pumped from BP_HalfSwordGameMode_C::ReceiveTick (menus included).
// Steam mode: the Steam lobby decides roles (owner hosts) and membership; the host's level changes pull every
//   member along (Travel), and clients may not leave the menu on their own while joined.
// Gameplay (Phase 2): the host is the authority for up to 8 fighters. Slot 0 is the host's stock fighter; every
//   client gets a mod-spawned Willie on the host wearing the client's own passport (relayed as a SaveGame blob),
//   driven by the client's input and aim. Clients fight as their own stock player 0, mirror host poses, and
//   show every other fighter as a brainless puppet with the right loadout. Decisions live in hsm_director.h.
// Matches (Phase 3): in fight levels (not the Tavern hub) with 2+ players, or players-vs-bots, the host runs
//   hsm::Match: team picks from the lobby, kill credit from weapon hit delegates, friendly-fire filtering,
//   respawns on team sides, bot waves, and a MatchInfo/Kill feed every client renders in the same HUD. Stock
//   NPCs that are not part of the match are removed so every machine sees the same fight.
// Owner sync (protocol rev 9, the default; session/hsm_authority.h): instead of the host simulating everyone, every
//   machine simulates its own fighter natively (the host also its bots) and streams it (game::OwnerStreams); every
//   fighter owned elsewhere is a neutralised stand-in servoed to that stream (game::StandIns). Blows on a stand-in
//   become hit claims replayed by the victim's owner through the game's own damage code (game::Claims). The host
//   relays the streams, routes the claims and referees the match. It is the only mode: the host-simulates-all flow
//   (sync_mode=host) was removed in 0.8.0, and a machine without the script VM hook refuses to start a session.
#include "config.h"
#include "util/hsm_names.h"
#include "util/hsm_sanitize.h"
#include "gameplay/adapter.h"
#include "gameplay/abyss_coop.h"
#include "gameplay/chest_lids.h"
#include "gameplay/claims.h"
#include "gameplay/combat_sync.h"
#include "gameplay/equip_sync.h"
#include "gameplay/fx_sync.h"
#include "gameplay/gear.h"
#include "gameplay/grab_sync.h"
#include "gameplay/mirror.h"
#include "gameplay/mod_maps.h"
#include "gameplay/owner_sync.h"
#include "gameplay/object_sync.h"
#include "gameplay/damage_pin.h"
#include "gameplay/paint_defer.h"
#include "gameplay/recorder.h"
#include "gameplay/save_guard.h"
#include "gameplay/shared_dice.h"
#include "gameplay/severs.h"
#include "gameplay/standin_gore.h"
#include "gameplay/spectate.h"
#include "gameplay/lobby.h"
#include "gameplay/session.h"
#include "gameplay/travel.h"
#include "gameplay/voice.h"
#include "hsm_chat.h"
#include "hsm_owner.h"
#include "hsm_protocol.h"
#include "hsm_drivelean.h"
#include "hsm_input.h"
#include "hsm_inputdelay.h"
#include "session/hsm_map_reset.h"
#include "sync/hsm_remote.h"
#include "log.h"
#include "ue4ss_load.h"
#include "ue/profiler.h"
#include "match/hsm_draw.h"
#include "match/hsm_giveup_action.h"
#include "match/hsm_hud_scene.h"
#include "match/hsm_input_mode.h"
#include "match/hsm_lobby_chat.h"
#include "match/hsm_maps.h"
#include "match/hsm_occlusion.h"
#include "match/hsm_match.h"
#include "match/hsm_respawn_retry.h"
#include "match/hsm_spectate.h"
#include "net/report_client.h"
#include "net/update_check.h"
#include "net/steam_transport.h"
#include "net/transport.h"
#include "session/hsm_authority.h"
#include "session/hsm_mesh.h"
#include "session/hsm_peer_ping.h"
#include "session/hsm_route.h"
#include "session/hsm_detour.h"
#include "session/hsm_dice.h"
#include <random>
#include "session/hsm_migration.h"
#include "session/hsm_autoflow.h"
#include "session/hsm_blob.h"
#include "session/hsm_budget.h"
#include "sync/hsm_handoff.h"
#include "sync/hsm_lockon.h"
#include <tuple>
#include "session/hsm_build.h"
#include "session/hsm_combat.h"
#include "session/hsm_gvas.h"
#include "session/hsm_director.h"
#include "session/hsm_level_gate.h"
#include "session/hsm_moderation.h"
#include "session/hsm_reconnect.h"
#include "session/hsm_report.h"
#include "session/hsm_roster.h"
#include "session/hsm_spawn_anchor.h"
#include "session/hsm_spawn.h"
#include "session/hsm_travel_gate.h"
#include "agent/bridge.h"
#include "agent/vmhook.h"
#include "paths.h"
#include "test/harness.h"
#include "ui/chat.h"
#include "ui/hud.h"
#include "ui/menu.h"
#include "ui/pause.h"
#include "ue/engine.h"
#include "ue/hook.h"
#include "ue/script_hook.h"
#include <windows.h>
#include <array>
#include <chrono>
#include <deque>
#include <atomic>
#include <cmath>
#include <cstring>
#include <ctime>
#include <map>
#include <unordered_map>
#include <set>
#include <memory>
#include <optional>
#include <algorithm>

namespace hsm {
using namespace hsm::ue;
namespace {

constexpr const char* kGameModeClass = "BP_HalfSwordGameMode_C";
constexpr uint64_t kHostKey = 0;                 // roster key of the host itself (SteamIDs are never 0)
constexpr double kNoSpawnerRingCm = 400.0;       // spawner-less maps: candidates on a ring this far around the host
constexpr double kSideRingCm = 900.0;            // spawner-less maps: team sides on a ring this far around the host
constexpr double kTeleportDistance = 300.0;
static_assert(game::kMaxLobbyMembers == kMaxPlayers, "a Steam lobby holds exactly the players a session can seat (a server lobby one more)");
constexpr uint8_t kFirstBotEntity = kMaxFighters + 1, kLastBotEntity = uint8_t(kMaxWillies);
static_assert(Match::kMaxBotsAlive <= kLastBotEntity - kFirstBotEntity + 1, "every live bot needs an entity");
constexpr uint32_t kInfoIntervalMs = 1000, kScanIntervalMs = 1000, kTintRetryMs = 500;
constexpr uint32_t kDrawSampleMs = 250;      // the Duel draw check's look at the fighters (hsm_draw.h)
constexpr int kTintTries = 8;
constexpr uint32_t kKeepAliveMs = 1000;          // the host's traffic when it streams nothing (HostLossMonitor); a client's, always (PeerSilenceMonitor)
constexpr uint32_t kVitalsDeathMs = 1500;        // owner sync: a fighter its owner reports dead this long without a DeathReport
constexpr const char* kHubLevel = "/Game/Maps/Map_Hub_Tavern_Frank";
constexpr const char* kMenuLevel = "/Game/Maps/Menus/Map_Menu_Startup";

uint32_t NowMs() { return uint32_t(GetTickCount64()); }
bool IsMenuLevel(const std::string& level) { return level.find("/Maps/Menus/") != std::string::npos || level.find("Map_Menu_") != std::string::npos; }
double Dist(const FVector& a, const FVector& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
// When a message arrived (the transport's receive stamp, on game::SteadyNowMs's clock), not when this frame polled it:
// a long frame no longer counts as network jitter in a stand-in's pose buffer. No stamp, or one from the future: now.
double ArrivalMs(const NetEvent& ev) {
    const double now = game::SteadyNowMs();
    return ev.rxMs > 0 && ev.rxMs <= now ? ev.rxMs : now;
}

// What a level gate that timed out was still waiting for, in log words.
const char* GateWaitingName(LevelSpawnGate::Waiting w) {
    switch (w) {
        case LevelSpawnGate::Waiting::Manager: return "a level manager";
        case LevelSpawnGate::Waiting::PlayerSpawnedCleared: return "\"Player Spawned\" to clear";
        case LevelSpawnGate::Waiting::PlayerSpawned: return "\"Player Spawned\"";
        case LevelSpawnGate::Waiting::Pawn: return "a player pawn";
        case LevelSpawnGate::Waiting::Nothing: break;
    }
    return "nothing";
}

// A Hello from another protocol revision passes validation so the host can answer it with a Reject.
bool IsForeignHello(const uint8_t* d, size_t n) {
    return n >= 4 && uint16_t(d[0] | d[1] << 8) == kMagic && d[2] != kProtocolRevision && d[3] == uint8_t(MsgType::Hello);
}

bool ValidateForHost(const uint8_t* d, size_t n, uint8_t) {
    MsgType t; if (!PeekType(d, n, t)) return IsForeignHello(d, n);
    switch (t) {
        case MsgType::Hello: { Hello m; return Decode(d, n, m); }
        case MsgType::Input: { InputFrame m; return Decode(d, n, m); }
        case MsgType::Ready: { Ready m; return Decode(d, n, m); }
        case MsgType::BlobChunk: { BlobChunk m; return Decode(d, n, m); }
        case MsgType::Chat: { ChatLine m; return Decode(d, n, m); }
        case MsgType::Ping: case MsgType::Pong: { Ping m; bool p; return DecodePing(d, n, m, p); }
        case MsgType::Pose: { PoseMsg m; return Decode(d, n, m); }
        case MsgType::Vitals: { VitalsMsg m; return Decode(d, n, m); }
        case MsgType::DeathReport: { DeathReport m; return Decode(d, n, m); }
        case MsgType::GiveUp: { GiveUpMsg m; return Decode(d, n, m); }
        case MsgType::HitClaim: { HitClaimMsg m; return Decode(d, n, m); }
        case MsgType::SeverClaim: { combat::SeverClaimMsg m; return Decode(d, n, m); }
        case MsgType::SeverNotice: { combat::SeverNoticeMsg m; return Decode(d, n, m); }
        case MsgType::StuckBlade: { combat::StuckBladeMsg m; return Decode(d, n, m); }
        case MsgType::RemoteWrite: { sync::RemoteWriteMsg m; return Decode(d, n, m); }
        case MsgType::GrabState: { GrabStateMsg m; return Decode(d, n, m); }
        case MsgType::BodyState: { BodyStateMsg m; return Decode(d, n, m); }
        case MsgType::ObjectAnnounce: { ObjectAnnounceMsg m; return Decode(d, n, m) && m.flags == kAnnounceProposal; }   // clients only propose
        case MsgType::ObjectEvent: { ObjectEventMsg m; return Decode(d, n, m); }
        case MsgType::PropLease: { PropLeaseMsg m; return Decode(d, n, m) && m.action != LeaseAction::Grant; }
        case MsgType::PaintFx: { combat::PaintFxMsg m; return Decode(d, n, m); }
        case MsgType::Equip: { combat::EquipStateMsg m; return Decode(d, n, m); }
        case MsgType::ReportShare: { ReportShareMsg m; return Decode(d, n, m); }
        case MsgType::MeshHeard: { MeshHeardMsg m; return Decode(d, n, m); }
        case MsgType::PingTable: { PingTableMsg m; return Decode(d, n, m); }
        case MsgType::Voice: { VoiceMsg m; return Decode(d, n, m); }
        case MsgType::HandOff: { sync::HandOffMsg m; return Decode(d, n, m) && !m.full; }   // rev 32: a client writes entries, never the table
        case MsgType::HandOffAsk: { sync::HandOffAskMsg m; return Decode(d, n, m); }
        default: return false;
    }
}
bool ValidateForClient(const uint8_t* d, size_t n, uint8_t) {
    MsgType t; if (!PeekType(d, n, t)) return false;
    switch (t) {
        case MsgType::Welcome: { Welcome m; return Decode(d, n, m); }
        case MsgType::Reject: { Reject m; return Decode(d, n, m); }
        case MsgType::Snapshot: { Snapshot m; return Decode(d, n, m); }
        case MsgType::Event: { Event m; return Decode(d, n, m); }
        case MsgType::Travel: { Travel m; return Decode(d, n, m); }
        case MsgType::ModMap: { ModMapMsg m; return Decode(d, n, m); }
        case MsgType::MapReset: { MapResetMsg m; return Decode(d, n, m); }
        case MsgType::LevelSeed: { dice::LevelSeedMsg m; return Decode(d, n, m); }
        case MsgType::Spawn: { Spawn m; return Decode(d, n, m); }
        case MsgType::Despawn: { Despawn m; return Decode(d, n, m); }
        case MsgType::BlobChunk: { BlobChunk m; return Decode(d, n, m); }
        case MsgType::MatchInfo: { MatchInfo m; return Decode(d, n, m); }
        case MsgType::Kill: { Kill m; return Decode(d, n, m); }
        case MsgType::Chat: { ChatLine m; return Decode(d, n, m); }
        case MsgType::CombatEvent: { CombatEvent m; return Decode(d, n, m); }
        case MsgType::Ping: case MsgType::Pong: { Ping m; bool p; return DecodePing(d, n, m, p); }
        case MsgType::Pose: { PoseMsg m; return Decode(d, n, m); }
        case MsgType::Vitals: { VitalsMsg m; return Decode(d, n, m); }
        case MsgType::HitClaim: { HitClaimMsg m; return Decode(d, n, m); }
        case MsgType::HitFx: { HitFxMsg m; return Decode(d, n, m); }
        case MsgType::SeverClaim: { combat::SeverClaimMsg m; return Decode(d, n, m); }
        case MsgType::SeverNotice: { combat::SeverNoticeMsg m; return Decode(d, n, m); }
        case MsgType::StuckBlade: { combat::StuckBladeMsg m; return Decode(d, n, m); }
        case MsgType::RemoteWrite: { sync::RemoteWriteMsg m; return Decode(d, n, m); }
        case MsgType::GrabState: { GrabStateMsg m; return Decode(d, n, m); }
        case MsgType::BodyState: { BodyStateMsg m; return Decode(d, n, m); }
        case MsgType::ObjectAnnounce: { ObjectAnnounceMsg m; return Decode(d, n, m) && m.flags == kAnnounceCanonical; }   // only the host's binds
        case MsgType::ObjectEvent: { ObjectEventMsg m; return Decode(d, n, m); }
        case MsgType::PropLease: { PropLeaseMsg m; return Decode(d, n, m) && m.action == LeaseAction::Grant; }
        case MsgType::PropCensus: { PropCensusMsg m; return Decode(d, n, m); }
        case MsgType::PaintFx: { combat::PaintFxMsg m; return Decode(d, n, m); }
        case MsgType::Equip: { combat::EquipStateMsg m; return Decode(d, n, m); }
        case MsgType::ReportShare: { ReportShareMsg m; return Decode(d, n, m); }
        case MsgType::PeerMap: { PeerMapMsg m; return Decode(d, n, m); }
        case MsgType::Voice: { VoiceMsg m; return Decode(d, n, m); }
        case MsgType::VoiceMode: { VoiceModeMsg m; return Decode(d, n, m); }
        case MsgType::RouteTable: { RouteTableMsg m; return Decode(d, n, m); }
        case MsgType::Forward: { ForwardMsg m; return Decode(d, n, m); }
        case MsgType::VoteState: { VoteStateMsg m; return Decode(d, n, m); }
        case MsgType::HandOff: { sync::HandOffMsg m; return Decode(d, n, m); }
        case MsgType::HandOffAsk: { sync::HandOffAskMsg m; return Decode(d, n, m); }
        case MsgType::RelayInput: { RelayInput m; return Decode(d, n, m); }
        default: return false;
    }
}

// An actor the mod spawned, with its object-array slot so a stale pointer is never dereferenced.
struct ActorRef {
    UObject* obj = nullptr; int32_t index = -1;
    void Set(UObject* o) { obj = o; index = ObjectIndex(o); }
    void Clear() { obj = nullptr; index = -1; }
};

class SessionImpl final : public Session {
public:
    explicit SessionImpl(const Config& c) : cfg_(c) {
        lobby_.onEntered = [this] { OnLobbyEntered(); };
        lobby_.onLeft = [this] {
            bool strandedInFight = welcomed_ && InFightLevel();   // a client whose host left: back to the menu
            ResetNetwork("left lobby");
            if (strandedInFight) { HSM_LOG("client: lobby gone; returning to the main menu"); game::OpenLevel(lastGameMode_, kMenuLevel, true, ""); }
        };
        lobby_.onOwnerChanged = [this](steam::SteamId old, steam::SteamId now) { return OnLobbyOwnerChanged(old, now); };
        lobby_.onMemberJoined = [this](steam::SteamId id) { if (steamNet_ && lobby_.IsHost()) steamNet_->AllowPeer(id); };
        lobby_.onMemberLeft = [this](steam::SteamId id) { lobbyGate_.Forget(id); meshPeers_.Forget(id); if (steamNet_) steamNet_->DropPeer(id); };
        lobby_.onChat = [this](steam::SteamId from, const std::string& text) { OnLobbyChat(from, text); };
        lobby_.onPresence = [](const std::string& name, bool joined) { ui::ShowLine("HSMMod", style::kBone, (name.empty() ? "?" : name) + (joined ? " joined" : " left")); };
        ui::ChatSetup(cfg_.chatKey, [this](bool on) {   // in a menu level the chat closing gives the menu its cursor back
                adapter_.CaptureInput(on); if (!on && !InFightLevel()) adapter_.ShowPauseCursor(); }, [this] { return adapter_.GamePaused() || adapter_.PauseMenuOpen(); });
    }

    bool Start() { return true; }   // the transport is created when a Steam lobby is entered (role=server: ServerLobbyTick)

    game::Lobby& lobby() override { return lobby_; }
    bool steamMode() const override { return true; }   // 0.8.0: Steam is the only transport (direct IP was removed)
    // role=server: the server opens a public Steam lobby itself; players find it in the lobby browser and Steam relays the
    // traffic (no port forwarding). Needs Steam running and signed in on that machine.
    bool SteamServer() const { return cfg_.role == Role::Server; }
    void StartOnMap(const std::string& path) override {
        if (!lastGameMode_ || (net_ && !IsHost())) return;
        if (net_ && !BuildsAllowStart()) return;
        HSM_LOG("session: starting on %s", path.c_str());
        // The level manager destroys every spawner whose "Required Combatants Amount" exceeds foes + 1 (decomp
        // BP_LevelManager.cpp:385), the player's included: keep at least 2 until our fighters exist (StopStockFoes).
        RememberFoes();
        adapter_.PrepareFreeModeFight(std::max(savedFoes_, 2));
        HostTravel(path);
    }
    // The lobby's Progression card: the stock Tavern hub with Free Mode off, so the game runs its own Progression
    // (the level manager and OnLevelLoaded key off the GI's "Free Mode Activated").
    void StartProgression() override {
        if (!lastGameMode_ || (net_ && !IsHost())) return;
        if (net_ && !BuildsAllowStart()) return;
        HSM_LOG("session: starting Progression in the hub");
        adapter_.ClearFreeMode();
        HostTravel(kHubLevel);
    }

    // Before the Half Sword game mode ticks (splash/startup screens), any actor tick answers the bridge with the
    // commands that need no session state.
    // Wall time of one Tick (the pump): our own share of a frame, against the frame time the game mode reports.
    void NoteTickCost(double ms) {
        if (!sampling_ || tickMs_.size() >= kFrameSamples) return;
        if (tickMs_.empty()) {   // the window starts: VM and ProcessEvent hook time from here
            vmNsAtWindow_ = GetScriptHookStats().hookNs;
            const ProcessEventHookStats pe = GetProcessEventHookStats();
            peNsAtWindow_ = pe.hookNs;
            peCallsAtWindow_ = pe.calls;
            peAllAtWindow_ = pe.all;
            const ReflectionCacheStats rc = GetReflectionCacheStats();
            lookupsAtWindow_ = rc.hits + rc.misses;
            lookupMissesAtWindow_ = rc.misses;
        }
        tickMs_.push_back(ms);
    }

    // The pump's cost by part, summed over the frames of a health window (as tickMs_ samples them): which of our own
    // parts grows when the frames slow down. Each part's time includes the game code it runs (a replayed claim's Get
    // Damage counts under claims, a stand-in's native cut under severs or net).
    enum PumpPart : uint8_t { kPumpNet, kPumpStreams, kPumpServo, kPumpClaims, kPumpSevers, kPumpGore, kPumpGrabs, kPumpProps,
                              kPumpEquip, kPumpMatch, kPumpHandOff, kPumpDrive, kPumpParts };
    static constexpr const char* kPumpNames[kPumpParts] = {"net", "streams", "servo", "claims", "severs", "gore", "grabs",
                                                           "props", "equip", "match", "handoff", "drive"};
    static constexpr const char* kPumpScopes[kPumpParts] = {"HSMMod pump: net", "HSMMod pump: streams", "HSMMod pump: servo",
        "HSMMod pump: claims", "HSMMod pump: severs", "HSMMod pump: gore", "HSMMod pump: grabs", "HSMMod pump: props",
        "HSMMod pump: equip", "HSMMod pump: match", "HSMMod pump: handoff", "HSMMod pump: drive"};
    template <class F> void Timed(PumpPart part, F&& f) {
        ue::prof::Scope scope(kPumpScopes[part], ue::prof::Kind::kMod);
        if (!sampling_ || tickMs_.size() >= kFrameSamples) { f(); return; }
        const ReflectionCacheStats r0 = GetReflectionCacheStats();
        const auto t0 = std::chrono::steady_clock::now();
        f();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        const ReflectionCacheStats r1 = GetReflectionCacheStats();
        pumpLookups_[part] += (r1.hits + r1.misses) - (r0.hits + r0.misses);
        pumpMs_[part] += ms;
        pumpMaxMs_[part] = std::max(pumpMaxMs_[part], ms);
    }
    void ClearPumpCost() { pumpMs_.fill(0); pumpMaxMs_.fill(0); pumpLookups_.fill(0); driveFed_ = driveSkipped_ = 0; }

    // " | pump ms/frame: net A streams B ... (max P x ms)": P is the part with the longest single run in the window.
    std::string PumpLine(double frames) const {
        const auto& kNames = kPumpNames;
        std::string line = " | pump ms/frame (and lookups/frame):";
        char b[64];
        size_t worst = 0;
        for (size_t i = 0; i < kPumpParts; ++i) {
            // "/N": reflection lookups per frame made inside that part (FindFunction/FindProperty/Call by name).
            std::snprintf(b, sizeof b, " %s %.2f/%.0f", kNames[i], pumpMs_[i] / frames, double(pumpLookups_[i]) / frames);
            line += b;
            if (pumpMaxMs_[i] > pumpMaxMs_[worst]) worst = i;
        }
        std::snprintf(b, sizeof b, " (max %s %.1f ms)", kNames[worst], pumpMaxMs_[worst]);
        return line + b;
    }

    // The profiler's hotkey and frame mark (every game mode tick; PumpAgentEarly polls the key, every 50 ms, while none ticks).
    void ProfilerTick(bool frame) {
        std::string msg = frame ? ue::prof::Frame() : std::string();
        if (msg.empty() && !ui::Typing()) msg = ue::prof::PollHotkey();
        ShowProfilerMessage(msg);
    }
    void ShowProfilerMessage(const std::string& msg) {
        if (msg.empty()) return;
        MessageBeep(ue::prof::Recording() ? MB_ICONASTERISK : MB_OK);
        ui::ShowLine("HSMMod", style::kBone, msg);
    }

    void PumpAgentEarly() {
        uint32_t now = NowMs();
        if (now - lastModeTick_ < 1000 || now - lastEarlyPump_ < 50) return;
        lastEarlyPump_ = now;
        ProfilerTick(false);
        // Paused game (actors frozen, widgets still tick) in a level we already know: the full command set is safe.
        if (lastGameMode_ && ObjectAt(lastGameModeIndex_) == lastGameMode_ && adapter_.World() == lastGameMode_) {   // never reads a freed mode
            agent::Tick([this](const AgentCommand& c) { return HandleAgent(c); });
            return;
        }
        agent::Tick([this](const AgentCommand& c) {
            if (c.verb == "ping") return std::string("{\"ok\":true,\"early\":true}");
            if (c.verb == "widgets") return WidgetsJson();
            if (c.verb == "exec" || c.verb == "shot") { AgentCommand x = c; return HandleAgent(x); }
            if (c.verb == "prof") return ue::prof::Command(c.args);
            return std::string("{\"ok\":false,\"error\":\"game mode not ticking yet; try widgets/key/click/exec\"}");
        });
    }

    // Owner sync: the pause menu pauses the world (Esc: Willie_BP.cpp:11091 opens UI_Pause_Eng, whose Construct pauses),
    // which stops the GameMode tick and with it this player's stream. The others saw them freeze, and a client whose host
    // paused went back to the main menu after 7 s (logs 4, 7, 8). The check in Tick cannot see a pause that outlasts the
    // frame; widgets still tick while paused (UI_Pause_Eng has its own Tick), so one of them lifts it. The menu stays
    // open. Esc while it is open: Willie's pause action (it checks only IsGamePaused) opens a second menu, but both
    // menus' Ticks see Esc held that frame and close, so Esc still closes the menu.
    void LiftPauseFromWidgetTick() {
        const uint32_t now = NowMs();
        if (!pauseLiftArmed_ || now - lastModeTick_ < 200 || now - lastPauseCheck_ < 50) return;
        lastPauseCheck_ = now;
        if (!(lastGameMode_ && ObjectAt(lastGameModeIndex_) == lastGameMode_ && adapter_.World() == lastGameMode_)) return;
        if (!adapter_.GamePaused()) return;
        adapter_.SetGamePaused(false);
        if (++unpaused_ % 100 == 1) HSM_LOG("session: game paused (pause menu?) during an owner-sync match; unpaused (%u time(s))", unpaused_);
    }

    // The pause menu's REPORT A BUG button (ui/pause.h): in a session fight with reports on. A click closes the menu as its
    // Continue does and opens the chat with "/bug " typed. Called from the game mode tick and from the widgets' Tick hook (a
    // paused world stops the first), so it throttles itself.
    void PauseReportTick() {
        const uint32_t now = NowMs();
        if (now - lastPauseButton_ < 30) return;
        lastPauseButton_ = now;
        if (!selfTestOk_ || !(lastGameMode_ && ObjectAt(lastGameModeIndex_) == lastGameMode_ && adapter_.World() == lastGameMode_)) return;
        UObject* menu = adapter_.PauseMenu();
        if (!menu) { ui::PauseMenuTick(nullptr, nullptr, false); return; }
        const bool fight = InFightLevel();
        switch (ui::PauseMenuTick(menu, InSession() && fight ? this : nullptr, report::Enabled() && net_ && fight)) {
            case ui::PauseAction::None: return;
            case ui::PauseAction::Resume:
                adapter_.ContinueFromPauseMenu();
                return;
            case ui::PauseAction::ReportBug:
                HSM_LOG("session: pause menu REPORT A BUG clicked");
                adapter_.ContinueFromPauseMenu();
                ui::ChatOpenWith("/bug ");
                return;
            case ui::PauseAction::Leave:
                adapter_.ContinueFromPauseMenu();
                LeaveSession();
                return;
        }
    }

    std::string WidgetsJson() {
        std::string j = "{\"ok\":true,\"widgets\":[";
        bool first = true;
        UClass* uw = FindClass("UserWidget");
        for (int32_t i = 0, n = ObjectCount(); i < n && uw; ++i) {
            UObject* o = ObjectAt(i);
            if (!o || IsDefaultObject(o) || !IsA(o, uw)) continue;
            Call v(o, "IsInViewport");
            if (!v.invoke() || !v.get<bool>("ReturnValue")) continue;
            j += (first ? "" : ",") + JsonString(ObjectName(reinterpret_cast<UObject*>(ClassOf(o))));
            first = false;
        }
        return j + "]}";
    }

    void Tick(UObject* gameMode, float dt) {
        lastModeTick_ = NowMs();
        fighterOfFrame_.clear();
        spawnIgnoreState_ = SpawnIgnoreState::InTick;
        struct EndOfTick { SpawnIgnoreState& s; ~EndOfTick() { s = SpawnIgnoreState::Outside; } } endOfTick{spawnIgnoreState_};
        sampling_ = net_ && InFightLevel();                               // sampled only where the health line is logged
        if (!sampling_) { frameMs_.clear(); tickMs_.clear(); ClearPumpCost(); }
        else if (frameMs_.size() < kFrameSamples) frameMs_.push_back(double(dt) * 1000.0);
        if (!selfTested_) {
            selfTested_ = true;
            selfTestOk_ = SelfTest();
            test::Begin(selfTestOk_);
            if (!selfTestOk_) { HSM_LOG("session: self-test failed; disabling multiplayer for this run"); ResetNetwork("self-test failed"); return; }
            InstallClientSuppression();
            game::InstallTravelHook([this](const std::string& l, bool a, const std::string& o) { return OnOpenLevel(l, a, o); });
            // Blueprint-to-Blueprint calls (hits, damage) never reach ProcessEvent; this hooks the VM's executor.
            // Fails closed: on an unverified build the hooks stay inert and the log says why.
            const bool vmHook = InstallScriptHook();
            HSM_LOG("session: script VM hook %s", vmHook ? "installed" : "unavailable");
            // Swapped-out armour outlives the paint tasks that read it (T5), in single player too; paint_defer=0 turns it off.
            if (vmHook && cfg_.paintDefer) game::InstallPaintDefer();
            else if (!cfg_.paintDefer) HSM_LOG("session: armour paint guard off (paint_defer=0)");
            // The map's Blueprint random numbers agree across a session (the seed comes with each travel); inert until one is set.
            game::shareddice::Install();
        }
        if (!selfTestOk_) return;
        if (steamMode()) lobby_.Tick();
        if (steamMode()) MigrationTick();
        if (SteamServer()) ServerLobbyTick();
        // Level change first: everything after this point must use the current world, never the old game mode.
        bool levelChanged = gameMode != lastGameMode_;
        if (levelChanged) FlushReflectionCache();     // the old level's classes are gone (again each retry: harmless)
        if (levelChanged) ShowProfilerMessage(ue::prof::StopCapture());
        if (!adapter_.Resolve(gameMode)) return;     // retried next tick, with the level change still pending
        if (levelChanged) { lastGameMode_ = gameMode; lastGameModeIndex_ = ObjectIndex(gameMode); OnLevelLoaded(); }
        inFight_ = InFightLevel();
        if (hostPending_) { hostPending_ = false; if (!ScriptHookMissing()) BecomeHost(); }
        agent::Tick([this](const AgentCommand& c) { return HandleAgent(c); });
        AutoRun();
        if (!inFight_ && menuInputAt_ && NowMs() >= menuInputAt_) {   // twice: after the menu level's own setup, then once more
            const bool first = !menuInputRepeat_;
            const bool done = first ? adapter_.MenuInputBack() : (adapter_.ShowPauseCursor(), true);
            menuInputAt_ = first && done ? NowMs() + 2000 : 0;
            menuInputRepeat_ = first && done;
            if (done) HSM_LOG("session: menu level: cursor and UI input given back");
        }
        if (!inFight_) ui::Tick(*this, gameMode);   // the main menu exists only in menu levels (its scan walks every object)
        else if (FighterLevel()) ui::LeaveMenus();
        if (net_ && FighterLevel()) { TickOwnBuild(); ProbeGiveUpFunctions(); }
        EnsureLocalFighter();
        test::Tick(adapter_, InFightLevel(), dt);
        if (cfg_.record) recorder_.Tick(adapter_, gameMode, FighterLevel(), currentLevel_, adapter_.PlayerWillie(0), dt);
        ApplyHeldInput();
        if (mirror_.Active() && net_) mirror_.Command({"off"}, adapter_, true, InFightLevel());   // offline bench only
        mirror_.Tick(adapter_, dt);
        InputProbeTick();
        bool joined = net_ && (steamMode() ? lobby_.InLobby() : true);
        game::TickPaintDefer();
        adapter_.TickDeferredDestroys();
        saveGuard_.Tick(adapter_, joined, MatchLevel());   // a session never touches the player's own save
        game::TickDamagePin(joined, SessionDamage(), IsHost());       // one player damage for everyone, the host's
        game::TickPhysicsPin(joined && !IsHost() ? info_.physics : std::array<float, kPinnedPhysics>{ -1.f, -1.f, -1.f });   // and its gore and bodies
        adapter_.SetDestroyAware(net_ != nullptr);
        adapter_.SetSubclassWeapons(true);   // offline too, so the mirror bench sees the weapons owner sync sees
        stockFlowGuard_ = joined && MatchLevel();
        pauseLiftArmed_ = stockFlowGuard_;
        adapter_.SuppressStockScreens(joined && MatchLevel(), true);
        if (stockFlowGuard_ && adapter_.ResetTimeDilation() && ++timeReset_ % 100 == 1)
            HSM_LOG("session: world time was not normal speed during a session match; reset (%u time(s))", timeReset_);
        if (stockFlowGuard_ && adapter_.GamePaused()) {   // owner sync: a paused game stops this player's stream
            adapter_.SetGamePaused(false);
            if (++unpaused_ % 100 == 1) HSM_LOG("session: game was paused during an owner-sync match; unpaused (%u time(s))", unpaused_);
        }
        PauseReportTick();
        ReportTick();   // before the solo return: reports work in the menus and offline too (R1)
        UpdateNoticeTick();
        if (std::optional<std::string> line = ui::TakeOutgoing(); line && !ReportCommandLine(*line) && net_)
            if (!IsHost() || !ModerationLine(*line, 0, false)) SendChat(*line);
        if (IsServer()) ConsoleTick();   // before the no-session return: bans can be listed and lifted while idle
        if (!net_) { if (localFighter_) VoiceIdleTick(); ui::HudTick({}, {}); ui::ChatDrop("session over"); return; }
        if (IsHost()) HostTick(dt); else ClientTick(dt);
        AbyssTick();
        VoiceTick();
        TintTick();
        // Twice: after the level's own setup, then once more; never while the chat holds the keyboard (it gives it back).
        // The pause menu owns the input mode while it is open (cursor shown once); closing it restores the game's, once.
        {
            const bool typing = ui::Typing();
            const bool menu = FighterLevel() && adapter_.PauseMenuOpen();
            InputModeState ms;
            ms.typing = typing; ms.menuOpen = menu; ms.wasMenuOpen = pauseMenuWas_; ms.fightLevel = FighterLevel();
            ms.delayedFixDue = inputFixAt_ && NowMs() >= inputFixAt_;
            if (ms.delayedFixDue && !typing) { inputFixAt_ = inputFixRepeat_ ? 0 : NowMs() + 3000; inputFixRepeat_ = !inputFixRepeat_; }
            switch (ChooseInputMode(ms)) {
                case InputMode::GameOnly: adapter_.RestoreGameInput(); HSM_LOG("session: game input mode restored"); break;
                case InputMode::GameAndUI: adapter_.ShowPauseCursor(); HSM_LOG("session: pause menu open; cursor shown"); break;
                case InputMode::Leave: break;
            }
            if (!typing) pauseMenuWas_ = menu;
        }
        if (net_) SpectateTick();
        // Once a game run, in the first session fight: how to report a bug (the relay must be set).
        if (bugHintAt_ && NowMs() >= bugHintAt_) {
            bugHintAt_ = 0; bugHintShown_ = true;
            if (report::Enabled())
                ui::ShowLine("HSMMod", style::kBone, "Found a bug? Open the chat, type /bug and what happened, press Enter. Your log goes with it.");
        }
        if (net_ && InFightLevel() && NowMs() >= nextHealth_) { nextHealth_ = NowMs() + 5000; LogHealth(); VoiceHealth(); }
        if (info_.phase != chatPhase_) {   // the results: a line being typed is dropped, a new one may be opened
            if (info_.phase == uint8_t(MatchPhase::Results)) ui::ChatDrop("match over");
            chatPhase_ = info_.phase;
        }
        GiveUpTick();
        DropsTick();
        MapResetTick();
        HudScene hud = BuildHud();
        ui::HudTick(hud, NameTags(hud), Format(info_.format));
        ui::ChatTick(hud);
    }

    // One line every 5 s in a session fight (see LogOwnerHealth).
    void LogHealth() { LogOwnerHealth(); }

    // Mesh: off prints nothing. Host: what each client says it hears directly and how many relays that saved. Client: the
    // direct peers, streams that arrived and were used, anything else from a non-host peer that was dropped.
    void LogMeshHealth(const char* who) {
        LogPingHealth(who);
        if (IsHost() ? !MeshHost() : !meshOn_) return;
        if (IsHost()) {
            std::string heard;
            for (uint8_t l = 1; l < kMaxFighters; ++l) {
                if (!PeerOfSlot(l)) continue;
                unsigned mask = 0;
                for (uint8_t f = 0; f < kMaxFighters; ++f) if (meshMasks_.Hears(l, f)) mask |= 1u << f;
                char b[32]; std::snprintf(b, sizeof b, " slot%u 0x%03x;", l, mask);
                heard += b;
            }
            HSM_LOG("%s: mesh: hears directly:%s relays skipped %u", who, heard.empty() ? " (no clients)" : heard.c_str(), meshRelaySkipped_);
        } else {
            HSM_LOG("%s: mesh: %zu direct peers, %u messages received directly, %u dropped from non-host peers, %u direct copies sent, "
                    "%u second copies dropped, hearing 0x%02x", who, meshSend_.size(), meshRx_, meshDropped_, meshDirectTx_, meshDupes_,
                    unsigned(meshHearing_.Mask(NowMs())));
        }
        meshRx_ = meshDropped_ = meshRelaySkipped_ = 0;
        meshDirectTx_ = meshDupes_ = 0;
        if (!IsHost()) LogDetourHealth(who);
    }

    static std::string PingCell(uint16_t rttMs, uint16_t steamMs, uint8_t lossPct) {
        char b[64];
        if (rttMs == kPingUnknown) std::snprintf(b, sizeof b, "-");
        else std::snprintf(b, sizeof b, "%u ms", rttMs);
        std::string s = b;
        if (steamMs != kPingUnknown) { std::snprintf(b, sizeof b, " (steam %u)", steamMs); s += b; }
        if (lossPct) { std::snprintf(b, sizeof b, " loss %u%%", lossPct); s += b; }
        return s;
    }

    // Per-player pings (peer_ping=1): a client's own row (the host and every other client, median of the last pings, what
    // Steam measures for the same link, lost pings); the host's table of every client's last report.
    void LogPingHealth(const char* who) {
        if (IsHost()) {
            if (!PeerMapHost() || !cfg_.peerPing || pingTables_.empty()) return;
            for (const auto& [from, table] : pingTables_) {
                std::string row;
                for (const PingTableEntry& e : table.entries)
                    row += std::string(row.empty() ? "" : "; ") + (e.slot == kHostSlot ? std::string("host") : "slot" + std::to_string(e.slot)) + " " +
                           PingCell(e.rttMs, e.steamMs, e.lossPct);
                HSM_LOG("%s: ping: slot%u -> %s", who, from, row.empty() ? "(nothing measured)" : row.c_str());
            }
            LogRoutePlan(who);
            return;
        }
        if (!peerPingOn_ || !net_) return;
        const PingTableMsg m = MyPingTable(NowMs());
        std::string row;
        for (const PingTableEntry& e : m.entries)
            row += std::string(row.empty() ? "" : "; ") + (e.slot == kHostSlot ? std::string("host") : "slot" + std::to_string(e.slot)) + " " +
                   PingCell(e.rttMs, e.steamMs, e.lossPct);
        HSM_LOG("%s: ping: slot%u -> %s", who, mySlot_, row.empty() ? "(nothing measured yet)" : row.c_str());
    }

    // Detour routing, step R0: every kPingTableGapMs the host plans each client pair's fastest path from the ping tables
    // (session/hsm_route.h) and logs what changed. Nothing is sent differently.
    void LogRoutePlan(const char* who) {
        const uint32_t now = NowMs();
        if (!DueAt(now, nextRoutePlan_)) return;
        nextRoutePlan_ = now + kPingTableGapMs;
        auto name = [](uint8_t r) { return r == kRouteDirect ? std::string("direct") : r == kRouteHost ? std::string("host") : "via slot" + std::to_string(r); };
        auto ms = [](uint32_t v) { return v == kRouteNone ? std::string("-") : std::to_string(v); };
        static const char* const kWhy[] = {"new pair", "faster", "path lost"};
        const std::vector<RouteChange> changes = routePlanner_.Plan(routeLinks_, ConnectedSlots(), now);
        if (!changes.empty() && DetourHost()) detourDirty_ = true;
        for (const RouteChange& c : changes) {
            if (!routeLogged_) {
                routeLogged_ = true;
                HSM_LOG("%s: route plan: %s (one-way ms)", who, DetourHost() ? "detours on: each client gets its routes" : "logged only, nothing is sent differently");
            }
            HSM_LOG("%s: route plan: slot%u <-> slot%u %s -> %s (direct %s, host %s, chosen %s; %s)", who, c.a, c.b,
                    name(c.from).c_str(), name(c.to).c_str(), ms(c.directMs).c_str(), ms(c.hostMs).c_str(), ms(c.toMs).c_str(),
                    kWhy[size_t(c.why)]);
        }
    }

    // Owner sync: what this machine streamed, how every stand-in tracks its owner, and every claim's fate.
    void LogOwnerHealth() {
        const char* who = IsHost() ? "host" : "client";
        HSM_LOG("%s: health (owner sync) streamed %llu poses %llu vitals; dropped %u foreign streams, rejected %u claims; remote writes %llu made, %llu dropped; stand-ins %d%s",
                who, static_cast<unsigned long long>(streams_.PosesSent()), static_cast<unsigned long long>(streams_.VitalsSent()),
                streamsDropped_, claimsRejected_, static_cast<unsigned long long>(remoteApplied_), static_cast<unsigned long long>(remoteDropped_),
                standIns_.Count(), IsHost() ? "" : (adapter_.PlayerWillie(0) ? ", own fighter ok" : ", own fighter MISSING"));
        streamsDropped_ = claimsRejected_ = 0;
        remoteApplied_ = remoteDropped_ = 0;
        LogMeshHealth(who);
        if (staleLife_) { HSM_LOG("%s: %u claims or reports dropped for naming an old body", who, staleLife_); staleLife_ = 0; }
        HSM_LOG("%s: %s", who, FrameAndPingLine().c_str());
        HSM_LOG("%s: %s", who, CensusLine().c_str());
        if (std::string line = standIns_.HealthLine(); !line.empty()) HSM_LOG("%s: %s", who, line.c_str());
        HSM_LOG("%s: %s", who, claims_.HealthLine().c_str());
        standIns_.SetAudit(cfg_.helpImprove);   // T2: stand-in fields written here between two Vitals
        claims_.SetAudit(cfg_.helpImprove);   // C0: the hit audit only with HELP IMPROVE THE MOD on (0.8.0-pre7)
        if (std::string line = claims_.AuditLine(); !line.empty()) HSM_LOG("%s: %s", who, line.c_str());
        HSM_LOG("%s: %s", who, fx_.HealthLine().c_str());
        if (std::string line = game::PaintDeferHealthLine(); !line.empty()) HSM_LOG("%s: %s", who, line.c_str());
        if (std::string line = game::shareddice::HealthLine(); !line.empty()) HSM_LOG("%s: %s", who, line.c_str());
        HSM_LOG("%s: %s", who, equip_.HealthLine().c_str());
        HSM_LOG("%s: %s", who, severs_.HealthLine().c_str());
        HSM_LOG("%s: %s", who, gore_.HealthLine().c_str());
        HSM_LOG("%s: %s", who, grabs_.HealthLine().c_str());
        HSM_LOG("%s: %s", who, props_.HealthLine().c_str());
        if (InAbyss()) HSM_LOG("%s: %s", who, abyss_.HealthLine().c_str());
    }

    // Every fighter in the level and its state, next to the frame time: what grows when the game slows down (bodies left
    // behind, fighters on the ground or bleeding). Two object scans per 5 s window; their time ends the line.
    std::string CensusLine() {
        const auto t0 = std::chrono::steady_clock::now();
        int total = 0, dead = 0, fallen = 0, out = 0, stand = 0;
        float bleeding = 0;
        for (UObject* w : adapter_.AllWillies()) {
            ++total;
            if (standIns_.IsStandIn(w)) ++stand;
            VitalsMsg v;
            if (!adapter_.ReadVitals(w, 0, v)) continue;
            dead += (v.flags & 1) ? 1 : 0;
            fallen += (v.flags & 2) ? 1 : 0;
            out += (v.flags & 4) ? 1 : 0;
            bleeding += v.bleeding;
        }
        const size_t weapons = adapter_.WeaponActors().size();
        const double scanMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        char b[256];
        std::snprintf(b, sizeof b, "census: fighters %d (stand-ins %d) dead %d fallen %d knocked out %d, bleeding total %.2f; "
                      "weapons %zu, objects %d; stand-in timeline calls blocked %u; scan %.1f ms",
                      total, stand, dead, fallen, out, double(bleeding), weapons, ObjectCount(), timelinesBlocked_, scanMs);
        timelinesBlocked_ = 0;
        std::string line = b;
        VitalsMsg mine;
        if (UObject* me = adapter_.PlayerWillie(0); me && adapter_.ReadVitals(me, 0, mine)) {
            std::snprintf(b, sizeof b, " | mine:%s%s%s health %.0f bleeding %.2f", (mine.flags & 1) ? " dead" : "",
                          (mine.flags & 2) ? " fallen" : "", (mine.flags & 4) ? " knocked out" : "", double(mine.health), double(mine.bleeding));
            line += b;
        }
        return line;
    }

    // Frame time (the game mode's DeltaSeconds) since the previous call, and the round trip to every peer: a long frame
    // and a slow link both look like lag on the other screen; this line tells them apart.
    std::string FrameAndPingLine() {
        char b[160];
        std::string line;
        if (frameMs_.empty()) line = "frames none";
        else {
            std::vector<double> ms = frameMs_;
            double sum = 0;
            for (double v : ms) sum += v;
            const size_t k = std::min(ms.size() - 1, (ms.size() * 99) / 100);
            std::nth_element(ms.begin(), ms.begin() + std::ptrdiff_t(k), ms.end());
            std::snprintf(b, sizeof b, "frames %zu avg %.1f ms p99 %.1f ms", ms.size(), sum / double(ms.size()), ms[k]);
            line = b;
        }
        frameMs_.clear();
        // Our share: the pump (stand-in servo, streams, our replays of claims including the game code they run), the VM
        // hook callbacks (claims, fx, ...) and the other ProcessEvent hooks (the hit hook on every contact, the tick and
        // timeline filters; the pump runs inside one of them and is taken out there, DiscountHookTime). Not counted: the
        // detours' lookup for calls nothing hooks. The rest of a frame is the game's own (physics, ragdolls, blood, rendering).
        if (!tickMs_.empty()) {
            double sum = 0, mx = 0;
            for (double v : tickMs_) { sum += v; mx = std::max(mx, v); }
            const double frames = double(tickMs_.size());
            const ScriptHookStats vm = GetScriptHookStats();
            const double vmMs = double(vm.hookNs - vmNsAtWindow_) / 1e6 / frames;
            const ProcessEventHookStats pe = GetProcessEventHookStats();
            const double peMs = double(pe.hookNs - peNsAtWindow_) / 1e6 / frames;
            const double peCalls = double(pe.calls - peCallsAtWindow_) / frames;
            const double peAll = double(pe.all - peAllAtWindow_) / frames;
            std::snprintf(b, sizeof b, " | mod tick avg %.2f max %.1f ms, vm hooks %.2f ms/frame, other hooks %.2f ms/frame (%.0f calls of %.0f)",
                          sum / frames, mx, vmMs, peMs, peCalls, peAll);
            line += b;
            // Which hooks (two-player test 13: 65 ms/frame at 1100 calls late in a match; two-player logs 6 to 9: 2 to 3.5 ms
            // in fights, from 15 to 25 us a call).
            if (peMs >= 1.0) line += " [top hooks: " + TopHookFunctions(3, frames) + "]";
            else TopHookFunctions(0, frames);
            const ReflectionCacheStats rc = GetReflectionCacheStats();   // FindFunction/FindProperty: misses walk names
            std::snprintf(b, sizeof b, ", lookups %.0f/frame (%.1f walked)", double(rc.hits + rc.misses - lookupsAtWindow_) / frames,
                          double(rc.misses - lookupMissesAtWindow_) / frames);
            line += b;
            line += PumpLine(frames);
            if (driveFed_ + driveSkipped_) {   // axis events played on other players' fighters (drive_lean spares some)
                std::snprintf(b, sizeof b, " | drive events fed %.1f skipped %.1f /frame", double(driveFed_) / frames, double(driveSkipped_) / frames);
                line += b;
            }
        }
        if (IsHost() && HostPhysics() && !hostDelay_.Off()) {
            std::snprintf(b, sizeof b, " | host input delay %d ms (target %d, clients %d)", hostDelay_.delayMs(), hostDelay_.targetMs(), hostDelay_.clients());
            line += b;
        }
        tickMs_.clear();
        ClearPumpCost();
        line += " | rtt";
        size_t links = 0;
        auto add = [&](uint32_t peer, int slot) {
            std::snprintf(b, sizeof b, " slot%d %u ms", slot, net_->RoundTripMs(peer));
            line += b; ++links;
        };
        if (net_ && IsHost()) for (const auto& [peer, p] : peers_) add(peer, p.slot);
        else if (net_ && hostPeer_) add(hostPeer_, kHostSlot);
        if (!links) line += " none";
        return line;
    }

    // The host stamps the sender's real slot and relays every line; it shows its own lines directly.
    void SendChat(const std::string& text) {
        ChatLine c; c.fromSlot = 0; c.text = SanitizeChat(text);
        if (c.text.empty()) return;
        if (IsHost()) { net_->Broadcast(kChanReliable, Encode(c), true); ui::ShowLine(MyName(), ChatNameInk(info_, 1), c.text); }
        else if (hostPeer_) net_->Send(hostPeer_, kChanReliable, Encode(c), true);
    }

    // ------------------------------------------------------------------ in-game bug reports (net/report_client.h)
    // "/bug <text>" (or "/report <text>"), "/sendlog" and "/nolog" typed in the chat; true: it was one (never sent as chat).
    bool ReportCommandLine(const std::string& line) {
        const ReportCommand cmd = ParseReportCommand(line);
        if (cmd.kind == ReportCommandKind::None) return false;
        auto say = [](const std::string& msg) { ui::ShowLine("HSMMod", style::kBone, msg); };
        const uint32_t now = NowMs();
        if (!report::Enabled()) { say("Bug reports are off (reports= and report_url= in HSMMod.ini)."); return true; }
        if (cmd.kind == ReportCommandKind::NoLog) {          // answers an offer, so it works while a send is busy too (R10)
            if (reportOffer_.Open(now)) say("OK, your log stays here.");
            reportOffer_.Decline();
            autoAttachDue_ = false;
            return true;
        }
        if (report::Busy()) { say("Still sending the last report..."); return true; }
        if (cmd.kind == ReportCommandKind::SendLog) {
            std::string id, token;
            if (!reportOffer_.Take(now, id, token)) say("No bug report is waiting for your log.");
            else if (report::Attach(id, token, ReportFactsNow())) say("Sending your HSMMod log (Steam IDs, IPs and your Windows user name removed)...");
            else say("Your log was not sent: the request could not be started.");
        } else if (cmd.text.empty()) {
            say("Type /bug and what went wrong, e.g. /bug my sword vanished after respawn");
        } else {
            say(SubmitReport(cmd.text));
        }
        return true;
    }

    // The menu's Report button and /bug: the one place the Enabled/Busy/cooldown checks live. Returns what to tell
    // the player (also when it was refused).
    std::string SubmitReport(const std::string& text) override {
        const uint32_t now = NowMs();
        if (!report::Enabled()) return "Bug reports are off (reports= and report_url= in HSMMod.ini).";
        if (report::Busy()) return "Still sending the last report...";
        if (text.empty()) return "Type what went wrong first.";
        if (reportedOnce_ && now - lastReportAt_ < kReportCooldownMs)
            return "Please wait " + std::to_string((kReportCooldownMs - (now - lastReportAt_)) / 1000 + 1) + " s before another report.";
        if (!report::Submit(text, ReportFactsNow(), ReportJob::Report)) return "Bug report not sent: the request could not be started.";
        reportSession_ = sessionGen_; reportIsManual_ = true;   // the cooldown starts when the relay answers (ReportTick)
        return "Sending your bug report... The text becomes a public GitHub issue; your log (Steam IDs, IPs and your Windows user name removed) stays private.";
    }

    std::string ReportStatus() const override { return reportStatus_; }
    uint32_t ReportStatusSeq() const override { return reportStatusSeq_; }

    // The lobby chat box (menu.cpp). The text goes out as a Steam lobby chat message: the session transport carries slots
    // and no names before a match, and only to members the host has welcomed. Commands run here, as in the match chat.
    void LobbyChatLine(const std::string& typed) override {
        const std::string line = SanitizeChat(typed);
        if (line.empty() || ReportCommandLine(line)) return;
        if (ParseModCommand(line).kind != ModCommandKind::None) {   // kick, ban, vote: the session host runs them
            if (net_ && IsHost()) ModerationLine(line, 0, false);
            else if (net_ && hostPeer_) SendChat(line);
            else ui::ShowLine("HSMMod", style::kBone, "Kick, ban and vote commands work once you are connected to the host.");
            return;
        }
        if (!lobby_.InLobby()) { ui::ShowLine("HSMMod", style::kBone, "Join a lobby to chat."); return; }
        if (!lobbyChatOut_.Allow(NowMs())) { ui::ShowLine("HSMMod", style::kBone, "Slow down."); return; }
        if (!lobby_.SendChat(line)) ui::ShowLine("HSMMod", style::kBone, "The chat line could not be sent.");
    }

    // A line from a member (ours comes back the same way, and is limited when it is sent). Shown in the chat history,
    // which the multiplayer page and the match chat share.
    void OnLobbyChat(steam::SteamId from, const std::string& text) {
        const bool mine = from == steam::MySteamId();
        if (!mine && !lobbyGate_.Allow(from, NowMs())) return;
        std::string name;
        Rgba ink = style::kIron;
        for (const game::LobbyMember& m : lobby_.members())
            if (m.id == from) { name = m.name; ink = LobbyChatInk(lobby_.config().format, m.team, m.hostOverride); }
        if (name.empty()) name = steam::PersonaName(from);
        ui::ShowLine(name.empty() ? "?" : name, ink, text);
    }

    // ------------------------------------------------------------------ kick, ban, votekick (session/hsm_moderation.h)
    // Runs on the host. `from`: the client peer that typed the line, 0 for this machine (the player host's chat, or the
    // role=server console). true: it was a moderation command (answered; never shown as chat).
    bool ModerationLine(const std::string& line, uint32_t from, bool console) {
        const ModCommand cmd = ParseModCommand(line, console);
        if (cmd.kind == ModCommandKind::None) return false;
        if (from && !peers_.count(from)) return true;
        if (cmd.kind == ModCommandKind::Say) { if (net_) ServerSay(cmd.target); else Reply(from, "Nobody is connected."); return true; }
        if (cmd.kind == ModCommandKind::Players) { ListPlayers(from); return true; }
        if (cmd.kind == ModCommandKind::VoteKick || cmd.kind == ModCommandKind::VoteBan || cmd.kind == ModCommandKind::Yes || cmd.kind == ModCommandKind::No) { VoteCommand(cmd, from); return true; }
        if (!IsAdmin(from)) {
            Reply(from, IsServer() ? (cfg_.serverVoteKick ? "Only the server's admins can do that. /votekick or /voteban <name> starts a vote."
                                                          : "Only the server's admins can do that.")
                                   : cfg_.serverVoteKick ? "Only the host can kick or ban. /votekick <name> starts a vote."
                                                         : "Only the host can kick or ban.");
            return true;
        }
        LoadBans();
        if (cmd.kind == ModCommandKind::Bans) { ListBans(from); return true; }
        if (cmd.kind == ModCommandKind::Unban) {
            const std::string key = bans_.Resolve(cmd.target);
            const BanEntry* b = key.empty() ? nullptr : bans_.Find(key, UnixNow());
            if (!b || b->kick) { Reply(from, "No ban matches '" + cmd.target + "'. /bans lists them."); return true; }
            const std::string who = b->name.empty() ? key : b->name;
            bans_.Remove(key);
            SaveBans();
            HSM_LOG("moderation: %s lifted the ban on %s", IssuerName(from).c_str(), key.c_str());
            Reply(from, who + " is unbanned.");
            return true;
        }
        uint32_t peer = 0;
        if (!net_ || !FindPeerTarget(cmd.target, from, peer)) return true;   // answered
        if (peer == from) { Reply(from, "You can't kick or ban yourself."); return true; }
        const std::string name = NameOfSlot(peers_.at(peer).slot);
        const std::string key = BanKeyOf(peer);
        const std::string by = from || console || IsServer() ? " by " + IssuerName(from) : std::string();
        if (cmd.kind == ModCommandKind::Kick) {
            HSM_LOG("moderation: %s kicked slot %u (%s)", IssuerName(from).c_str(), peers_.at(peer).slot, key.c_str());
            ServerSay(name + " was kicked" + by + ".");
            DropMember(peer, RejectReason::Kicked);
        } else {
            if (key.empty()) { Reply(from, "Can't ban " + name + ": no Steam ID or address is known. /kick works."); return true; }
            if (!bans_.Add(key, name, cmd.seconds ? UnixNow() + cmd.seconds : 0, false)) { Reply(from, "The ban list is full (HSMMod.bans)."); return true; }
            SaveBans();
            HSM_LOG("moderation: %s banned slot %u (%s) %s", IssuerName(from).c_str(), peers_.at(peer).slot, key.c_str(), DurationText(cmd.seconds).c_str());
            ServerSay(name + " was banned " + DurationText(cmd.seconds) + by + ".");
            DropMember(peer, RejectReason::Banned);
        }
        return true;
    }

    // /votekick, /yes, /no (from the chat or the in-match menu), in every hosted session since 0.7.0-pre6; /voteban
    // (7 days, two thirds) on role=server hosts only. The player host votes too (as kHostVoterKey) but kicks directly
    // instead of starting one; it can't be voted out (not a peer).
    static constexpr uint64_t kHostVoterKey = 1;
    void VoteCommand(const ModCommand& cmd, uint32_t from) {
        if (!cfg_.serverVoteKick) { Reply(from, IsServer() ? "Votekick is off on this server (votekick=0)." : "Votekick is off in this session (votekick=0)."); return; }
        const bool start = cmd.kind == ModCommandKind::VoteKick || cmd.kind == ModCommandKind::VoteBan, ban = cmd.kind == ModCommandKind::VoteBan;
        if (!from && (IsServer() || !localFighter_)) { Reply(from, "The console kicks and bans directly: kick <name>, ban <name> 7d."); return; }
        if (!from && start) { Reply(from, "You're the host: kick or ban from the Esc menu, or type /kick <name> or /ban <name>."); return; }
        if (ban && !IsServer()) { Reply(from, "Votebans are for servers only. /votekick <name> starts a vote to kick."); return; }
        const uint64_t me = from ? peers_.at(from).key : kHostVoterKey;
        const std::string meName = from ? NameOfSlot(peers_.at(from).slot) : MyName();
        if (!start) {
            const bool yes = cmd.kind == ModCommandKind::Yes;
            if (!vote_.Active()) { Reply(from, "No vote is running."); return; }
            if (!vote_.Cast(me, yes)) { Reply(from, "You can't vote on this one."); return; }
            voteDirty_ = true;
            ServerSay(meName + " voted " + (yes ? "yes" : "no") + " (" + std::to_string(vote_.Yes()) + " of " +
                      std::to_string(vote_.Needed()) + " yes needed).");
            return;
        }
        uint32_t peer = 0;
        if (!FindPeerTarget(cmd.target, from, peer)) return;
        if (peer == from) { Reply(from, "You can't vote yourself out."); return; }
        if (IsAdmin(peer)) { Reply(from, "Admins can't be voted out."); return; }
        std::vector<uint64_t> players;
        for (const auto& [pp, q] : peers_) players.push_back(q.key);
        if (localFighter_ && !IsServer()) players.push_back(kHostVoterKey);
        const std::string target = NameOfSlot(peers_.at(peer).slot);
        switch (vote_.Begin(me, peers_.at(peer).key, players, NowMs(), ban)) {
            case VoteKick::Start::Ok:
                voteTargetName_ = target;
                voteDirty_ = true;
                HSM_LOG("moderation: slot %u started a vote to %s slot %u", peers_.at(from).slot, ban ? "ban" : "kick", peers_.at(peer).slot);
                ServerSay(meName + (ban ? " votes to ban " + target + " " + DurationText(kVoteBanOutSeconds) : " votes to kick " + target) +
                          ": vote in the Esc menu, or type /yes or /no (" + std::to_string(vote_.Needed()) +
                          " yes needed, " + std::to_string(kVoteKickMs / 1000) + " s).");
                break;
            case VoteKick::Start::Running: Reply(from, "A vote is already running: /yes or /no."); break;
            case VoteKick::Start::Cooldown: Reply(from, "You started a vote recently; wait a couple of minutes."); break;
            case VoteKick::Start::TooFew: Reply(from, "A vote needs at least 3 players."); break;
            case VoteKick::Start::Self: Reply(from, "You can't vote yourself out."); break;
        }
    }

    // Each host tick: rejected peers are dropped after their Reject had time to arrive, the vote is decided, and expired
    // bans leave the file.
    void ModerationTick() {
        const uint32_t now = NowMs();
        for (size_t i = 0; i < pendingDrops_.size();) {
            if (!DueAt(now, pendingDrops_[i].second)) { ++i; continue; }
            net_->Disconnect(pendingDrops_[i].first);
            pendingDrops_.erase(pendingDrops_.begin() + long(i));
        }
        const VoteKick::Outcome o = vote_.Tick(now);
        if (o == VoteKick::Outcome::Passed) {
            uint32_t peer = 0;
            for (const auto& [pp, q] : peers_) if (q.key == vote_.target()) peer = pp;
            if (peer) {
                LoadBans();
                const std::string key = BanKeyOf(peer);
                const bool ban = vote_.Ban();
                const int64_t secs = ban ? kVoteBanOutSeconds : kVoteKickOutSeconds;
                if (!key.empty() && bans_.Add(key, voteTargetName_, UnixNow() + secs, false)) SaveBans();
                HSM_LOG("moderation: the %s passed; slot %u is out %s (%s)", ban ? "voteban" : "vote", peers_.at(peer).slot, DurationText(secs).c_str(), key.c_str());
                ServerSay("Vote passed: " + voteTargetName_ + (ban ? " is banned " : " is kicked ") + DurationText(secs) + ".");
                DropMember(peer, ban ? RejectReason::Banned : RejectReason::Kicked);
            }
        } else if (o == VoteKick::Outcome::Failed) {
            ServerSay("Vote failed: " + voteTargetName_ + " stays.");
        } else if (o == VoteKick::Outcome::Cancelled) {
            ServerSay(std::string("The vote to ") + (vote_.Ban() ? "ban " : "kick ") + voteTargetName_ + " ended (a player left).");
        }
        if (DueAt(now, nextBanPrune_)) {
            nextBanPrune_ = now + 60000;
            if (bansLoaded_ && bans_.Prune(UnixNow())) SaveBans();
        }
        VoteBroadcast(now);
    }

    // The running vote as `key` sees it (its own vote), for the clients' menus.
    VoteStateMsg VoteStateFor(uint64_t key, uint32_t now) const {
        VoteStateMsg m;
        if (!vote_.Active()) return m;
        for (const auto& [pp, q] : peers_) if (q.key == vote_.target()) m.target = q.slot;
        if (m.target == kVoteNone) return VoteStateMsg{};
        auto u8 = [](size_t v) { return uint8_t(std::min<size_t>(v, kMaxPeerMap)); };
        m.voters = u8(vote_.Voters()); m.yes = u8(vote_.Yes()); m.no = u8(vote_.No()); m.needed = u8(vote_.Needed());
        m.secondsLeft = uint8_t(std::min<uint32_t>(vote_.SecondsLeft(now), 60));
        m.mine = int8_t(vote_.VoteOf(key));
        m.ban = vote_.Ban() ? 1 : 0;
        return m;
    }
    // Each client hears of the vote when it changes, once a second while it runs, and once when it ends.
    void VoteBroadcast(uint32_t now) {
        const bool active = vote_.Active();
        if (!active && !voteWasActive_) return;
        if (active && voteWasActive_ && !voteDirty_ && !DueAt(now, voteSentAt_ + 1000)) return;
        voteDirty_ = false; voteSentAt_ = now; voteWasActive_ = active;
        for (const auto& [peer, p] : peers_) net_->Send(peer, kChanReliable, Encode(VoteStateFor(p.key, now)), true);
    }

    // role=server: admin commands written to HSMMod.cmd (the Pelican egg forwards its console there), read each second.
    // The file is renamed before it is read, so a line appended meanwhile lands in a fresh file for the next round.
    void ConsoleTick() {
        const uint32_t now = NowMs();
        if (!DueAt(now, nextConsolePoll_)) return;
        nextConsolePoll_ = now + 1000;
        const std::wstring path = InstanceFilePath(L"HSMMod", L".cmd"), work = path + L".work";
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        if (!MoveFileExW(path.c_str(), work.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
        std::string text;
        if (FILE* f = _wfopen(work.c_str(), L"rb")) {
            text.resize(16384);
            text.resize(std::fread(text.data(), 1, text.size(), f));
            std::fclose(f);
        }
        DeleteFileW(work.c_str());
        size_t pos = 0;
        for (int lines = 0; pos < text.size() && lines < 32; ++lines) {
            size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            std::string line = SanitizeChat(text.substr(pos, end - pos));
            pos = end + 1;
            if (line.empty()) continue;
            HSM_LOG("console: > %s", line.c_str());
            if (!ModerationLine(line, 0, true)) HSM_LOG("console: unknown command; try players, kick <name>, ban <name> [30m|2h|7d|perm], unban <name>, bans, say <text>");
        }
    }

    bool IsAdmin(uint32_t from) {
        if (!from) return true;                        // the player host, or the server console
        if (!IsServer()) return false;
        auto it = peers_.find(from);
        if (it == peers_.end() || (it->second.key >> 63)) return false;   // a key with the top bit has no Steam ID to check
        if (!adminsParsed_) { admins_ = ParseAdminList(cfg_.serverAdmins); adminsParsed_ = true; }
        return std::find(admins_.begin(), admins_.end(), it->second.key) != admins_.end();
    }

    std::string IssuerName(uint32_t from) const {
        if (from) { auto it = peers_.find(from); return it == peers_.end() ? std::string("?") : NameOfSlot(it->second.slot); }
        return IsServer() ? std::string("the server") : MyName();
    }

    // What a ban names: the Steam ID; empty when it is not known.
    std::string BanKeyOf(uint32_t peer) {
        auto it = peers_.find(peer);
        if (it == peers_.end()) return {};
        return (it->second.key >> 63) ? std::string() : SteamBanKey(it->second.key);
    }

    // "#slot" or a name; false (and the issuer told why) when no single connected player matches.
    bool FindPeerTarget(const std::string& query, uint32_t from, uint32_t& peer) {
        std::vector<ModPlayer> players;
        for (const auto& [pp, q] : peers_) players.push_back({q.slot, NameOfSlot(q.slot)});
        uint8_t slot = 0;
        const TargetMatch m = FindTarget(query, players, slot);
        if (m == TargetMatch::NoMatch) { Reply(from, "No player matches '" + query + "'. /players lists them."); return false; }
        if (m == TargetMatch::Ambiguous) { Reply(from, "More than one player matches '" + query + "'; use their #number (/players)."); return false; }
        for (const auto& [pp, q] : peers_) if (q.slot == slot) { peer = pp; return true; }
        return false;
    }

    void ListPlayers(uint32_t from) {
        if (!net_ || !IsHost()) { Reply(from, "Not hosting a session."); return; }
        if (!IsServer()) Reply(from, "#0 " + MyName() + " (host)");
        if (peers_.empty()) { Reply(from, "No players connected."); return; }
        for (const auto& [pp, q] : peers_) {
            std::string line = "#" + std::to_string(q.slot) + " " + NameOfSlot(q.slot) + " (" + std::to_string(net_->RoundTripMs(pp)) + " ms)";
            if (!from) line += " " + BanKeyOf(pp);       // this machine only: the ID or address to ban
            Reply(from, line);
        }
    }

    void ListBans(uint32_t from) {
        const int64_t now = UnixNow();
        if (bans_.Prune(now)) SaveBans();
        const std::vector<const BanEntry*> b = bans_.Bans();
        if (b.empty()) { Reply(from, "No bans."); return; }
        const size_t shown = std::min<size_t>(b.size(), 8);
        for (size_t i = 0; i < shown; ++i) {
            const std::string left = b[i]->until ? DurationText(b[i]->until - now).replace(0, 4, "") + " left" : std::string("permanent");
            Reply(from, "#" + std::to_string(i + 1) + " " + (b[i]->name.empty() ? std::string("?") : b[i]->name) + " " + b[i]->key + ", " + left);
        }
        if (b.size() > shown) Reply(from, "... and " + std::to_string(b.size() - shown) + " more (HSMMod.bans). /unban #n or a name.");
        else Reply(from, "/unban #n or a name lifts one.");
    }

    // An answer to whoever typed the command: a client gets a SERVER chat line only it sees, this machine its chat/log.
    void Reply(uint32_t to, const std::string& text) {
        if (!to) { HSM_LOG("moderation: %s", text.c_str()); ui::ShowLine("HSMMod", style::kBone, text); return; }
        if (!net_) return;
        ChatLine c; c.fromSlot = kChatSystemSlot; c.text = text;
        net_->Send(to, kChanReliable, Encode(c), true);
    }

    // A SERVER line everyone in the session sees.
    void ServerSay(const std::string& text) {
        HSM_LOG("moderation: says: %s", text.c_str());
        ui::ShowLine("SERVER", style::kBone, SanitizeChat(text));
        if (!net_) return;
        ChatLine c; c.fromSlot = kChatSystemSlot; c.text = text;
        net_->Broadcast(kChanReliable, Encode(c), true);
    }

    // Kicked or banned: the client is told why, leaves the roster now, and its connection closes a moment later.
    void DropMember(uint32_t peer, RejectReason why) {
        auto it = peers_.find(peer);
        if (it == peers_.end()) return;
        const uint8_t slot = it->second.slot;
        const uint64_t key = it->second.key;
        vote_.Left(key);
        RejectPeer(peer, why);
        peerSilence_.Forget(peer);
        peers_.erase(it);
        MeshRosterChanged(slot);
        props_.SlotLeft(slot);
        RemoveMember(slot, key);                       // no rejoin reservation for a kicked player
    }

    // Reject first; the disconnect follows kRejectLingerMs later so the Reject is not lost with the closed session.
    void RejectPeer(uint32_t peer, RejectReason why) {
        Reject r; r.reason = why;
        net_->Send(peer, kChanReliable, Encode(r), true);
        for (const auto& d : pendingDrops_) if (d.first == peer) return;
        pendingDrops_.push_back({peer, NowMs() + kRejectLingerMs});
    }

    // A banned (or just kicked) Steam ID; null when this peer may join.
    const BanEntry* BannedEntry(const NetEvent& ev) {
        LoadBans();
        return ev.steamId ? bans_.Find(SteamBanKey(ev.steamId), UnixNow()) : nullptr;
    }

    static int64_t UnixNow() { return int64_t(std::time(nullptr)); }

    void LoadBans() {
        if (bansLoaded_) return;
        bansLoaded_ = true;
        std::string text;
        if (FILE* f = _wfopen(InstanceFilePath(L"HSMMod", L".bans").c_str(), L"rb")) {
            text.resize(256u << 10);
            text.resize(std::fread(text.data(), 1, text.size(), f));
            std::fclose(f);
        }
        const size_t n = bans_.Parse(text, UnixNow());
        if (n) HSM_LOG("moderation: %zu ban(s) loaded from HSMMod.bans", n);
    }

    void SaveBans() {
        const std::wstring path = InstanceFilePath(L"HSMMod", L".bans"), tmp = path + L".tmp";
        const std::string text = bans_.Serialize();
        FILE* f = _wfopen(tmp.c_str(), L"wb");
        if (!f) { HSM_LOG("moderation: could not write HSMMod.bans"); return; }
        const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
        std::fclose(f);
        if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) HSM_LOG("moderation: could not write HSMMod.bans");
    }

    // A client the host refused (full, other version, kicked, banned): back to the menu, out of the Steam lobby, told why.
    // Without this the client sat in the lobby waiting for a Welcome that never came (N2).
    void OnRejected(RejectReason why) {
        const char* msg = "The host refused the connection.";
        switch (why) {
            case RejectReason::BadRevision: msg = "The host runs another HSMMod version; everyone needs the same one."; break;
            case RejectReason::BuildMismatch: msg = "The host runs another build of Half Sword; everyone needs the same one."; break;
            case RejectReason::Full: msg = "The session is full."; break;
            case RejectReason::Malformed: break;
            case RejectReason::Kicked: msg = "The host kicked you from the lobby. You can join again."; break;
            case RejectReason::Banned: msg = "You are banned from this lobby (by the host, an admin or a vote)."; break;
        }
        HSM_LOG("client: rejected by the host (reason %d); leaving", int(why));
        const bool fight = InFightLevel();
        welcomed_ = false;                             // the lobby's onLeft must not open the menu a second time
        if (steamMode()) lobby_.Leave();
        ResetNetwork("rejected by the host");
        ui::ShowLine("HSMMod", style::kBone, msg);
        notice_ = msg;                                 // shown on the multiplayer page, which opens for it (menu.cpp)
        if (fight) game::OpenLevel(lastGameMode_, kMenuLevel, true, "");
    }

    std::string TakeNotice() override { std::string n; n.swap(notice_); return n; }

    // ------------------------------------------------------------------ the multiplayer menus (match/hsm_mp_scene.h)
    bool InSession() const override { return net_ && (IsHost() || welcomed_); }

    MpVoice VoiceView() override {
        const uint32_t now = NowMs();
        voiceMeterUntil_ = now + 1500;   // the menu is open: keep the microphone recording for the meter
        MpVoice v;
        const bool inLobby = steamMode() && lobby_.InLobby();
        v.session = inLobby || net_;
        v.host = net_ ? IsHost() : inLobby ? lobby_.IsHost() : true;
        v.server = IsServer();
        v.reach = MyReach(); v.cut = cfg_.voiceCut;
        v.mic = cfg_.voiceMic; v.levelDb = int(std::lround(cfg_.voiceLevelDb)); v.key = cfg_.voiceKey;
        v.volumePct = int(std::lround(cfg_.voiceVolume * 100.f)); v.device = cfg_.voiceDevice;
        v.devices = voice_.Devices(now);
        v.monitor = voice_.Monitor();
        v.micDb = voice_.MicLevelDb(now);
        v.gateOpen = voice_.GateOpen();
        v.sending = voice_.MeTalking();
        v.recording = voice_.Recording();
        v.source = voice_.SourceName();
        return v;
    }

    void SetVoiceReach(voice::Reach r) override {
        if (IsServer() || r == MyReach()) return;   // a role=server host only relays
        cfg_.voice = r != voice::Reach::Off;
        if (r != voice::Reach::Off) cfg_.voiceReach = r;
        HSM_LOG("voice: voice chat set to %s from the menu", voice::ReachIni(r));
        if (!SaveIniValues({{"voice", cfg_.voice ? "1" : "0"}, {"voice_reach", voice::ReachIni(cfg_.voiceReach)}}))
            HSM_LOG("voice: could not write voice/voice_reach to HSMMod.ini");
        if (r == voice::Reach::Off) for (uint8_t e = 1; e < voice::kMaxSpeakers; ++e) voice_.Forget(e);
    }

    void SetVoicePrefs(const MpVoice& v) override {
        cfg_.voiceMic = v.mic;
        cfg_.voiceCut = v.cut;
        cfg_.voiceLevelDb = float(std::clamp(v.levelDb, voice::kLevelMinDb, voice::kLevelMaxDb));
        if (v.key) cfg_.voiceKey = v.key;
        cfg_.voiceVolume = float(std::clamp(v.volumePct, 0, voice::kVolumeMaxPct)) / 100.f;
        cfg_.voiceDevice = v.device;
        voice_.Configure(VoiceSettingsFromConfig());
        voiceConfigured_ = true;
        if (!SaveIniValues({{"voice_mode", voice::MicIni(cfg_.voiceMic)},
                            {"voice_cut", cfg_.voiceCut ? "1" : "0"},
                            {"voice_device", cfg_.voiceDevice},
                            {"voice_level_db", std::to_string(int(cfg_.voiceLevelDb))},
                            {"voice_key", voice::VoiceKeyIni(cfg_.voiceKey)},
                            {"voice_volume", std::to_string(int(std::lround(cfg_.voiceVolume * 100.f)))}}))
            HSM_LOG("voice: could not write the voice settings to HSMMod.ini");
    }

    void SetVoiceMonitor(bool on) override { voice_.SetMonitor(on); }

    int PingOfSlot(uint8_t slot, uint32_t now) {
        if (IsHost()) { const std::optional<uint32_t> p = PeerOfSlot(slot); return p ? int(net_->RoundTripMs(*p)) : -1; }
        const PingSummary s = pings_.Summary(slot, now);
        if (s.medianMs != kPingUnknown) return s.medianMs;
        if (slot == kHostSlot && hostPeer_) return int(net_->RoundTripMs(hostPeer_));
        return -1;
    }
    std::string RouteOfSlot(uint8_t slot) const {
        if (IsHost()) return "direct";
        if (slot == kHostSlot) return "direct";
        if (const uint8_t via = DetourClientOn() ? detour_.RelayFor(slot) : 0) return "through " + NameOfSlot(via);
        return meshOn_ && meshPeers_.PeerOfSlot(slot) ? "direct" : "through the host";
    }
    void FillListen(MpPlayer& p) const {
        const auto it = listen_.find(p.name);
        if (it != listen_.end()) { p.muted = it->second.muted; p.volumePct = it->second.volumePct; }
    }

    std::vector<MpPlayer> Players() override {
        std::vector<MpPlayer> out;
        if (!InSession()) return out;
        const uint32_t now = NowMs();
        auto add = [&](uint8_t slot) {
            MpPlayer p;
            p.slot = slot;
            const uint8_t e = Roster::EntityForSlot(slot);
            p.me = e == MyEntity();
            p.host = slot == kHostSlot;
            p.name = p.me ? MyName() : NameOfSlot(slot);
            p.talking = p.me ? voice_.MeTalking() : voice_.Talking(e, now);
            if (!p.me) { p.pingMs = PingOfSlot(slot, now); p.route = RouteOfSlot(slot); FillListen(p); }
            p.canKick = IsHost() && !p.me && !p.host;
            p.canVote = !IsHost() && !p.me && !p.host;
            p.canVoteBan = p.canVote && !hostHasFighter_;   // a role=server session
            out.push_back(std::move(p));
        };
        if (IsHost()) {
            if (localFighter_) add(kHostSlot);
            std::vector<uint8_t> slots;
            for (const auto& [peer, q] : peers_) slots.push_back(q.slot);
            std::sort(slots.begin(), slots.end());
            for (uint8_t s : slots) add(s);
        } else {
            for (const FighterEntry& f : info_.fighters) {
                if (f.entity < 1 || f.entity > kMaxFighters || NoSuchFighter(f.entity)) continue;
                add(uint8_t(f.entity - 1));
            }
        }
        return out;
    }

    void SetPlayerListen(uint8_t slot, bool muted, int volumePct) override {
        const std::string name = NameOfSlot(slot);
        if (name.empty()) return;
        listen_[name] = voice::Listen{muted, std::clamp(volumePct, 0, voice::kVolumeMaxPct)};
        HSM_LOG("voice: slot%u %s, volume %d%% (this machine only)", unsigned(slot), muted ? "muted" : "unmuted", volumePct);
    }

    void KickPlayer(uint8_t slot, bool ban) override {
        if (!net_ || !IsHost() || slot == kHostSlot) return;
        ModerationLine(std::string(ban ? "/ban #" : "/kick #") + std::to_string(slot), 0, false);
    }
    void MenuVoteLine(const std::string& line) {
        if (!InSession()) return;
        if (IsHost()) ModerationLine(line, 0, false); else SendChat(line);
    }
    void VoteKickPlayer(uint8_t slot) override { if (slot != kHostSlot) MenuVoteLine("/votekick #" + std::to_string(slot)); }
    void VoteBanPlayer(uint8_t slot) override { if (slot != kHostSlot) MenuVoteLine("/voteban #" + std::to_string(slot)); }
    void CastVote(bool yes) override { MenuVoteLine(yes ? "/yes" : "/no"); }

    MpVote Vote() override {
        MpVote v;
        if (!InSession()) return v;
        VoteStateMsg m = IsHost() ? VoteStateFor(kHostVoterKey, NowMs()) : voteState_;
        if (m.target == kVoteNone) return v;
        v.active = true; v.target = m.target; v.yes = m.yes; v.no = m.no; v.needed = m.needed; v.voters = m.voters;
        v.secondsLeft = m.secondsLeft; v.mine = m.mine; v.ban = m.ban != 0;
        v.name = IsHost() ? voteTargetName_ : NameOfSlot(m.target);
        if (IsHost() && (!localFighter_ || IsServer())) v.mine = kVoteNotVoter;
        return v;
    }

    std::vector<std::string> NetworkLines() override {
        std::vector<std::string> out;
        if (!InSession()) { out.push_back("Not in a session."); return out; }
        const uint32_t now = NowMs();
        out.push_back(std::string("Role: ") + (IsServer() ? "server host" : IsHost() ? "host" : "client") + " (Steam)");
        out.push_back("Players: " + std::to_string(IsHost() ? ConnectedSlots().size() : info_.fighters.size()));
        if (!IsHost()) {
            const int ms = PingOfSlot(kHostSlot, now);
            out.push_back("Ping to the host: " + (ms >= 0 ? std::to_string(ms) + " ms" : std::string("not measured")));
        }
        out.push_back(std::string("Direct client links (mesh): ") + (IsHost() ? (MeshHost() ? "offered" : "off") : (meshOn_ ? "on" : "off")));
        if (IsHost()) {
            out.push_back(std::string("Detours: ") + (DetourHost() ? "on, plan " + std::to_string(detourEpoch_) : "off (detour=1 with mesh=1)"));
        } else if (DetourClientOn()) {
            int via = 0;
            for (uint8_t s = 1; s < kMaxFighters; ++s) if (detour_.RelayFor(s)) ++via;
            out.push_back("Detours: plan " + std::to_string(detour_.Epoch()) + ", " + std::to_string(via) + " through a relay, relaying " +
                          std::to_string(detour_.RelayPairs()));
        } else {
            out.push_back("Detours: off");
        }
        out.push_back("Voice chat: " + ReachLabel(MyReach()));
        return out;
    }

    void LeaveSession() override {
        if (!net_ && !(steamMode() && lobby_.InLobby())) return;
        HSM_LOG("session: leaving from the menu (%s)", net_ && IsHost() ? "host" : "client");
        const bool fight = InFightLevel();
        welcomed_ = false;                             // the lobby's onLeft must not open the menu a second time
        if (steamMode()) lobby_.Leave();
        if (net_) ResetNetwork("left from the menu");
        if (fight) game::OpenLevel(lastGameMode_, kMenuLevel, true, "");
    }

    bool HelpImprove() const override { return cfg_.helpImprove; }
    bool HelpImproveAsked() const override { return cfg_.helpImproveAsked; }
    void SetHelpImprove(bool on) override {
        cfg_.helpImprove = on;
        cfg_.helpImproveAsked = true;
        HSM_LOG("report: help improve the mod: %s", on ? "on" : "off");
        if (!SaveHelpImprove(on)) HSM_LOG("report: could not write help_improve=%d to HSMMod.ini", on ? 1 : 0);
    }

    // The relay's answers, on the game thread. Each request ends in exactly one outcome line (ReportOutcomeLine), shown
    // where the player can see it. A filed report is shared with the session so the others may add logs.
    void ReportTick() {
        CrashReportAtStart();
        const uint32_t now = NowMs();
        while (std::optional<report::Result> r = report::Poll()) {
            const ReportVerdict& v = r->verdict;
            const bool filing = r->job != ReportJob::Log;
            const bool manual = filing && reportIsManual_;
            const bool sameSession = reportSession_ == sessionGen_;   // a late answer is not shared into another session (R2)
            if (filing) reportIsManual_ = false;
            // The cooldown counts reports the relay may have filed: a lost answer is no reason to send it again (R10).
            if (manual && v.outcome != ReportOutcome::NotSent && v.outcome != ReportOutcome::Refused) { reportedOnce_ = true; lastReportAt_ = now; }
            bool shared = false;
            if (filing && v.outcome == ReportOutcome::Done) {
                if (r->job == ReportJob::Report && !sameSession) HSM_LOG("report: %s was filed in an earlier session; not shared with this one", v.id.c_str());
                else if (ShareReport(v.id, v.token)) shared = true;
                else if (r->job == ReportJob::Crash) pendingShare_.Hold(v.id, v.token, now);   // filed in the menu: the next session gets it
            }
            ReportOutcomeToPlayer(ReportOutcomeLine(r->job, v, shared));
        }
        ReportOutcomeFlush();
        if (!report::Busy() && SessionUp()) {              // a crash report filed before this session: shared once
            std::string id, token;
            if (pendingShare_.Take(now, id, token) && ShareReport(id, token))
                HSM_LOG("report: shared the crash report %s with this session", id.c_str());
        }
        if (autoAttachDue_ && !report::Busy()) {           // help_improve: an offer that came while a send was busy
            autoAttachDue_ = false;
            if (reportOffer_.Open(now)) AutoAttach();
        }
    }

    // The outcome line goes to the chat history once the player can see it: the match chat is up, or the multiplayer page
    // is open (its lobby chat box, or the note under REPORT A BUG outside a lobby). Until then it waits, so an answer
    // that comes back during a level change or with the pause menu up is not missed. The log says what the player saw.
    void ReportOutcomeToPlayer(const std::string& line) {
        if (line.empty()) return;
        reportHeld_.push_back(line);
        while (reportHeld_.size() > 4) reportHeld_.pop_front();
        if (!ReportOutcomeVisible()) HSM_LOG("report: outcome held until the chat or the multiplayer page is on screen: %s", line.c_str());
    }

    static bool ReportOutcomeVisible() { return ui::ChatHudShown() || ui::PanelOpen(); }

    void ReportOutcomeFlush() {
        if (reportHeld_.empty() || !ReportOutcomeVisible()) return;
        for (const std::string& line : reportHeld_) {
            HSM_LOG("report: told the player: %s", line.c_str());
            ui::ShowLine("HSMMod", style::kBone, line);
            reportStatus_ = line; ++reportStatusSeq_;
        }
        reportHeld_.clear();
    }

    bool SessionUp() const { return net_ && (IsHost() ? !peers_.empty() : bool(hostPeer_)); }

    // A filed report's id and token to everyone else in the session (the host relays a client's). false: nobody to tell.
    bool ShareReport(const std::string& id, const std::string& token) {
        if (!SessionUp()) return false;
        ReportShareMsg m; m.id = id; m.token = token;
        if (IsHost()) net_->Broadcast(kChanReliable, Encode(m), true);
        else net_->Send(hostPeer_, kChanReliable, Encode(m), true);
        return true;
    }

    // help_improve: the last session's log ends in a crash (crash_log.cpp): report it once, at the first tick of this one.
    // The report's text is the crash line only; both logs go to the relay redacted as for /report.
    void CrashReportAtStart() {
        if (crashChecked_) return;
        crashChecked_ = true;
        std::string tail;
        if (FILE* f = _wfopen(InstanceFilePath(L"HSMMod", L".prev.log").c_str(), L"rb")) {
            if (std::fseek(f, 0, SEEK_END) == 0) {
                const long size = std::ftell(f);
                const long from = size > long(kCrashTailBytes) ? size - long(kCrashTailBytes) : 0;
                if (size > 0 && std::fseek(f, from, SEEK_SET) == 0) {
                    tail.resize(size_t(size - from));
                    tail.resize(std::fread(tail.data(), 1, tail.size(), f));
                }
            }
            std::fclose(f);
        }
        const std::string line = CrashLineOf(tail);
        if (line.empty()) return;
        if (!cfg_.helpImprove) {
            HSM_LOG("report: the last session crashed (%s); help_improve is off, nothing sent", line.c_str());
            // Said once, where the player can see it (held like a report outcome until the chat or the page is up).
            if (report::Enabled())
                ReportOutcomeToPlayer("The game crashed last session. Turn on HELP IMPROVE THE MOD on the multiplayer page "
                                      "(or the Esc menu in a match) to send crash reports automatically.");
            return;
        }
        if (!report::Enabled() || report::Busy()) { HSM_LOG("report: the last session crashed; reports are off or busy, not sent"); return; }
        if (!report::Submit(CrashReportText(line), ReportFactsNow(), ReportJob::Crash)) { HSM_LOG("report: the crash report could not be started"); return; }
        HSM_LOG("report: the last session crashed; sending a crash report (help_improve is on): %s", line.c_str());
    }

    // help_improve: this player's log joins the open offer without /sendlog.
    void AutoAttach() {
        std::string id, token;
        if (!reportOffer_.Take(NowMs(), id, token)) return;
        if (!report::Attach(id, token, ReportFactsNow())) { HSM_LOG("report: could not add the log to report %s", id.c_str()); return; }
        HSM_LOG("report: help_improve is on; sending this log to report %s", id.c_str());
        ui::ShowLine("HSMMod", style::kBone, "Help improve the mod is on: sending your HSMMod log too (Steam IDs, IPs and your "
                                             "Windows user name removed; player names stay).");
    }

    // A newer HSMMod release (net/update_check.h), said once in the chat a few seconds into the first fight level, when the HUD is up.
    void UpdateNoticeTick() {
        if (!FighterLevel() || NowMs() - levelLoadedAt_ < 4000) return;
        const std::string notice = update::TakeChatNotice();
        if (!notice.empty()) ui::ShowLine("HSMMod", style::kBone, notice);
    }

    // Another player's report: this player may add their own log with /sendlog (nothing is sent otherwise).
    void OfferLog(const ReportShareMsg& m) {
        if (!report::Enabled()) return;
        reportOffer_.Offer(m.id, m.token, NowMs());
        if (cfg_.helpImprove) {                            // opted in: no question, the log goes along
            HSM_LOG("report: %s filed report %s; help_improve is on", NameOfSlot(m.fromSlot).c_str(), m.id.c_str());
            ui::ShowLine("HSMMod", style::kBone, NameOfSlot(m.fromSlot) + " filed a bug report.");
            if (report::Busy()) autoAttachDue_ = true;
            else AutoAttach();
            return;
        }
        HSM_LOG("report: %s filed report %s; offered /sendlog", NameOfSlot(m.fromSlot).c_str(), m.id.c_str());
        ui::ShowLine("HSMMod", style::kBone, NameOfSlot(m.fromSlot) + " filed a bug report. Type /sendlog to add your log, or /nolog.");
    }

    ReportFacts ReportFactsNow() const {
        static constexpr const char* kPhases[] = {"idle", "countdown", "live", "sudden death", "results"};
        ReportFacts f;
        f.modBuild = game::ModBuild();
        char key[32]; std::snprintf(key, sizeof key, "%016llx", (unsigned long long)BuildKey()); f.gameBuild = key;
        f.protocol = kProtocolRevision;
        f.role = !net_ ? "solo" : IsHost() ? "host" : "client";
        f.transport = !net_ ? "none" : "steam";
        f.syncMode = HostPhysics() ? "host" : "owner";
        f.level = currentLevel_ + (progressionHub_ ? " (progression hub)" : "");
        f.matchFormat = ModeName(info_.format, info_.gear);
        f.matchPhase = info_.phase < std::size(kPhases) ? kPhases[info_.phase] : "?";
        f.players = !net_ ? 1 : IsHost() ? int(ConnectedSlots().size()) : int(info_.fighters.size());   // a client knows the fighters
        f.os = report::OsName();
        f.ue4ss = ue4ss::ReportVersion();
        f.ue4ssMods = ue4ss::ReportMods();
        return f;
    }

    // A stand-in (the mirror bench's or a session's) whose own Willie ReceiveTick must not run: it re-arms the muscles,
    // pain and damage the stand-in's neutralisation switched off.
    bool IsTickBlocked(const UObject* o) const { return o && (o == mirror_.TickBlocked() || standIns_.IsStandIn(o)); }
    bool BlockStandInTimeline(const UObject* o) {
        if (!IsTickBlocked(o)) return false;
        ++timelinesBlocked_;
        return true;
    }
    // Slow motion (R) and photo mode change world time, and photo mode pauses the game, on one machine only (audit items
    // 4, 20, 32: Willie_BP SloMo input -> Slomo Timeline -> SetGlobalTimeDilation; BPC_PhotoMode SetGamePaused and
    // SetGlobalTimeDilation). In a session match neither key does anything.
    bool CuttingRingOut(UFunction* fn, void* params) {
        if (!stockFlowGuard_ || !params) return false;
        const int32_t off = OtherActorOffset(fn);
        UObject* other = off >= 0 ? *reinterpret_cast<UObject**>(static_cast<uint8_t*>(params) + off) : nullptr;
        bool player = false;
        if (other && adapter_.IsWillie(other) && GetBool(other, "Player", player) && player) RingOut(other, "the Cutting's edge");
        return false;   // the level's own UI_Lose is suppressed with the other owner-match screens
    }
    bool BlockTimeInput(const char* what) {
        if (!stockFlowGuard_) return false;
        if (++timeInputBlocked_ % 20 == 1) HSM_LOG("session: blocked %s during a session match (%u so far)", what, timeInputBlocked_);
        return true;
    }

    std::string NameOfSlot(uint8_t slot) const {
        uint8_t e = Roster::EntityForSlot(slot);
        for (const FighterEntry& f : info_.fighters) if (f.entity == e && !f.name.empty()) return f.name;
        if (const RosterMember* m = roster_.BySlot(slot)) return m->name;
        return "Player " + std::to_string(slot + 1);
    }

private:
    bool IsHost() const { return lobby_.IsHost(); }
    // role=server: no fighter of its own. Slot 0 / entity 1 is never used; every host path that looks at the host fighter
    // either iterates roster_ (which does not hold it) or checks localFighter_.
    bool IsServer() const { return cfg_.role == Role::Server; }
    uint8_t HostEntity() const { return localFighter_ ? uint8_t(1) : uint8_t(0); }
    // Client: the host's Welcome said it plays no fighter (kWelcomeNoHostFighter), so its entity is never one to spectate,
    // list or wait for, whatever a roster says.
    bool NoSuchFighter(uint8_t e) const { return !hostHasFighter_ && e == Roster::EntityForSlot(kHostSlot); }
    int SnapshotHz() const { return std::max(cfg_.snapshotHz, 60); }   // older ini files say 30
    bool InFightLevel() const { return !currentLevel_.empty() && !IsMenuLevel(currentLevel_); }
    // Matches run in fight levels, not in the Tavern hub where players walk around between fights.
    bool MatchLevel() const { return InFightLevel() && currentLevel_.find("Hub") == std::string::npos; }
    // A level with fighters in it. Not the Progression hub: outside Free Mode the Tavern is the game's menu-like hub
    // (character creation, the next fight's pick), with no pawn and a mouse cursor. Test 2026-10-25: the mod spawned a
    // fighter there after 15 s, took the cursor away (game-only input) and set the travel's game mode back over the
    // hub's own, so both players sat in the character screen unable to click while their spawned fighters died.
    bool FighterLevel() const { return InFightLevel() && !progressionHub_; }
    bool InAbyss() const { return FighterLevel() && currentLevel_.find("Abyss") != std::string::npos; }

    // Sync audit 36, 37, every machine in a session on the Abyss: the level's own enemy spawner held shut (the host spawns
    // every zombie) and floor under every fighter that is not this machine's own (session/hsm_abyss.h).
    void AbyssTick() {
        if (!InAbyss()) return;
        std::vector<UObject*> others = StandInActors();
        if (IsHost()) for (auto& [e, ref] : bots_) if (adapter_.StillValid(ref.obj, ref.index)) others.push_back(ref.obj);
        abyss_.Tick(adapter_, game::SteadyNowMs(), adapter_.PlayerWillie(0), others);
    }
    // On a match level no blow between session fighters counts before the match is live or after it ended (from
    // CSLewin's PR #1): a fighter hit while the others were still loading or in the countdown died before it began.
    bool CombatEnabled() const {
        return !MatchLevel() || CombatPhase(IsHost() ? match_.phase() : MatchPhase(info_.phase));
    }
    std::string MyName() const { return steam::PersonaName(steam::MySteamId()); }

    // ------------------------------------------------------------------ agent bridge (sensors/actuators)
    static constexpr const char* kAxisNames[kAxisCount] = {"MoveForward", "MoveRight", "TurnMouse", "LookMouse", "TurnGamepad",
                                                         "LookGamepad", "RightArm", "LeftArm", "RightGuard", "LeftGuard"};
    static constexpr const char* kActionNames[kActionCount] = {"GrabLeft", "GrabRight", "GuardLeft", "GuardRight", "Thrust", "ThrustGamepad",
                                                             "Jump", "CrouchKey", "CrouchHold", "Run", "SwapHands", "ToggleCameraLock",
                                                             "KeyForward", "KeyBack", "KeyLeft", "KeyRight", "Talk", "RightMouse", "MiddleMouse"};
    struct Held { float value = 0; uint32_t until = 0; };

    static std::string Vec3(const FVector& v) { char b[96]; std::snprintf(b, sizeof b, "[%.0f,%.0f,%.0f]", v.x, v.y, v.z); return b; }

    std::string FighterJson(uint8_t entity, UObject* w) {
        if (!w) return "{\"entity\":" + std::to_string(entity) + ",\"actor\":false}";
        double* hp = Field<double>(w, "Health");
        return "{\"entity\":" + std::to_string(entity) + ",\"pos\":" + Vec3(adapter_.ActorLocation(w)) + ",\"dead\":" +
               (adapter_.IsDead(w) ? "true" : "false") + ",\"health\":" + std::to_string(hp ? int(*hp) : -1) + "}";
    }

    std::string StateJson() {
        std::string j = "{\"ok\":true,\"role\":" + JsonString(net_ ? (IsHost() ? "host" : "client") : "solo") + ",\"level\":" + JsonString(currentLevel_) +
                        ",\"fight\":" + (InFightLevel() ? "true" : "false") + ",\"match_level\":" + (MatchLevel() ? "true" : "false") +
                        ",\"paused\":" + (adapter_.GamePaused() ? "true" : "false") + ",\"mode_ticking\":" + (NowMs() - lastModeTick_ < 1000 ? "true" : "false");
        j += ",\"match\":{\"phase\":" + std::to_string(info_.phase) + ",\"format\":" + std::to_string(info_.format) + ",\"status\":" +
             JsonString(StatusLine(info_, IsHost() ? HostEntity() : world_.myEntity())) + "}";
        j += ",\"local\":" + FighterJson(IsHost() ? HostEntity() : world_.myEntity(), InFightLevel() ? adapter_.PlayerWillie(0) : nullptr);
        j += ",\"fighters\":[";
        bool first = true;
        auto add = [&](uint8_t e, UObject* w) { j += (first ? "" : ",") + FighterJson(e, w); first = false; };
        if (IsHost()) {
            for (const RosterMember& m : roster_.members()) add(m.entity, EntityActor(m.entity));
            for (auto& [e, ref] : bots_) add(e, EntityActor(e));
        } else {
            for (auto& [e, ref] : puppets_) add(e, adapter_.StillValid(ref.obj, ref.index) ? ref.obj : nullptr);
        }
        j += "],\"peers\":" + std::to_string(peers_.size()) + ",\"welcomed\":" + (welcomed_ ? "true" : "false") +
             ",\"roster\":" + std::to_string(roster_.size()) + ",\"willies_in_level\":" + std::to_string(InFightLevel() ? adapter_.AllWillies().size() : 0) + "}";
        return j;
    }

    std::string HandleAgent(const AgentCommand& c) {
        auto ok = [](const std::string& extra = "") { return "{\"ok\":true" + extra + "}"; };
        auto err = [](const std::string& why) { return "{\"ok\":false,\"error\":" + JsonString(why) + "}"; };
        if (c.verb == "ping") return ok();
        if (c.verb == "state") return StateJson();
        if (c.verb == "open" && c.args.size() == 1) {
            if (!IsHost() && net_) return err("clients follow the host");
            adapter_.PrepareFreeModeFight();
            HostTravel(c.args[0]);
            return ok();
        }
        if (c.verb == "axis" && c.args.size() == 3) {
            for (size_t i = 0; i < kAxisCount; ++i) if (c.args[0] == kAxisNames[i]) {
                float v = ParseArgFloat(c.args[1], -kAxisLimit, kAxisLimit), ms = ParseArgFloat(c.args[2], 0, 60000);
                if (std::isnan(v) || std::isnan(ms)) return err("bad value or duration");
                held_[i] = {v, NowMs() + uint32_t(ms)};
                return ok();
            }
            return err("unknown axis");
        }
        if (c.verb == "action" && c.args.size() == 2) {
            UObject* me = InFightLevel() ? adapter_.PlayerWillie(0) : nullptr;
            if (!me || !adapter_.BindInputs(me)) return err("no local fighter");
            for (size_t i = 0; i < kActionCount; ++i) if (c.args[0] == kActionNames[i]) {
                bool tap = c.args[1] == "tap";
                if (c.args[1] == "down" || tap) adapter_.FireAction(me, Action(i), true);
                if (c.args[1] == "up" || tap) adapter_.FireAction(me, Action(i), false);
                return ok();
            }
            return err("unknown action");
        }
        if (c.verb == "quit") { quitRequested_ = true; return ok(); }
        if (c.verb == "widgets") return WidgetsJson();
        if (c.verb == "pause" && c.args.size() == 1) { adapter_.SetGamePaused(c.args[0] == "on"); return ok(); }
        if (c.verb == "prof") return ue::prof::Command(c.args);
        if (c.verb == "vmhook") return agent::VmHookCommand(c.args, [this] { return InFightLevel() ? adapter_.PlayerWillie(0) : nullptr; });
        if (c.verb == "mirror") return mirror_.Command(c.args, adapter_, net_ != nullptr, InFightLevel());
        if (c.verb == "rec" && !c.args.empty() && c.args[0] == "mark") {   // rec mark <text>: a Marker event in the recording
            std::string text;
            for (size_t i = 1; i < c.args.size(); ++i) text += (text.empty() ? "" : " ") + c.args[i];
            recorder_.Marker(text);
            return recorder_.Recording() ? ok() : err("not recording (record=1 in [mod], in a fight level)");
        }
        if (c.verb == "shot") { adapter_.ConsoleCommand("HighResShot 1"); return ok(",\"note\":\"saved under Saved/Screenshots\""); }
        if (c.verb == "exec" && !c.args.empty()) {
            std::string cmd;
            for (const std::string& a : c.args) cmd += (cmd.empty() ? "" : " ") + a;
            adapter_.ConsoleCommand(cmd);
            return ok();
        }
        return err("unknown command (ping, state, open <level>, axis <name> <value> <ms>, action <name> down|up|tap, shot, exec <console command>, vmhook stats|hits|log|block|off, mirror on <delayMs> <jitterMs> <lossPct> [offsetUu] [keeptick]|stats|off, quit)");
    }

    // Held axes are re-applied every frame like a real device, through Willie's own input events.
    void ApplyHeldInput() {
        UObject* me = InFightLevel() ? adapter_.PlayerWillie(0) : nullptr;
        if (!me || !adapter_.BindInputs(me)) return;
        uint32_t now = NowMs();
        for (size_t i = 0; i < kAxisCount; ++i)
            if (held_[i].until) {
                bool live = int32_t(held_[i].until - now) > 0;
                adapter_.ApplyAxis(me, Axis(i), live ? held_[i].value : 0.f);
                if (!live) held_[i] = Held{};
            }
    }

    // In a session (or an automated -hsmauto/-hsmtest run) the mod does not wait forever for the stock level flow to
    // produce a player: a fight level that still has no local pawn after the grace period gets one from the mod (own
    // loadout, possessed by player 0). Clients take the spot the host announced; host/solo use the level's spawn
    // points. Plain single-player play is never touched (no pawn is normal there: death screens, transitions).
    // An owner-sync client waits for its level gate instead (the gate's exit spawns the announced fighter) and is
    // only the short fallback after it (LocalFighterGrace).
    void EnsureLocalFighter() {
        if (!localFighter_ || !(net_ || automated_) || !FighterLevel() || adapter_.PlayerWillie(0)) { noPawnSince_ = 0; return; }
        double graceMs = 0;
        if (!LocalFighterGrace(net_ && !IsHost(), levelGate_.Open(), graceMs)) { noPawnSince_ = 0; return; }
        uint32_t now = NowMs();
        if (!noPawnSince_) { noPawnSince_ = now; return; }
        if (double(now - noPawnSince_) < graceMs) return;
        noPawnSince_ = now;                                   // retry spacing if the spawn fails
        game::Adapter::SpawnOptions o; o.teamInt = 1;
        std::vector<uint8_t> own;
        bool client = net_ && !IsHost();
        if (client) {
            const Spawn* sp = world_.Find(world_.myEntity());
            if (!sp) return;                                  // wait for the host's announcement
            o.at = {sp->x, sp->y, sp->z}; o.yawDeg = sp->yaw;
            o.blob = sp->blobId ? clientBlobs_.AtLeast(sp->blobId) : nullptr;
            if (!o.blob && !lastUpload_.empty()) o.blob = &lastUpload_;
        } else {
            std::vector<FVector> pts = adapter_.SpawnCandidates();
            o.at = pts.empty() ? FVector{} : pts.front();
            if (adapter_.ExportPassport(own)) o.blob = &own;
        }
        HSM_LOG("session: no player pawn in %s after %.0f ms; spawning the local fighter", currentLevel_.c_str(), graceMs);
        RespawnLocalNow(o);
    }

    // -hsmauto=<level>: a host/solo instance loads that Free Mode level by itself; -hsmquit=<s>: quit after s seconds.
    // -hsmmirror=<delayMs>,<jitterMs>,<lossPct>[,<offsetUu>]: offline, "mirror on" with those numbers once per fight
    // level, 3 s after the local fighter is there (a failed attempt is retried every 10 s).
    void AutoRun() {
        uint32_t now = NowMs();
        if (!startedAt_) {
            startedAt_ = now;
            std::string cl = CommandLineUtf8();
            autoLevel_ = CmdArg(cl, "hsmauto").value_or("");
            automated_ = !autoLevel_.empty() || CmdArg(cl, "hsmtest").has_value();
            quitAfterMs_ = uint32_t(CmdArgInt(cl, "hsmquit", 0, 10, 86400)) * 1000u;
            std::string m = CmdArg(cl, "hsmmirror").value_or("");
            if (!m.empty()) {
                mirrorArgs_ = {"on"};
                for (size_t at = 0; at <= m.size();) {
                    size_t comma = std::min(m.find(',', at), m.size());
                    mirrorArgs_.push_back(m.substr(at, comma - at));
                    at = comma + 1;
                }
                HSM_LOG("session: -hsmmirror: the mirror comes on in every offline fight level (%s)", m.c_str());
            }
        }
        if (quitRequested_ || (quitAfterMs_ && now - startedAt_ >= quitAfterMs_)) {
            HSM_LOG("session: quitting (%s)", quitRequested_ ? "agent request" : "-hsmquit timer");
            quitRequested_ = false; quitAfterMs_ = 0;
            adapter_.QuitGame();
            return;
        }
        bool mayTravel = !net_ || IsHost();
        if (!autoLevel_.empty() && mayTravel && lastGameMode_ && !InFightLevel() && now - startedAt_ > 8000 && now >= nextAuto_) {
            nextAuto_ = now + 15000;
            HSM_LOG("session: -hsmauto opening %s", autoLevel_.c_str());
            adapter_.PrepareFreeModeFight();
            HostTravel(autoLevel_);
        }
        if (mirrorArgs_.empty() || mirrorDone_ || net_ || !InFightLevel() || mirror_.Active() || !adapter_.PlayerWillie(0)) {
            mirrorSince_ = 0;
            return;
        }
        if (!mirrorSince_) mirrorSince_ = now;
        if (now - mirrorSince_ < 3000 || now < nextMirror_) return;
        nextMirror_ = now + 10000;
        std::string r = mirror_.Command(mirrorArgs_, adapter_, false, true);
        mirrorDone_ = mirror_.Active();
        HSM_LOG("session: -hsmmirror: %s", r.c_str());
    }

    // ------------------------------------------------------------------ lobby / transport lifecycle
    void OnLobbyEntered() {
        ui::ChatClearLines();   // the last lobby's chat is not this one's
        lobbyGate_.Clear();
        auto st = std::make_unique<SteamTransport>(lobby_.IsHost() ? ValidateForHost : ValidateForClient);
        steamNet_ = st.get();
        net_ = std::move(st);
        ++sessionGen_;
        if (lobby_.IsHost()) {
            for (const game::LobbyMember& m : lobby_.members()) if (m.id != steam::MySteamId()) steamNet_->AllowPeer(m.id);
            hostPending_ = true;
        } else { hostSteam_ = lobby_.owner(); steamNet_->AllowPeer(hostSteam_); }
    }

    void ResetNetwork(const char* why) {
        if (!migration_.Active()) { migrateBoard_ = ScoreCarry{}; migrateObjects_.clear(); }
        if (net_) { HSM_LOG("session: network reset (%s)", why); net_->Stop(); }
        steamNet_ = nullptr; net_.reset();
        ++sessionGen_; reportOffer_ = ReportOffer{}; autoAttachDue_ = false;   // R2: no offer or late report outlives its session
        game::shareddice::Clear();                   // the game's own randomness again
        claims_.Stop();                              // the hooks go before any fighter does
        fx_.Stop();                                  // with its paint boxes on the stand-ins
        equip_.Stop();
        ClearScriptHooks(kStockFlowOwner);
        grabs_.Stop();                               // unmarks the fighters it held, so before they go too
        props_.Stop();
        severs_.Stop();
        gore_.Stop();
        // host
        for (uint8_t slot = 1; slot < kMaxFighters; ++slot) DespawnFighter(slot);
        roster_.Clear(); director_ = HostDirector{}; hostBlobs_.Clear(); peers_.clear(); helloWatch_.Clear(); blobVersion_.fill(0); hostExport_.clear();
        for (auto& b : ownLoadout_) b.clear();
        ownBlobId_.fill(0);
        picksValid_ = false; hostPending_ = false; travelSeq_ = 0;
        for (auto& [e, ref] : bots_) if (adapter_.StillValid(ref.obj, ref.index)) adapter_.DestroyActor(ref.obj);
        bots_.clear(); match_ = Match{}; kills_ = KillLog{}; info_ = MatchInfo{}; groundSeen_ = false; clearCount_ = 0; nextGroundClear_ = 0; feed_.clear(); banner_.Reset(); tints_.clear();
        ownDeadPending_ = false; ownRespawn_.Reset(); dropClearPending_ = false; mapGate_.Reset();
        mirrorBlob_.clear(); ownBlob_ = 0;
        // client
        for (auto& [e, ref] : puppets_) {
            adapter_.DestroyFighter(ref.obj, ref.index);   // a stand-in: its weapons go too
        }
        puppets_.clear(); world_ = ClientWorld{}; clientBlobs_.Clear(); clientAssembler_ = BlobAssembler{}; lastUpload_.clear();
        modMapHost_.reset(); modMapChecked_ = false; modMapApproved_.clear(); pendingTravel_.reset();
        hostPeer_ = 0; welcomed_ = false; snappedOwn_ = false; hellosSent_ = 0;
        hostSteam_ = 0; meshOn_ = false; meshPeers_.Clear(); meshHearing_.Clear(); meshMasks_.Clear(); peerMapDirty_ = false;
        peerPingOn_ = false; meshDedupe_.Clear(); pings_.Clear(); pongs_.Clear(); pingTables_.clear(); nextPingTable_ = 0;
        routeLinks_.Clear(); routePlanner_.Clear(); nextRoutePlan_ = 0;
        for (MsgBudget& b : meshBudget_) b = MsgBudget{};
        meshRx_ = meshDropped_ = meshRelaySkipped_ = 0;
        voice_.Stop(); voiceState_ = {}; voiceSeq_ = 0; voiceDrops_ = VoiceDrops{}; voiceRelayed_ = 0; voiceCutLogs_ = 0;
        voteState_ = VoteStateMsg{}; voteWasActive_ = voteDirty_ = false; voteSentAt_ = 0;
        detour_.Clear(); pairKeys_.Clear(); detourEpoch_ = 0; detourDirty_ = false; detourStats_ = DetourStats{}; detourLoggedEpoch_ = -1;
        meshDirectTx_ = meshDupes_ = 0;
        reservations_.Clear();
        RestoreFoes();
        SetMyWillie(nullptr);                    // the permanent input hooks must stop swallowing this pawn's look
        held_.fill(Held{});
        hostLoss_ = HostLossMonitor(NowMs());
        // owner sync
        standIns_.Clear(); grabs_.Clear(); chests_.Clear(); abyss_.Clear(adapter_); streams_.Clear(); lives_.clear(); respawnQueue_.clear();
        // host physics: back to owner sync until a host says otherwise (Welcome, MatchInfo)
        syncMode_ = nextSync_ = clientNextSync_ = SyncMode::Owner; standIns_.SetReplica(false);
        capture_ = InputCapture{}; sendAccum_ = 0; inputsTx_ = 0;
        ResetLeases(); ownLeased_ = false; nativeButtons_ = 0; ResetHostDelay();
        ownDeathReported_ = false; nextKeepAlive_ = 0; streamsDropped_ = claimsRejected_ = 0;
        lifeBook_.Clear(); clientLife_.fill(0); staleLife_ = 0;
        vote_.Clear(); pendingDrops_.clear(); rejected_.reset();
    }

    // ------------------------------------------------------------------ travel / levels
    // Our own OpenLevel bypasses the travel hook, so a level the host opens itself (Start match, -hsmauto, the bridge,
    // Progression back to the hub) must be announced to the lobby here, after the GI settings for it are set.
    void HostTravel(const std::string& level) {
        OnOpenLevel(level, true, "");
        game::OpenLevel(lastGameMode_, level, true, "");
    }

    // Modded maps: what clients are told about the level the host opens (empty path for a stock map). Nothing is sent but
    // the map path and the pak's name, size and SHA-256.
    static ModMapMsg HostModMapFor(const std::string& level) {
        const HostModMapResult r = game::modmaps::HostMessage(level);
        if (r.state == HostModMap::Stock) return ModMapMsg{};
        if (r.state == HostModMap::HashPending) HSM_LOG("maps: the pak of %s is still being hashed; clients get the map path only", level.c_str());
        return r.msg;
    }

    // Client: the host's modded-map message, and the check of it against the paks on this machine.
    void ModMapTick() {
        if (!modMapHost_ || modMapChecked_) return;
        const ModMapVerdict v = game::modmaps::Decide(*modMapHost_);
        if (v == ModMapVerdict::Pending) return;
        modMapChecked_ = true;
        if (v == ModMapVerdict::NoMap) { modMapApproved_.clear(); pendingTravel_.reset(); return; }
        if (v == ModMapVerdict::Ok) {
            modMapApproved_ = modMapHost_->path;
            HSM_LOG("maps: host's modded map %s is available here", modMapHost_->path.c_str());
            if (pendingTravel_) { Travel tr = *pendingTravel_; pendingTravel_.reset(); FollowTravel(tr); }
            return;
        }
        const std::string line = ModMapJoinLine(*modMapHost_, v);
        HSM_LOG("maps: cannot join, %s", line.c_str());
        pendingTravel_.reset(); modMapApproved_.clear(); modMapHost_.reset();
        const bool fight = InFightLevel();
        welcomed_ = false;                             // the lobby's onLeft must not open the menu a second time
        if (steamMode()) lobby_.Leave();
        ResetNetwork("modded map missing");
        ui::ShowLine("HSMMod", style::kBone, line);
        notice_ = line;
        if (fight) game::OpenLevel(lastGameMode_, kMenuLevel, true, "");
    }

    void FollowTravel(const Travel& tr) {
        if (tr.seq == travelSeq_ && !currentLevel_.empty() && tr.level.find(currentLevel_) != std::string::npos) {
            // Already here (the host re-sends its travel after our Hello): confirm instead of reloading.
            if (hostPeer_) { Ready r; r.travelSeq = travelSeq_; r.level = pendingLevel_; net_->Send(hostPeer_, kChanReliable, Encode(r), true); }
            return;
        }
        RememberFoes();
        size_t applied = game::ApplyGameSettings(tr.settings);
        fightSettings_ = tr.settings;
        travelSeq_ = tr.seq; pendingLevel_ = tr.level;
        HSM_LOG("client: following host to %s (%zu settings applied)", tr.level.c_str(), applied);
        game::OpenLevel(lastGameMode_, tr.level, tr.absolute, tr.options);
    }

    bool OnOpenLevel(const std::string& level, bool absolute, const std::string& options) {
        pauseLiftArmed_ = false;                       // the old level's world is going away
        ShowProfilerMessage(ue::prof::StopCapture());   // before the old level's functions are freed
        bool joined = steamMode() ? lobby_.InLobby() : net_ != nullptr;
        if (joined && IsHost() && !IsMenuLevel(level) && adapter_.EndErrandMode())
            HSM_LOG("session: %s opened as an errand (Game Mode 2); a session plays it as a normal fight", level.c_str());
        if (!joined || IsHost()) {                     // remembered even before hosting, for late joiners
            Travel t; t.level = level; t.absolute = absolute; t.options = options; t.settings = game::CaptureGameSettings();
            lastTravel_ = t;
        }
        fightSettings_.clear();
        if (!joined) return true;
        if (IsHost()) {
            Travel& t = *lastTravel_; t.seq = ++travelSeq_;
            fightSettings_ = t.settings;
            const uint64_t seed = NewLevelSeed();   // shared dice: a fresh seed for every level the host opens
            game::shareddice::SetSeed(seed);
            nextSync_ = WantedSyncMode();   // the new level's sync mode: in a MatchInfo ahead of the Travel
            EndLeases("level change");   // ahead of the Travel: every fighter is its owner's in the new level
            if (net_) {
                BroadcastInfo(NowMs());
                net_->Broadcast(kChanReliable, Encode(HostModMapFor(level)), true);   // modded maps: before the Travel, so a client can refuse in time
                net_->Broadcast(kChanReliable, Encode(dice::LevelSeedMsg{t.seq, seed}), true);   // before the Travel: it is set by the time the level's BeginPlay runs
                net_->Broadcast(kChanReliable, Encode(t), true);
            }
            const MatchConfig shown = lobby_.config();
            lobby_.SetPublicState(IsMenuLevel(level) ? "menu" : "playing", level.substr(level.find_last_of('/') + 1),
                                  lobby_.Progression() ? "" : ModeName(shown.format, shown.gear));
            HSM_LOG("host: travel #%u to %s (%zu settings) sent to lobby", t.seq, level.c_str(), t.settings.size());
            return true;
        }
        if (IsMenuLevel(level)) return true;           // clients can always go back to the menu
        HSM_LOG("client: in a lobby - waiting for the host to start the fight (leave the lobby to play solo)");
        return false;
    }

    void OnLevelLoaded() {
        abyss_.Clear(adapter_);   // its tiles went with the old level
        currentLevel_ = ObjectName(OuterOf(OuterOf(lastGameMode_)));   // GameMode -> PersistentLevel -> World
        progressionHub_ = InFightLevel() && currentLevel_.find("Hub") != std::string::npos && !adapter_.FreeModeActive();
        if (progressionHub_) HSM_LOG("session: %s is the Progression hub; no fighters, the game keeps its own input", currentLevel_.c_str());
        HSM_LOG("session: level loaded (%s)", currentLevel_.c_str());
        levelLoadedAt_ = NowMs();
        SetMyWillie(nullptr);                        // the old world's fighter is gone; the next tick names the new one
        peerSilence_.LevelLoaded(NowMs());   // the clients load too, and send nothing meanwhile
        ui::ChatDrop("level change");   // the new level's controller starts with the game's input
        agent::VmHookFlush();
        mirror_.OnLevel();
        mirrorDone_ = false; mirrorSince_ = 0;
        game::PaintDeferLevelChanged();
        if (ScriptHookActive()) ResolveScriptHooks();   // the level's Blueprint classes and overrides are loaded now
        snappedOwn_ = false; picksValid_ = false;
        held_.fill(Held{});                          // agent-held axes do not carry into the next level
        groundValid_ = false; foesStopped_ = false;
        if (FighterLevel() && !fightSettings_.empty()) {   // a session fight: the mode its travel set, whatever the load did
            const std::string changed = game::ChangedGameSettings(fightSettings_);
            if (!changed.empty())
                HSM_LOG("session: the level load changed the fight's settings (%s); set back (%zu)", changed.c_str(),
                        game::ApplyGameSettings(fightSettings_));
        }
        oneFight_ = InFightLevel() && !adapter_.FreeModeActive();   // a client has the host's GI settings (Travel) by now
        // Players are built for the lobby's tier whatever their gear (both sides read it from the Steam lobby; the host's
        // force_strength rules, a client has it from MatchInfo); MatchInfo keeps it current. 0.8.0-pre20, #294 #295 #301.
        {
            const bool lobbyMatch = steamMode() && lobby_.InLobby() && MatchLevel() && !lobby_.Progression();   // the Tavern's co-op fights keep their builds
            const MatchConfig lc = lobbyMatch ? lobby_.config() : MatchConfig{};
            adapter_.SetBuild(ForcedWeight(lc), lobbyMatch && (IsHost() ? cfg_.forceStrength : info_.forceStrength != 0), int(lc.tier));
        }
        adapter_.SetBare(steamMode() && lobby_.InLobby() && MatchLevel() && !lobby_.Progression() && IsBrawl(lobby_.config()));   // Brawl: no gear on any body
        if (!InFightLevel()) RestoreFoes();
        inputFixAt_ = net_ && FighterLevel() ? NowMs() + 1500 : 0;
        menuInputAt_ = InFightLevel() ? 0 : NowMs() + 500; menuInputRepeat_ = false;
        if (net_ && MatchLevel() && !bugHintShown_ && !bugHintAt_) bugHintAt_ = NowMs() + 8000;   // once the fight is under way
        probeSince_ = FighterLevel() ? std::max(NowMs(), 1u) : 0;
        hitKind_.clear(); fighterOfFrame_.clear(); otherActorOffset_.clear(); normalImpulseOffset_.clear();   // the old level's classes and functions are gone
        spawnIgnore_.clear();
        // The sync mode is fixed for the level: the host's pick at the travel, which its clients heard before the Travel.
        if (net_) SetSyncMode(IsHost() ? nextSync_ : clientNextSync_);
        // Host side: every actor of the old level is gone.
        for (Fighter& f : fighters_) { f.actor.Clear(); f.input.ReleaseAll(); }
        director_.OnLevel(FighterLevel(), NowMs(), travelSeq_);
        serverTravelling_ = false; serverAnchor_.reset(); serverPawnSeenAt_ = serverStripAt_ = 0; serverStripped_ = 0; anchorSearch_.OnLevel(NowMs());
        if (net_ && !IsHost()) hostLoss_.Reset(NowMs());   // the load itself froze the loop: that silence is not the host's (N1)
        bots_.clear(); match_ = Match{}; kills_ = KillLog{}; sidesValid_ = false; wave_ = 0; info_ = MatchInfo{}; groundSeen_ = false; clearCount_ = 0; nextGroundClear_ = 0; feed_.clear(); banner_.Reset();
        tints_.clear();
        ownDeadPending_ = false; ownRespawn_.Reset(); dropClearPending_ = false; mapGate_.Reset(); ownDiedAt_ = 0; mirrorBlob_.clear(); ownBlob_ = 0;
        // Client side: puppets died with the level; the host re-announces them after Ready.
        world_.OnLevel(); puppets_.clear();
        ownLeased_ = false; nativeButtons_ = 0; ResetLeases(); ResetHostDelay();   // the bodies went with the level
        levelGate_.OnLevel(game::SteadyNowMs()); nextGateProbe_ = 0; managerProbed_ = false; recreate_.Clear(); lostSlots_.clear();
        // Owner sync, client: the stock pawn the level gives player 0 is replaced by the dressed fighter the host
        // announces, once the level's own start-up cleanup is over (ClientGateOpen).
        if (net_ && !IsHost() && FighterLevel()) ownDeadPending_ = true;
        // Owner sync: every stand-in died with the level (the feeds stay: their owners keep streaming).
        for (uint8_t e = 1; e <= kMaxWillies; ++e) { standIns_.Detach(e); severs_.Forget(e); claims_.Forget(e); fx_.Forget(e); equip_.Forget(e); }
        gore_.Clear();
        grabs_.Clear();
        props_.Clear();                              // every prop actor died with the level
        lives_.clear(); gaveUp_.clear(); ownDeathReported_ = false; respawnQueue_.clear();
        if (!net_) return;
        if (IsHost()) { ShareHostLoadout(); return; }
        if (welcomed_) UploadLoadout();             // before Ready, so the host never spawns us in a stale loadout
        if (travelSeq_ && hostPeer_) { Ready r; r.travelSeq = travelSeq_; r.level = pendingLevel_; net_->Send(hostPeer_, kChanReliable, Encode(r), true); }
    }

    // ------------------------------------------------------------------ host
    struct Fighter { ActorRef actor; InputApplier input; uint32_t inputsRx = 0, lastInputAt = 0; uint8_t lockTarget = 0; };   // input: host physics, its player's
    struct Peer { uint8_t slot = 0; uint64_t key = 0; BlobAssembler blobs; uint32_t helloAt = 0; ChatRateLimiter chat; MsgBudget budget; std::string name, modBuild;
                   uint32_t reportShareAt = 0; bool reportShared = false; };
    static constexpr uint32_t kHelloRepeatMs = 5000;

    void BecomeHost() {
        roster_.ReserveHostSlot(!localFighter_);
        roster_.Clear();
        if (localFighter_) roster_.Add(kHostKey, MyName());
        director_ = HostDirector{}; if (localFighter_) director_.OnJoin(0, NowMs());
        director_.OnLevel(InFightLevel(), NowMs(), 0);
        hostExport_.clear(); ShareHostLoadout();
        // The sync mode: the lobby's pick, from the next map on (OnOpenLevel). A host taking over mid-fight (a migration)
        // keeps the level's mode until the map is opened again.
        if (!InFightLevel()) SetSyncMode(WantedSyncMode());
        nextSync_ = syncMode_;              // what MatchInfo announces until the next travel picks again
        if (localFighter_) HSM_LOG("host: hosting as slot 0 (up to %d fighters), %s", kMaxPlayers, SyncName(syncMode_));
        else HSM_LOG("server: no local fighter, slot 0 reserved (up to %d clients), %s", kMaxPlayers, SyncName(syncMode_));
        if (!localFighter_ && !serverReadyLogged_) { serverReadyLogged_ = true; HSM_LOG("hsm: server ready"); }
        StartOwner();
    }

    // Owner sync is the only sync mode and it needs the script VM hook (claims, the stock flow guard). Without it no
    // session starts: the host leaves its lobby / closes the transport, a joiner leaves the host's.
    bool ScriptHookMissing() {
        if (ScriptHookActive()) return false;
        HSM_LOG("session: the script VM hook is unavailable; multiplayer needs it (host sync was removed in 0.8.0), leaving the session");
        const char* msg = "Multiplayer cannot start: this game build is not supported by HSMMod (script VM hook unavailable). Update HSMMod.";
        ui::ShowLine("HSMMod", style::kBone, msg);
        notice_ = msg;
        const bool fight = InFightLevel();
        welcomed_ = false;
        if (steamMode()) lobby_.Leave();
        ResetNetwork("no script VM hook");
        if (fight) game::OpenLevel(lastGameMode_, kMenuLevel, true, "");
        return true;
    }

    // Owner sync on this machine: a fresh stream clock, and the claims hooks with what they need to know of the session.
    void StartOwner() {
        streams_.Start(game::SteadyNowMs());
        streams_.SetEpochOf([this](uint8_t e) { return leases_.Epoch(e); });   // rev 32: hand-off leases
        GuardStockFlow();
        game::Claims::Env env;
        env.ownedEntity = [this](const UObject* w) { return OwnedEntityOf(w); };
        env.ownedActor = [this](uint8_t e) { return OwnedActorOf(e); };
        env.send = [this](const combat::HitClaim& c0) {
            if (HostPhysics() && !IsHost()) return;   // host physics: every blow is the host's own
            combat::HitClaim c = c0; c.victimLife = LifeOf(c.victim);
            if (IsHost()) RouteHostClaim(kHostSlot, c);
            else if (net_ && hostPeer_) {
                net_->Send(hostPeer_, kChanReliable, Encode(HitClaimMsg{c}), true);
                MeshCopy(MsgType::HitClaim, c.victim, Encode(HitClaimMsg{c}), true);   // to the victim's owner
                MeshCopy(MsgType::HitFx, c.victim, Encode(HitFxMsg{c}), true);         // to everyone else
            }
        };
        env.allowed = [this](uint8_t attacker, uint8_t victim) {
            return CombatEnabled() &&
                   HitAllowed(true, KnownTeam(attacker), KnownTeam(victim), IsHost() ? match_.config().friendlyFire : info_.friendlyFire);
        };
        env.prop = [this](UObject* actor, game::ObjectSync::Touch& out) { return props_.PropOf(actor, out); };
        env.propActor = [this](uint64_t id) { return props_.ActorOf(id); };
        env.myEntity = [this]() { return IsHost() ? HostEntity() : world_.myEntity(); };
        env.sendRemote = [this](const sync::RemoteWriteMsg& m) { SendRemote(m); };
        if (!claims_.Start(adapter_, standIns_, streams_, std::move(env)))
            HSM_LOG("session: WARNING owner sync without the script VM hook: blows between this machine and stand-ins do nothing");
        // Blood and wounds on fighters simulated here, shown on every other machine's stand-in of them.
        game::FxSync::Env fenv;
        fenv.ownedEntity = [this](const UObject* w) { return OwnedEntityOf(w); };
        fenv.ownedActor = [this](uint8_t e) { return OwnedActorOf(e); };
        fenv.noteSpawn = [this] { props_.NoteSpawn(); };   // armour came off: the loose piece is looked for now, not at the next census
        fenv.send = [this](const combat::PaintFxMsg& m) {
            if (!net_) return;
            if (IsHost() && combat::IsStripRequest(m)) {   // to the fallen fighter's owner only
                if (std::optional<uint32_t> to = PeerOfSlot(OwnerSlotOf(m.entity, leases_.holders()))) net_->Send(*to, kChanReliable, Encode(m), true);
            } else if (IsHost()) net_->Broadcast(kChanReliable, Encode(m), true);
            else if (hostPeer_) {
                net_->Send(hostPeer_, kChanReliable, Encode(m), true);
                if (!combat::IsStripRequest(m)) MeshCopy(MsgType::PaintFx, 0, Encode(m), true);   // a strip request goes through the host
            }
        };
        fx_.Start(adapter_, standIns_, std::move(fenv));
        // Weapons drawn and sheathed by fighters simulated here, matched by every other machine's stand-in of them.
        game::EquipSync::Env eenv;
        eenv.send = [this](const combat::EquipStateMsg& m) {
            if (!net_) return;
            if (IsHost()) net_->Broadcast(kChanReliable, Encode(m), true);
            else if (hostPeer_) { net_->Send(hostPeer_, kChanReliable, Encode(m), true); MeshCopy(MsgType::Equip, 0, Encode(m), true); }
        };
        equip_.Start(adapter_, standIns_, std::move(eenv));
        game::Severs::Env sev;
        sev.ownedEntity = [this](const UObject* w) { return OwnedEntityOf(w); };
        sev.ownedActor = [this](uint8_t e) { return OwnedActorOf(e); };
        sev.sendSever = [this](const combat::SeverClaim& c0) {
            combat::SeverClaim c = c0; c.victimLife = LifeOf(c.victim);
            if (IsHost()) RouteHostSever(kHostSlot, c);
            else if (net_ && hostPeer_) {
                net_->Send(hostPeer_, kChanReliable, Encode(combat::SeverClaimMsg{c}), true);
                MeshCopy(MsgType::SeverClaim, c.victim, Encode(combat::SeverClaimMsg{c}), true);
            }
        };
        sev.sendNotice = [this](const combat::SeverClaim& c0) {
            combat::SeverClaim c = c0; c.victimLife = LifeOf(c.victim);
            if (IsHost()) RouteHostNotice(kHostSlot, c);
            else if (net_ && hostPeer_) {
                net_->Send(hostPeer_, kChanReliable, Encode(combat::SeverNoticeMsg{c}), true);
                MeshCopy(MsgType::SeverNotice, c.victim, Encode(combat::SeverNoticeMsg{c}), true);
            }
        };
        sev.sendStuck = [this](const combat::StuckBladeMsg& m) {
            if (IsHost()) RouteHostStuck(kHostSlot, m);
            else if (net_ && hostPeer_) { net_->Send(hostPeer_, kChanReliable, Encode(m), true); MeshCopy(MsgType::StuckBlade, m.victim, Encode(m), true); }
        };
        sev.allowed = [this](uint8_t attacker, uint8_t victim) {
            return CombatEnabled() && HitAllowed(true, KnownTeam(attacker), KnownTeam(victim), IsHost() ? match_.config().friendlyFire : info_.friendlyFire);
        };
        sev.nativeShown = [this](uint8_t e, uint8_t part) { gore_.NoteNativeSever(e, part); };
        sev.nativeLost = [this](uint8_t e, uint8_t part) { gore_.NoteNativeLost(e, part); };
        severs_.Start(adapter_, standIns_, streams_, std::move(sev));
        game::StandInGore::Env gore;
        gore.standInActor = [this](uint8_t e) { return standIns_.ActorOf(e); };
        gore_.Start(adapter_, std::move(gore));
        standIns_.SetDeadBones([this](uint8_t e) { return gore_.DeadBones(e); });
        standIns_.SetHeldWeapons([this](uint8_t e) { return severs_.HeldWeapons(e); });   // its blades held in my fighter
        standIns_.SetHeldBones([this](uint8_t e) { return grabs_.HeldBones(e); });        // its bones held by my fighter's grab
        standIns_.SetStuckBones([this](uint8_t e) { return severs_.StuckBones(e); });      // its bones my blades are stuck in
        standIns_.SetGrabNoSnap(cfg_.grabDrag);
        adapter_.SetLean(cfg_.driveLean);
        hostDelay_.Configure(cfg_.hostInputDelay);
        game::GrabSync::Env genv;
        genv.standInEntity = [this](const UObject* w) { return standIns_.EntityOf(w); };
        genv.standInActor = [this](uint8_t e) { return standIns_.ActorOf(e); };
        genv.dead = [this](uint8_t e) { const VitalsMsg* v = standIns_.Vitals(e); return v && (v->flags & 1); };
        genv.down = [this](uint8_t e) { const VitalsMsg* v = standIns_.Vitals(e); return v && ((v->flags & 7) || v->health <= 0.f); };
        genv.send = [this](const std::vector<uint8_t>& b, bool reliable) {
            const uint8_t chan = reliable ? kChanReliable : kChanUnreliable;
            if (!net_) return;
            if (IsHost()) net_->Broadcast(chan, b, reliable);
            else if (hostPeer_) { net_->Send(hostPeer_, chan, b, reliable); MeshCopy(MsgType::GrabState, 0, b, reliable); }
        };
        genv.drag = cfg_.grabDrag;
        genv.sendBreak = [this](uint8_t held, uint8_t grabber, uint8_t hand) {
            sync::RemoteWriteMsg m;
            m.kind = sync::RemoteKind::GrabBreak; m.from = held; m.target = grabber; m.bone = hand; m.seq = ++remoteSeq_;
            SendRemote(m);
        };
        grabs_.Start(adapter_, std::move(genv));
        game::ObjectSync::Env penv;
        penv.host = IsHost();
        penv.mySlot = [this] { return IsHost() ? kHostSlot : mySlot_; };
        penv.ownedEntity = [this](const UObject* w) { return OwnedEntityOf(w); };
        penv.send = [this](const std::vector<uint8_t>& b, bool reliable) {
            if (!net_) return;
            const auto chan = reliable ? kChanReliable : kChanUnreliable;
            if (IsHost()) net_->Broadcast(chan, b, reliable);
            else if (hostPeer_) {
                net_->Send(hostPeer_, chan, b, reliable);
                MsgType t;
                // States and events also go straight to the mesh peers; leases and announces stay with the host.
                if (PeekType(b.data(), b.size(), t) && (t == MsgType::BodyState || t == MsgType::ObjectEvent)) MeshCopy(t, 0, b, reliable);
            }
        };
        // Stand-in hands matched to their owners' poses (pickups, leftover weapons): Equip's view of each hand, gore's cut-off hands.
        penv.equipToken = [this](uint8_t e, int hand, uint8_t& token) { return equip_.OwnerToken(e, hand, token); };
        penv.mayBeDraw = [this](uint8_t e, uint32_t cls) { return equip_.MayBeDraw(e, cls); };
        penv.mayBeSheathe = [this](uint8_t e, int hand) { return equip_.MayBeSheathe(e, hand); };
        penv.deadBones = [this](uint8_t e) { return gore_.DeadBones(e); };
        penv.handsChanged = [this](uint8_t e) { equip_.Replan(e); };
        // Level props: the host censuses a fight level from its first tick in it; a client matches once its level gate is open.
        penv.syncStructures = cfg_.syncStructures;
        penv.fightLevel = [this] { return FighterLevel() && (IsHost() || levelGate_.Open()); };
        const bool hostNow = penv.host;
        props_.Start(adapter_, standIns_, std::move(penv));
        // The new host after a migration: the objects the old host announced (every machine heard them) keep their ids.
        if (hostNow && !migrateObjects_.empty()) { props_.AdoptKnown(migrateObjects_); migrateObjects_.clear(); }
    }

    // Owner sync: the game's single-player win/lose flow must not run in a session match. "Event Lose Match" on a
    // player fighter (ours, every stand-in and every mod-spawned fighter have Player set) opens UI_Lose, which pauses
    // the game and leads to "return to tavern / another fight"; "Force Finish Match" opens UI_NextFIght; "Call Minus
    // Enemy" counts a dead non-player down to the stock win screen. The session's match referee decides instead.
    // Non-player fighters (host bots) still run Event Lose Match for its "Give Up" AI state; its Call Minus Enemy is
    // blocked. Active only while stockFlowGuard_ (owner sync, in a match level, joined); host sync never registers it.
    static constexpr const char* kStockFlowOwner = "stockflow";
    void GuardStockFlow() {
        ClearScriptHooks(kStockFlowOwner);
        auto block = [this](const char* what) {
            return [this, what](ScriptCall& c) {
                if (!stockFlowGuard_) return false;
                if (std::strcmp(what, "Event Lose Match") == 0) {
                    bool player = false;
                    if (!GetBool(c.self(), "Player", player) || !player) return false;
                    RingOut(c.self(), "a lose volume");   // the Pit, Yard and Slums volumes, the fence bags
                }
                if (++stockFlowBlocked_ % 50 == 1) HSM_LOG("session: blocked stock \"%s\" during an owner-sync match (%u so far)", what, stockFlowBlocked_);
                return true;
            };
        };
        // Literal names, one call each: tools/check_reflection.py verifies HookScript names against the game.
        int n = HookScript(kStockFlowOwner, "Willie_BP_C", "Event Lose Match", block("Event Lose Match"));
        n += HookScript(kStockFlowOwner, "Willie_BP_C", "Force Finish Match", block("Force Finish Match"));
        n += HookScript(kStockFlowOwner, "Willie_BP_C", "Call Minus Enemy", block("Call Minus Enemy"));
        HSM_LOG("session: stock win/lose flow guard %s (%d function(s) hooked now)", ScriptHookActive() ? "on" : "NOT ACTIVE (no script VM hook)", n);
    }

    // A ring-out (sync audit items 35, 41): a level's lose volume (Pit, Yard, Slums, the fence bags: Event Lose Match;
    // Cutting: UI_Lose) touched by a player fighter simulated here. Only that machine reports it: a stand-in's touch is
    // still blocked and dropped, so one ring-out is one report. It ends the fight the way G does: a give-up whose death
    // credits the last attacker (Progression co-op: the fight is lost; bot formats: the fighter dies).
    void RingOut(UObject* w, const char* how) {
        const uint8_t e = OwnedEntityOf(w);
        if (!stockFlowGuard_ || !e || bots_.count(e)) return;
        const uint32_t now = NowMs();
        if (adapter_.IsDead(w) || !EntityAlive(e) || (ringOutAt_.count(e) && now - ringOutAt_[e] < kRingOutEveryMs)) return;
        const MatchInfo mi = Extrapolate(info_, infoAt_, now);
        const bool fought = mi.phase == uint8_t(MatchPhase::Live) || mi.phase == uint8_t(MatchPhase::SuddenDeath);
        if (!fought || (mi.format == uint8_t(Format::Duel) && InRoundGap(mi))) return;
        ringOutAt_[e] = now;
        const bool coop = oneFight_ && mi.format == uint8_t(Format::PlayersVsBots);
        HSM_LOG("match: #%u rang out (%s)%s", e, how, coop ? ": the fight is lost" : "");
        if (!IsBotFormat(Format(mi.format)) || coop) {
            if (IsHost()) HandleGiveUp(e, now);
            else if (hostPeer_) { GiveUpMsg g; g.entity = e; net_->Send(hostPeer_, kChanReliable, Encode(g), true); }
        }
        if (!coop) adapter_.TriggerDeath(w);   // the normal death path credits the attacker
    }
    // The entity of a fighter this machine simulates (owner sync): its own fighter, and on the host its bots.
    // Host physics: on the host every session fighter (its clients' too), on a client none. A hand-off: the fighters this
    // machine holds too; my own not while another machine holds it (a client holding nothing then).
    uint8_t OwnedEntityOf(const UObject* w) {
        if (!w) return 0;
        if (!IsHost() && (HostPhysics() || MyFighterHanded())) return 0;
        if (w == myWillie_.load()) return MyFighterHanded() ? 0 : MyEntity();
        if (IsHost())
            for (auto& [e, ref] : bots_) if (ref.obj == w && adapter_.StillValid(ref.obj, ref.index)) return e;
        if (IsHost() && (HostPhysics() || leases_.Any()))
            for (uint8_t slot = 1; slot < kMaxFighters; ++slot)
                if (const ActorRef& a = fighters_[slot].actor; a.obj == w && HostDrives(Roster::EntityForSlot(slot)) && adapter_.StillValid(a.obj, a.index))
                    return Roster::EntityForSlot(slot);
        if (!IsHost() && leases_.Any())
            for (auto& [e, ref] : puppets_) if (ref.obj == w && HeldHere(e) && adapter_.StillValid(ref.obj, ref.index)) return e;
        return 0;
    }
    // My own fighter as an (object, index) pair, so a level change that frees it (the menu after a fight) is seen as stale
    // instead of read through (public #43: a StuckBlade right after the load reached ObjectIndex on the old world's Willie).
    void SetMyWillie(UObject* w) {
        myWillie_.store(w);
        myWillieIndex_.store(w ? ue::ObjectIndex(w) : -1);   // w is the engine's current pawn, read this frame
    }
    // The player's own fighter is the game's, built from this machine's save before the mod sees it, and under owner sync
    // it is the body that is simulated: CopyPassport only builds the fighters the mod spawns, so pre20 left it at the
    // save's build (a Free Mode Weight 2.0 still lifted a 0.5 one, not the other way round). Once per body and build.
    // Diagnostic (scratch branch): which function of the player's fighter starts the game's own give-up (the kneel, the
    // dropped weapon and the voice lines). Once per body: the class's functions with a give-up word in the name, and every
    // input-action event. While G is held (ProbeGiveUpHeld) the give-up flags are logged twice a second.
    void ProbeGiveUpFunctions() {
        UObject* w = MyWillieLive();
        if (!w || w == giveUpProbed_) return;
        giveUpProbed_ = w;
        const UClass* cls = ue::ClassOf(w);
        auto lower = [](std::string x) { for (char& c : x) c = char(tolower(static_cast<unsigned char>(c))); return x; };
        int n = 0, listed = 0;
        for (UFunction* f : ue::FunctionsWithPrefix(cls, "")) {
            ++n;
            const std::string name = ue::ObjectName(reinterpret_cast<UObject*>(f)), l = lower(name);
            const bool give = l.find("give") != std::string::npos || l.find("surrender") != std::string::npos ||
                              l.find("kneel") != std::string::npos || l.find("yield") != std::string::npos;
            if (!give && l.rfind("inpactevt_", 0) != 0) continue;
            std::string params;
            for (ue::FProperty* p : ue::Params(f)) params += (params.empty() ? "" : ", ") + ue::PropertyName(p);
            if (++listed <= 120) HSM_LOG("probe: %s %s (%s)", give ? "give-up function" : "input event", name.c_str(), params.c_str());
        }
        HSM_LOG("probe: %s has %d function(s) of its own, %d listed", ue::ObjectName(reinterpret_cast<UObject*>(const_cast<UClass*>(cls))).c_str(), n, listed);
    }
    void ProbeGiveUpHeld(UObject* own, uint32_t now, bool skip) {
        if (!own || now < probeHeldAt_ || probeLines_ > 80) return;
        probeHeldAt_ = now + 500;
        bool a = false, b = false;
        GetBool(own, "Give Up", a); GetBool(own, "Give Up 2 (Temp)", b);
        HSM_LOG("probe: G down (%s) Give Up=%d Give Up 2=%d", skip ? "break" : "wave", int(a), int(b));
        ++probeLines_;
    }
    void TickOwnBuild() {
        if (!adapter_.BuildOn()) { ownBuildActor_ = nullptr; return; }
        UObject* w = MyWillieLive();
        if (!w || (w == ownBuildActor_ && adapter_.ForcedBuild() == ownBuildWeight_)) return;
        ownBuildActor_ = w; ownBuildWeight_ = adapter_.ForcedBuild();
        adapter_.ApplyBuildLive(w);
    }
    UObject* MyWillieLive() {
        UObject* w = myWillie_.load();
        return w && adapter_.StillValid(w, myWillieIndex_.load()) ? w : nullptr;
    }
    // Sever, FX, grab and claim messages mean something only inside the fight this machine is in; before the Welcome, in a
    // menu or the hub they name fighters of a world that is gone or not yet built.
    static bool IsFightMsg(MsgType t) {
        return t == MsgType::HitClaim || t == MsgType::SeverClaim || t == MsgType::SeverNotice || t == MsgType::StuckBlade ||
               t == MsgType::PaintFx || t == MsgType::Equip || t == MsgType::GrabState || t == MsgType::RemoteWrite;
    }
    bool FightMsgsOpen() const { return hsm::FightMsgsOpen(IsHost(), welcomed_, FighterLevel()); }
    UObject* OwnedActorOf(uint8_t e) {
        if (!e || (!IsHost() && (HostPhysics() || MyFighterHanded()))) return nullptr;
        if (e == MyEntity()) return MyFighterHanded() ? nullptr : MyWillieLive();
        if (IsHost() && e >= 2 && e <= kMaxFighters && HostDrives(e)) return EntityActor(e);   // a client's fighter, simulated here
        if (!IsHost() && HeldHere(e)) return ClientActor(e);                                   // another's fighter, held here
        return IsHost() && e >= kFirstBotEntity ? EntityActor(e) : nullptr;
    }

    // ------------------------------------------------------------------ host physics (0.9.0)
    // One simulation: the host's. Every client's fighter is a real Willie on the host, played from that client's input
    // through the Willie's own input events (Adapter::BindInputs); no machine but the host streams anything. On a client
    // every fighter, its own included, is a replica of the host's (StandIns::SetReplica). The mode is fixed per level: the
    // host's pick at the travel (OnOpenLevel), sent in MatchInfo ahead of the Travel; a joiner takes the Welcome's.
    bool HostPhysics() const { return syncMode_ == SyncMode::Host; }
    // Hand-off (sync/hsm_handoff.h): under owner sync the machine whose fighter grabbed or stabbed another fighter holds
    // that one too while they are coupled. MySlot: this machine's roster slot (kNoSlot before the Welcome).
    uint8_t MySlot() const { return IsHost() ? kHostSlot : welcomed_ ? mySlot_ : kNoSlot; }
    // Another machine holds my fighter: it is a replica here, as under host physics, and what I press is played there.
    bool MyFighterHanded() const { return !HostPhysics() && MyEntity() && leases_.Active(MyEntity()); }
    // This machine holds another's fighter: a live fighter here, played from its player's input.
    bool HeldHere(uint8_t e) const { return !HostPhysics() && MySlot() != kNoSlot && leases_.Active(e) && leases_.Holder(e) == MySlot(); }
    // Host: a client's fighter is simulated here (every one under host physics; one held here).
    bool HostDrives(uint8_t e) const { return HostPhysics() || HeldHere(e); }
    bool ClientHanded(uint8_t slot) const { return slot >= 1 && slot < kMaxFighters && leases_.Active(Roster::EntityForSlot(slot)); }
    static const char* SyncName(SyncMode m) { return m == SyncMode::Host ? "host physics" : "owner sync"; }
    SyncMode WantedSyncMode() const {
        // Progression co-op has no Sync row (hsm_lobby_scene.h): it stays on owner sync, whatever the row last said.
        if (!IsServer() && steamMode() && lobby_.InLobby() && lobby_.Progression()) return SyncMode::Owner;
        const MatchConfig c = IsServer() ? ServerMatchConfig() : steamMode() && lobby_.InLobby() ? lobby_.config() : MatchConfig{};
        return c.hostPhysics ? SyncMode::Host : SyncMode::Owner;
    }
    void SetSyncMode(SyncMode m) {
        if (m != syncMode_) HSM_LOG("session: %s from this level on (was %s)", SyncName(m), SyncName(syncMode_));
        syncMode_ = m;
        standIns_.SetReplica(HostPhysics() && !IsHost());
    }
    // What only a machine that simulates fighters or objects sends: under host physics no client does.
    static bool ClientSimMsg(MsgType t) {
        switch (t) {
            case MsgType::Pose: case MsgType::Vitals: case MsgType::HitClaim: case MsgType::HitFx: case MsgType::SeverClaim:
            case MsgType::SeverNotice: case MsgType::StuckBlade: case MsgType::PaintFx: case MsgType::Equip: case MsgType::GrabState:
            case MsgType::BodyState: case MsgType::ObjectAnnounce: case MsgType::ObjectEvent: case MsgType::PropLease:
            case MsgType::DeathReport: case MsgType::RemoteWrite:
                return true;
            default:
                return false;
        }
    }
    // The entity this client simulates: its own under owner sync, none under host physics or while handed (a replica).
    uint8_t ClientOwnEntity() const { return HostPhysics() || MyFighterHanded() ? 0 : world_.myEntity(); }

    // Another player's input, played on its fighter here (host physics, a hand-off; mouse deltas spread over my frames,
    // InputApplier::Tick).
    void DriveFighter(InputApplier& in, UObject* w, float dt) {
        if (!adapter_.BindInputs(w)) return;
        in.Target(w);
        InputApplier::Step s = in.Tick(dt * 1000.f);
        // Not played on the host (sync audit item 15): the camera lock flips the host's own setting (Willie_BP.cpp:11280),
        // the middle mouse button runs a debug damage trace (:11067).
        auto played = [](Action a) { return a != Action::ToggleCameraLock && a != Action::MiddleMouse; };
        for (Action a : s.released) if (played(a)) adapter_.FireAction(w, a, false);
        for (Action a : s.pressed) if (played(a)) adapter_.FireAction(w, a, true);
        // The client's mouse axes come scaled by its own look settings; the game multiplies by mine again (item 16).
        float sens = 1.f; bool invert = false;
        if (adapter_.MouseLook(sens, invert) && std::fabs(sens) > 0.01f) {
            s.axes[size_t(Axis::TurnMouse)] /= sens;
            s.axes[size_t(Axis::LookMouse)] /= invert ? -sens : sens;
        }
        // Quiet axes (hsm_input.h kQuietMs): an axis the game already holds at 0 is not fed; each event is a ProcessEvent
        // into a Blueprint handler, the look handlers the dearest (lag notes, KNOWN_ISSUES 0.8.0-pre14). drive_lean adds
        // a held guard (hsm_drivelean.h); the move, arm and look axes are fed as before (the reasons are there).
        if (cfg_.driveLean) { LeanFeed& lean = leanFeed_[&in]; lean.Target(w); lean.Trim(s.axes, s.skip); }
        for (size_t i = 0; i < kAxisCount; ++i) {
            if (s.skip[i]) { ++driveSkipped_; continue; }
            ++driveFed_;
            adapter_.ApplyAxis(w, Axis(i), s.axes[i]);
        }
        adapter_.KeepRemoteFighter(w, in.latest().controlYaw, in.latest().controlPitch);
    }
    // Host: a client's held buttons let go (it left; its body stays for a rejoin).
    void ReleaseInputs(uint8_t slot) {
        if (slot == 0 || slot >= kMaxFighters) return;
        InputApplier::Step s = fighters_[slot].input.ReleaseAll();
        fighters_[slot].lockTarget = 0;
        const InputApplier& in = fighters_[slot].input;
        if (in.replayed() + in.maskFixes()) HSM_LOG("input: e%u left: %u button edge(s) played from the counts, %u from the held mask", unsigned(slot) + 1u, in.replayed(), in.maskFixes());
        const ActorRef& a = fighters_[slot].actor;
        if (a.obj && adapter_.StillValid(a.obj, a.index))
            for (Action act : s.released) adapter_.FireAction(a.obj, act, false);
    }

    // ------------------------------------------------------------------ hand-off (rev 32, sync/hsm_handoff.h)
    // Peer to peer: the machine whose fighter grabbed or stabbed another fighter asks that fighter's owner for it; the
    // owner grants it (a lease, written through the host, which only relays and checks). The holder then simulates
    // both bodies, the held one from its player's input (RelayInput), and gives it back the moment they part.
    UObject* AnyActor(uint8_t e) { return IsHost() ? EntityActor(e) : ClientActor(e); }
    UObject* HeldActor(uint8_t e) { return AnyActor(e); }
    InputApplier& HeldInput(uint8_t e) { return IsHost() ? fighters_[e - 1].input : heldInput_[e]; }

    // A hand of a held to grab (R/L Grab Pressed) within reach of b's body.
    bool GrabOn(UObject* a, UObject* b, bool keep) {
        bool r = false, l = false;
        GetBool(a, "R Grab Pressed", r); GetBool(a, "L Grab Pressed", l);
        if (!r && !l) return false;
        static const char* const kBodyBones[] = {"pelvis", "spine_02", "spine_03", "neck_01", "head", "upperarm_l", "upperarm_r", "lowerarm_l",
                                                 "lowerarm_r", "hand_l", "hand_r", "thigh_l", "thigh_r", "calf_l", "calf_r"};
        std::vector<sync::HandOffPoint> body;
        for (const char* bone : kBodyBones) if (FVector p; adapter_.BoneLocation(b, bone, p)) body.push_back({p.x, p.y, p.z});
        const double reach = keep ? sync::kGrabKeepUu : sync::kGrabReachUu;
        for (int side = 0; side < 2; ++side) {
            FVector h;
            if ((side == 0 ? r : l) && adapter_.BoneLocation(a, side == 0 ? "hand_r" : "hand_l", h) && sync::HandOn({h.x, h.y, h.z}, body, reach))
                return true;
        }
        return false;
    }
    // a's native grab has latched on b's body: Grabbed R/L and the held component is b's mesh (Willie_BP ReceiveTick,
    // GrabSync::ReadHands). Only a live body here can be latched: a stand-in's never is, so the ask goes on GrabOn.
    bool GrabLatched(UObject* a, UObject* b) {
        UObject* mesh = adapter_.Mesh(b);
        if (!mesh) return false;
        for (int side = 0; side < 2; ++side) {
            bool g = false;
            if (!(side == 0 ? GetBool(a, "Grabbed R", g) : GetBool(a, "Grabbed L", g)) || !g) continue;
            UObject** comp = side == 0 ? Field<UObject*>(a, "Grab Component R") : Field<UObject*>(a, "Grab Component L");
            if (comp && *comp == mesh) return true;
        }
        return false;
    }
    // Held here: still coupled? A latched native grab either way round, a stuck blade either way, or a hand to grab
    // within keep reach (CoupledWhy). The grab can latch only once the body is live here, frames after the take, and
    // the hand that asked has often slid off it by then: no "parted" within kLatchMs of the take (0.8.0-pre13 session:
    // 38 hand-offs, most given back the frame they came, every switch neutralising and reviving the body).
    static constexpr double kLatchMs = 600.0, kReAskMs = 500.0;
    bool StillCoupled(UObject* b, UObject* v, double now, uint8_t e) {
        if (GrabLatched(b, v) || GrabLatched(v, b)) return true;
        if (CoupledWhy(b, v, true, true)) return true;
        return now - leaseSince_[e] < kLatchMs;
    }
    // Why a and b are coupled now (Couple bits): a grabs b or a's blade is stuck in b; both: either way round.
    uint8_t CoupledWhy(UObject* a, UObject* b, bool keep, bool both) {
        auto stuckIn = [&](UObject* x, UObject* y) {
            for (UObject* t : adapter_.StuckTargets(x)) if (t == y) return true;
            return false;
        };
        uint8_t why = 0;
        if (stuckIn(a, b) || (both && stuckIn(b, a))) why |= sync::kCoupleStuck;
        FVector pa, pb;
        if (adapter_.BoneLocation(a, "pelvis", pa) && adapter_.BoneLocation(b, "pelvis", pb) &&
            sync::HandOffDist({pa.x, pa.y, pa.z}, {pb.x, pb.y, pb.z}) <= sync::kCoupleNearUu &&
            (GrabOn(a, b, keep) || (both && GrabOn(b, a, keep))))
            why |= sync::kCoupleGrab;
        return why;
    }
    bool StandInDead(uint8_t e) { const VitalsMsg* v = standIns_.Vitals(e); return v && (v->flags & 1); }

    // Every frame: give back what parted, then ask for what my fighters (mine; the host's bots too) grabbed or stabbed.
    void HandOffTick(double now) {
        if (HostPhysics() || !net_ || MySlot() == kNoSlot || !FighterLevel()) return;
        for (uint8_t e = 1; e <= kMaxFighters; ++e) {
            if (!HeldHere(e)) continue;
            const sync::Lease l = leases_.Of(e);
            UObject* v = HeldActor(e);
            UObject* b = AnyActor(l.by);
            if (!v || !taken_[e]) { WriteLease(e, kNoSlot, 0, 0, "no body here"); continue; }
            if (adapter_.IsDead(v)) { ReportHeldDeath(e, v, now); continue; }   // kept until its respawn
            if (!b) { WriteLease(e, kNoSlot, 0, 0, "no grabber here"); continue; }
            if (!StillCoupled(b, v, now, e)) WriteLease(e, kNoSlot, 0, 0, "they parted");
        }
        if (!cfg_.handOff) return;   // hand_off=0 (the default): ask for nothing; a lease held above is still given back
        std::vector<std::pair<uint8_t, UObject*>> home;
        if (UObject* me = MyWillieLive(); me && !MyFighterHanded() && !adapter_.IsDead(me)) home.push_back({MyEntity(), me});
        if (IsHost())
            for (auto& [e, ref] : bots_) if (adapter_.StillValid(ref.obj, ref.index) && !adapter_.IsDead(ref.obj)) home.push_back({e, ref.obj});
        for (auto& [h, hw] : home)
            for (uint8_t e = 1; e <= kMaxWillies; ++e) {
                if (e == h || e == MyEntity() || HeldHere(e) || (IsHost() && e >= kFirstBotEntity)) continue;
                UObject* ow = AnyActor(e);
                if (!ow || standIns_.ActorOf(e) != ow || StandInDead(e)) continue;
                const uint8_t why = CoupledWhy(hw, ow, false, false);
                if (!why) { if (asking_[e] == h) asking_[e] = 0; continue; }
                if (now - askedAt_[e] < sync::kAskEveryMs || now < reAskAt_[e] || now < reAskAt_[h]) continue;
                if (e >= kFirstBotEntity) {
                    // A bot is the host's alone: my fighter goes to the host, which then has both bodies.
                    if (IsHost() || h != MyEntity() || leases_.Active(h) || leases_.HoldsAny(MySlot())) continue;
                    askedAt_[e] = now;
                    WriteLease(h, kHostSlot, e, why, "coupled to a bot");
                    continue;
                }
                if (leases_.Active(e) || leases_.HoldsAny(sync::LeaseOwner(e))) continue;   // held already, or its machine holds one
                askedAt_[e] = now; asking_[e] = h;
                sync::HandOffAskMsg a; a.victim = e; a.by = h; a.why = why;
                if (IsHost()) { if (std::optional<uint32_t> to = PeerOfSlot(sync::LeaseOwner(e))) net_->Send(*to, kChanReliable, Encode(a), true); }
                else if (hostPeer_) net_->Send(hostPeer_, kChanReliable, Encode(a), true);
            }
    }
    // The owner of the victim: grant it, or not (sync::JudgeAsk; my fighter alive, in a fight).
    void AnswerAsk(const sync::HandOffAskMsg& m, uint8_t askerSlot) {
        const double now = game::SteadyNowMs();
        const bool askingBack = asking_[m.by] == m.victim && now - askedAt_[m.by] < 2 * sync::kAskEveryMs;
        sync::AskVerdict v = sync::JudgeAsk(leases_, m, askerSlot, MySlot(), askingBack);
        UObject* me = MyWillieLive();
        if (v == sync::AskVerdict::Grant && (!cfg_.handOff || HostPhysics() || !FighterLevel() || !me || adapter_.IsDead(me) || m.victim != MyEntity()))
            v = sync::AskVerdict::Wrong;
        if (v != sync::AskVerdict::Grant) {
            if (++asksRefused_ <= 20 || asksRefused_ % 100 == 0)
                HSM_LOG("%s: hand-off ask for entity %u by entity %u (slot %u) refused (%d)", IsHost() ? "host" : "client", m.victim, m.by, askerSlot, int(v));
            return;
        }
        WriteLease(m.victim, askerSlot, m.by, m.why, "asked");
    }
    // This machine writes a lease (an owner's grant, a holder's give-back, the host as referee): applied here, then sent.
    void WriteLease(uint8_t e, uint8_t holder, uint8_t by, uint8_t why, const char* reason) {
        const sync::Lease was = leases_.Of(e);
        sync::HandOffMsg m; m.entries = {leases_.Write(e, holder, by, why)};
        SendLease(m, kNoSlot);
        OnLeaseChanged(e, was, reason);
    }
    void SendLease(const sync::HandOffMsg& m, uint8_t exceptSlot) {
        if (!net_) return;
        if (!IsHost()) { if (hostPeer_) net_->Send(hostPeer_, kChanReliable, Encode(m), true); return; }
        const std::vector<uint8_t> b = Encode(m);
        for (const auto& [peer, p] : peers_) if (p.slot != exceptSlot) net_->Send(peer, kChanReliable, b, true);
    }
    // Host: a client's lease entries, checked (sync::LeaseWriteOk; no chains), applied and relayed. A refused one: the
    // writer gets the table as it is.
    void HostLeaseIn(uint8_t slot, const sync::HandOffMsg& m) {
        sync::HandOffMsg ok;
        bool refused = false;
        for (const sync::LeaseEntry& in : m.entries) {
            const sync::Lease cur = leases_.Of(in.entity);
            bool good = sync::LeaseWriteOk(slot, in, cur) && FighterLevel();
            if (good && in.lease.Active())
                good = cfg_.handOff && (in.lease.holder == kHostSlot || PeerOfSlot(in.lease.holder)) && !leases_.Active(Roster::EntityForSlot(in.lease.holder)) &&
                       !leases_.HoldsAny(slot);
            if (!good) { refused = true; continue; }
            sync::Lease was;
            if (leases_.Apply(in, &was)) { ok.entries.push_back(in); OnLeaseChanged(in.entity, was, "written by a client"); }
        }
        if (!ok.entries.empty()) SendLease(ok, slot);
        if (refused) {
            if (++leasesRefused_ <= 20 || leasesRefused_ % 100 == 0) HSM_LOG("host: a hand-off lease from slot %u refused; it gets the table", slot);
            if (std::optional<uint32_t> to = PeerOfSlot(slot)) net_->Send(*to, kChanReliable, Encode(leases_.Snapshot()), true);
        }
    }
    // Client: lease entries relayed by the host (newer only), or its whole table (as it is).
    void ClientLeaseIn(const sync::HandOffMsg& m) {
        if (!m.full) {
            for (const sync::LeaseEntry& in : m.entries) { sync::Lease was; if (leases_.Apply(in, &was)) OnLeaseChanged(in.entity, was, "relayed"); }
            return;
        }
        std::array<bool, kMaxFighters + 1> listed{};
        for (const sync::LeaseEntry& in : m.entries) { listed[in.entity] = true; sync::Lease was; if (leases_.Set(in, &was)) OnLeaseChanged(in.entity, was, "the host's table"); }
        for (uint8_t e = 1; e <= kMaxFighters; ++e) {
            if (listed[e]) continue;
            sync::LeaseEntry none; none.entity = e;
            sync::Lease was;
            if (leases_.Set(none, &was)) OnLeaseChanged(e, was, "the host's table");
        }
    }
    // A lease changed here: take the fighter, give it back, or follow its stream from another machine. My own fighter
    // switches in OwnHandOffTick.
    void OnLeaseChanged(uint8_t e, const sync::Lease& was, const char* reason) {
        const sync::Lease& l = leases_.Of(e);
        const double now = game::SteadyNowMs();
        if (!was.Active() && l.Active()) leaseSince_[e] = now;
        if (was.Active() && !l.Active()) reAskAt_[e] = now + kReAskMs;
        if (HeldHere(e)) { if (!taken_[e]) TakeFighter(e); }
        else if (taken_[e]) GiveBackFighter(e);
        else if (e != MyEntity()) standIns_.ResetFeed(e);   // its stream comes from another machine now: another clock
        if (l.Active())
            HSM_LOG("%s: entity %u held by slot %u (%s with entity %u, epoch %u; %s)", IsHost() ? "host" : "client", e, l.holder, sync::CoupleName(l.why),
                    l.by, l.epoch, reason);
        else if (was.Active())
            HSM_LOG("%s: entity %u back with its owner after %.2f s (epoch %u; %s)", IsHost() ? "host" : "client", e, (now - leaseSince_[e]) / 1000.0,
                    l.epoch, reason);
    }
    // Held here: the stand-in becomes a live fighter (FighterRig::Revive), played from its player's input.
    void TakeFighter(uint8_t e) {
        UObject* w = HeldActor(e);
        if (!w) { HSM_LOG("hand-off: entity %u is held here but has no body here", e); return; }
        standIns_.Release(e);
        adapter_.EnsureBrain(w);
        if (IsHost()) fighters_[e - 1].input.DropDeltas(); else heldInput_[e] = InputApplier{};
        grabs_.Forget(e);
        taken_[e] = true;
        heldDeathSent_[e] = false;
        heldRx_[e] = heldDrives_[e] = 0;
    }
    // Given back: what its player held is let go here, and it is a stand-in of its owner's stream again.
    void GiveBackFighter(uint8_t e) {
        taken_[e] = false;
        UObject* w = HeldActor(e);
        InputApplier::Step s = HeldInput(e).ReleaseAll();
        if (w) for (Action a : s.released) adapter_.FireAction(w, a, false);
        standIns_.Detach(e);
        standIns_.ResetFeed(e);
        if (w) standIns_.Attach(e, w);
        // Its player's input while held here: frames that came (Input / RelayInput) against frames it was driven.
        HSM_LOG("%s: entity %u given back: %u input frame(s) came from its player, driven %u frame(s), %u quiet axis event(s) skipped, %zu button(s) let go",
                IsHost() ? "host" : "client", e, heldRx_[e], heldDrives_[e], HeldInput(e).skipped(), s.released.size());
    }
    // My own fighter while another machine holds it: a replica of the holder's stream, as under host physics, and what I
    // press is played there; back, it plays here again from what I hold now.
    void OwnHandOffTick(UObject* me, uint8_t mine) {
        if (!me || !mine) return;
        const char* who = IsHost() ? "host" : "client";
        const bool leased = MyFighterHanded();
        if (leased && (!ownLeased_ || standIns_.ActorOf(mine) != me)) {
            if (!ownLeased_) { severs_.LetGo(mine); grabs_.Forget(mine); }
            standIns_.Detach(mine);
            standIns_.ResetFeed(mine);   // the holder's stream of it, from its next pose
            standIns_.Attach(mine, me, true);
            if (!ownLeased_) HSM_LOG("%s: slot %u holds my fighter (hand-off, epoch %u): a replica of its stream until it gives it back", who,
                                     leases_.Holder(mine), leases_.Epoch(mine));
            ownLeased_ = true;
        } else if (!leased && ownLeased_) {
            ownLeased_ = false;
            const bool revived = standIns_.Release(mine);
            // What I pressed while it was held never reached my fighter: the difference is played now.
            const uint32_t held = capture_.buttons(), diff = held ^ nativeButtons_;
            int fired = 0;
            for (size_t i = 0; i < kActionCount; ++i) {
                const Action a = Action(i);
                if (!((diff >> i) & 1u) || a == Action::ToggleCameraLock || a == Action::MiddleMouse) continue;
                adapter_.FireAction(me, a, ((held >> i) & 1u) != 0);
                ++fired;
            }
            nativeButtons_ = held;
            HSM_LOG("%s: my fighter is mine again (hand-off over, epoch %u; %s, %d button(s) caught up)", who, leases_.Epoch(mine),
                    revived ? "revived" : "was not neutralised", fired);
        }
    }
    // Client holder: a held fighter died here; the host referees it (once per lease).
    void ReportHeldDeath(uint8_t e, UObject* v, double now) {
        if (IsHost() || heldDeathSent_[e] || !hostPeer_ || !adapter_.IsDead(v)) return;
        heldDeathSent_[e] = true;
        DeathReport r; r.victim = e; r.killer = LastAttacker(e, now); r.victimLife = LifeOf(e);
        net_->Send(hostPeer_, kChanReliable, Encode(r), true);
        HSM_LOG("client: entity %u died while held here; reported to the host (last blow from entity %u)", e, r.killer);
    }
    // Client: who dealt the last blow: a native one here (both bodies simulated here, a hand-off) within 5 s, else a claim.
    uint8_t LastAttacker(uint8_t v, double now) {
        if (v <= kMaxFighters && nativeHits_[v].first && now - nativeHits_[v].second < 5000.0) return nativeHits_[v].first;
        return claims_.RecentAttacker(v, now);
    }
    // Host: a slot left; a lease it held, or of its fighter, ends.
    void FreeSlotLeases(uint8_t slot) {
        for (uint8_t e = 1; e <= kMaxFighters; ++e)
            if (leases_.Active(e) && (leases_.Holder(e) == slot || sync::LeaseOwner(e) == slot)) WriteLease(e, kNoSlot, 0, 0, "a player left");
    }
    // Host: every hand-off ends (the bodies are going away); everyone starts the next level from a clear table.
    void EndLeases(const char* why) {
        for (uint8_t e = 1; e <= kMaxFighters; ++e) if (taken_[e]) GiveBackFighter(e);
        const bool any = leases_.Any();
        leases_.Clear();
        if (net_) net_->Broadcast(kChanReliable, Encode(leases_.Snapshot()), true);
        if (any) HSM_LOG("host: every hand-off ended (%s)", why);
    }
    void ResetLeases() {
        leases_.Clear();
        taken_ = {}; heldDeathSent_ = {}; askedAt_ = {}; asking_ = {}; leaseSince_ = {}; nativeHits_ = {}; reAskAt_ = {};
        heldRx_ = {}; heldDrives_ = {};
        for (InputApplier& in : heldInput_) in = InputApplier{};
    }
    // Client: a Pose or Vitals against the leases known here: another epoch is from before a switch. My own fighter's
    // comes in only while another machine holds it; a fighter held here streams from here.
    bool ClientStreamGate(uint8_t e, uint8_t epoch) {
        if (HostPhysics()) return true;
        if (epoch != leases_.Epoch(e)) { ++streamsDropped_; return false; }
        if (e == MyEntity()) return MyFighterHanded();
        return !HeldHere(e);
    }

    // My input. While another machine plays my fighter (host physics, a hand-off) movement and actions never reach my
    // replica (that machine's copy plays them); the look axes and the camera lock stay mine, so the camera answers at
    // once. Otherwise it is captured all the same (a hand-off starts from what is held) and plays here as usual;
    // nativeButtons_ keeps what my fighter itself last got, for the hand-back. Clients send it to the host always; the
    // host sends its own to a client holding its fighter (HostSendOwnInput).
    void BindClientInputHooks() {
        hooksBound_ = true;
        for (const auto& [fn, b] : adapter_.Bindings()) {
            game::InputBinding bind = b;
            HookFunction(fn, [this, bind](UObject* obj, UFunction* f, void* params) {
                if (!net_ || obj != myWillie_.load()) return false;
                if (replayingDelayed_) return false;   // the host's own delayed input, played again (HostInputDelayTick)
                const bool hostPlays = (HostPhysics() && !IsHost()) || MyFighterHanded();
                if (bind.isAxis) {
                    const Axis a = Axis(bind.index);
                    const float value = game::Adapter::ReadAxisParam(f, params);
                    capture_.OnAxis(a, value);
                    // Host input delay: not the look axes (they turn the host's own camera as well as aim, Willie_BP.cpp:11410-11560).
                    if (HostDelays() && !IsLookAxis(a)) { delayedIn_.PushAxis(NowMs(), uint8_t(bind.index), value); return true; }
                    return hostPlays && !(a == Axis::TurnMouse || a == Axis::LookMouse || a == Axis::TurnGamepad || a == Axis::LookGamepad);
                }
                capture_.OnAction(Action(bind.index), bind.pressed);
                if (HostDelays() && Action(bind.index) != Action::ToggleCameraLock && Action(bind.index) != Action::Talk) {
                    delayedIn_.PushAction(NowMs(), uint8_t(bind.index), bind.pressed);
                    return true;
                }
                if (!hostPlays) { const uint32_t bit = 1u << bind.index; nativeButtons_ = bind.pressed ? nativeButtons_ | bit : nativeButtons_ & ~bit; }
                return hostPlays && Action(bind.index) != Action::ToggleCameraLock;
            });
        }
        HSM_LOG("%s: input hooks on %zu function(s) (host physics and hand-offs)", IsHost() ? "host" : "client", adapter_.Bindings().size());
    }
    // Host physics, the host's own input (hsm_inputdelay.h): while a delay is set the hook above holds back the host
    // pawn's buttons and movement (not the look axes, the camera lock or the talk key) and this plays them again that
    // much later, through the same adapter calls DriveFighter uses. The delay follows the clients' median input lag.
    bool HostDelays() const { return IsHost() && HostPhysics() && hostDelay_.delayMs() > 0; }
    void ResetHostDelay() { delayedIn_.Clear(); hostDelay_.Suspend(); hostDelayLogged_ = false; delayPawn_ = nullptr; }
    void HostInputDelayTick(UObject* me) {
        if (!HostPhysics() || hostDelay_.Off() || !me || !net_) {
            if (me && !delayedIn_.Empty()) ReplayDelayed(me, delayedIn_.Take(NowMs(), 0));
            ResetHostDelay();
            return;
        }
        const uint32_t now = NowMs();
        if (me != delayPawn_) { delayedIn_.Clear(); delayPawn_ = me; }   // a new body: nothing held for the old one
        if (hostDelay_.Due(now)) {
            std::vector<uint32_t> rtts;   // the clients with a fighter here
            for (const auto& [peer, p] : peers_)
                if (p.slot >= 1 && p.slot < kMaxFighters && fighters_[p.slot].actor.obj) rtts.push_back(net_->RoundTripMs(peer));
            hostDelay_.Evaluate(now, rtts, cfg_.inputHz);
            if (hostDelay_.delayMs() > 0 && !hostDelayLogged_) {
                hostDelayLogged_ = true;
                HSM_LOG("host: input delay on: %d ms (%s, target %d ms, %d client(s)); my buttons and movement are played that much later, turning and aiming are not",
                        hostDelay_.delayMs(), hostDelay_.ModeName(), hostDelay_.targetMs(), hostDelay_.clients());
            }
        }
        const int delay = hostDelay_.delayMs();
        if (delay <= 0 && delayedIn_.Empty()) return;
        ReplayDelayed(me, delayedIn_.Take(now, delay));
        if (delay <= 0) delayedIn_.Clear();
    }
    void ReplayDelayed(UObject* me, const DelayedInput::Out& o) {
        replayingDelayed_ = true;   // the hook lets these through
        for (const auto& [act, pressed] : o.actions) {
            adapter_.FireAction(me, Action(act), pressed);
            const uint32_t bit = 1u << act;
            nativeButtons_ = pressed ? nativeButtons_ | bit : nativeButtons_ & ~bit;
        }
        for (size_t i = 0; i < kAxisCount; ++i) if (o.feed[i]) adapter_.ApplyAxis(me, Axis(i), o.axes[i]);
        replayingDelayed_ = false;
    }
    void SendInputs(float dt) {
        sendAccum_ += dt;
        if (!hostPeer_ || sendAccum_ < 1.f / float(cfg_.inputHz)) return;
        sendAccum_ = 0;
        net_->Send(hostPeer_, kChanUnreliable, Encode(TakeInputFrame()), false);
        ++inputsTx_;
    }
    // Host: while a client holds my fighter, what I press is played there, as a client's Input is on the host.
    void HostSendOwnInput(float dt) {
        sendAccum_ += dt;
        if (sendAccum_ < 1.f / float(cfg_.inputHz)) return;
        sendAccum_ = 0;
        RelayInput r; r.entity = HostEntity(); r.frame = TakeInputFrame();   // taken always: no deltas pile up meanwhile
        if (!MyFighterHanded() || !net_) return;
        if (std::optional<uint32_t> to = PeerOfSlot(leases_.Holder(r.entity))) { net_->Send(*to, kChanUnreliable, Encode(r), false); ++inputsTx_; }
    }
    InputFrame TakeInputFrame() {
        InputFrame f = capture_.Take(NowMs());
        float sens = 1.f; bool invert = false;   // my look settings, not the host's (sync audit item 16; DriveRemoteFighter)
        if (adapter_.MouseLook(sens, invert)) {
            float& x = f.axes[size_t(Axis::TurnMouse)];
            float& y = f.axes[size_t(Axis::LookMouse)];
            x = std::clamp(x * sens, -kAxisLimit, kAxisLimit);
            y = std::clamp(y * (invert ? -sens : sens), -kAxisLimit, kAxisLimit);
        }
        adapter_.ControlRotation(0, f.controlYaw, f.controlPitch);
        f.lockTarget = HostPhysics() && !IsHost() ? lockTarget_ : 0;
        return f;
    }

    // Host physics, client side: the game's lock-on (sync/hsm_lockon.h) for my replica, whose own tick, where the game
    // runs it, is blocked; the host's copy of my fighter has none (Adapter::KeepRemoteFighter). Guarding with the camera
    // lock on, the camera turns towards the fighter in front; the host gets my yaw as always, so my fighter turns with it
    // on every screen, and the frame names the target for the host's log.
    void ClientLockOn(UObject* me, float dt) {
        bool camera = false, fallen = false;
        if (cfg_.hostLockOn) {
            if (UObject* gi = adapter_.GameInstance()) GetBool(gi, "Camera Lock On", camera);
            GetBool(me, "Fallen", fallen);
        }
        const double* strength = Field<double>(me, "Lock on Strength");
        const uint32_t guards = (1u << size_t(Action::GuardLeft)) | (1u << size_t(Action::GuardRight));
        const bool active = cfg_.hostLockOn && lockon::Active(camera, (capture_.buttons() & guards) != 0, fallen, strength ? *strength : 0.0);
        float yaw = 0, pitch = 0;
        const bool haveRot = adapter_.ControlRotation(0, yaw, pitch);
        if (lock_.Due(active && haveRot, double(dt) * 1000.0)) {
            uint8_t found = 0;
            FVector body{}, eye{};
            if (adapter_.BoneLocation(me, "spine_05", body) && adapter_.CameraLocation(eye)) {
                std::vector<lockon::Candidate> cands;
                for (uint8_t e = 1; e <= kMaxWillies; ++e) {
                    UObject* w = e == world_.myEntity() ? nullptr : standIns_.ActorOf(e);
                    bool down = false;
                    FVector at{};
                    if (!w || adapter_.IsDead(w) || (GetBool(w, "Fallen", down) && down) || !adapter_.BoneLocation(w, "spine_05", at)) continue;   // a fallen fighter's beacon is off
                    cands.push_back({e, {at.x, at.y, at.z}});
                }
                found = lockon::Pick({body.x, body.y, body.z}, {eye.x, eye.y, eye.z}, lockon::Forward({yaw, pitch}), cands);
            }
            lock_.Found(found);
        }
        if (const uint8_t t = lock_.target()) {
            UObject* w = standIns_.ActorOf(t);
            FVector head{}, at{};
            if (!w || !adapter_.BoneLocation(me, "head", head) || !adapter_.BoneLocation(w, "spine_05", at)) lock_.Drop();
            else {
                const lockon::Rot r = lockon::Slew({yaw, pitch}, {head.x, head.y, head.z}, {at.x, at.y, at.z}, double(dt), strength ? *strength : 0.0);
                adapter_.SetControlRotation(0, float(r.yaw), float(r.pitch));
            }
        }
        if (lock_.target() != lockTarget_) {
            lockTarget_ = lock_.target();
            if (lockTarget_) HSM_LOG("client: lock-on on (target e%u)", lockTarget_); else HSM_LOG("client: lock-on off");
        }
    }

    // Host physics, client side: nothing is simulated here. My fighter is a replica like every other (its tick blocked,
    // moved onto the host's stream of it); what I press goes to the host.
    void ClientReplicaTick(UObject* me, float dt) {
        const double now = game::SteadyNowMs();
        const uint8_t mine = world_.myEntity();
        if (me && mine && !ownDeadPending_ && standIns_.ActorOf(mine) != me) {
            standIns_.Detach(mine);
            standIns_.Attach(mine, me);
            HSM_LOG("client: my fighter is a replica of the host's (host physics)");
        }
        if (me && !hooksBound_ && adapter_.BindInputs(me)) BindClientInputHooks();
        if (me) adapter_.TickLookSensitivity(me, dt);   // its tick is blocked: the look axes would stay at 0 (#137)
        if (me) ClientLockOn(me, dt); else { lock_.Drop(); lockTarget_ = 0; }
        SendInputs(dt);
        const std::vector<std::pair<uint8_t, UObject*>> none;
        Timed(kPumpServo, [&] { standIns_.Tick(adapter_, now, dt, {}); });
        Timed(kPumpClaims, [&] { claims_.Tick(now); });
        Timed(kPumpSevers, [&] { severs_.Tick(now, none); });
        Timed(kPumpGore, [&] { gore_.Tick(now); });
        Timed(kPumpGrabs, [&] { grabs_.Tick(now, dt, none); });
        chests_.Tick(now, StandInActors());
        Timed(kPumpProps, [&] { props_.Tick(now, none); });
        Timed(kPumpEquip, [&] { equip_.Tick(now, none); });
        fx_.Tick();
    }

    void HostTick(float dt) {
        SetMyWillie(inFight_ && localFighter_ ? adapter_.PlayerWillie(0) : nullptr);   // owner-authority checks read this, not the engine
        Timed(kPumpNet, [&] {
            NetEvent ev;
            while (net_->Poll(ev)) {
                if (ev.kind == NetEvent::Connected) { HSM_LOG("host: peer %u connected", ev.peer); helloWatch_.Connected(ev.peer, NowMs()); }
                else if (ev.kind == NetEvent::Disconnected) {
                    helloWatch_.Forget(ev.peer);
                    std::erase_if(pendingDrops_, [&](const auto& d) { return d.first == ev.peer; });
                    OnClientLeft(ev.peer);
                } else HostReceive(ev);
            }
        });
        if (peerMapDirty_) SendPeerMap();
        if (detourDirty_) SendRouteTables();
        // Steam peers have no transport timeout: a joined peer silent for 15 s is gone (N6).
        // Clients without the keep-alive are only timed in fight levels, while they have a live fighter (public #8, #9, #10).
        const auto streams = [this](uint32_t peer) {
            auto it = peers_.find(peer);
            return it != peers_.end() && (match_.phase() == MatchPhase::Idle || match_.Alive(Roster::EntityForSlot(it->second.slot)));
        };
        for (uint32_t peer : peerSilence_.Expired(NowMs(), FighterLevel(), streams)) {
            auto it = peers_.find(peer);
            if (it == peers_.end() || (it->second.key >> 63)) continue;
            HSM_LOG("host: steam peer %u silent for %u s; dropping slot %u", peer, PeerSilenceMonitor::kSilentMs / 1000, it->second.slot);
            net_->Disconnect(peer);
            OnClientLeft(peer);
        }
        // M5: a peer that connected and sent no Hello holds a slot for nothing.
        for (uint32_t peer : helloWatch_.Expired(NowMs())) {
            if (peers_.count(peer)) continue;
            HSM_LOG("host: peer %u sent no Hello within %u s of connecting; dropping it", peer, HelloWatch::kHelloMs / 1000);
            net_->Disconnect(peer);
        }
        for (uint8_t slot : reservations_.Expire(NowMs())) {
            bool back = std::any_of(peers_.begin(), peers_.end(), [&](const auto& kv) { return kv.second.slot == slot; });
            if (const RosterMember* m = roster_.BySlot(slot); m && !back) { HSM_LOG("host: slot %u did not rejoin in time", slot); RemoveMember(slot, m->key); }
        }
        ModerationTick();
        if (IsServer()) { ServerFlowTick(); ServerAnchorTick(); }
        if (director_.fight()) SpawnDue();
        Timed(kPumpMatch, [&] { HostMatchTick(); });
        OwnerHostTick(dt);
    }

    // Owner sync, host side: stream its own fighter and its bots, drive every client's stand-in, keep the line warm.
    // Severs, stuck blades and gore (phase 4a) ride on claims and vitals; grabs and dropped weapons (phase 4b) ride on
    // GrabState and ObjectSync's leases; level props on the host's census and the same leases.
    void OwnerHostTick(float dt) {
        const double now = game::SteadyNowMs();
        std::vector<std::pair<uint8_t, UObject*>> owned;
        UObject* me = MyWillieLive();
        OwnHandOffTick(me, HostEntity());
        if (me && ownLeased_) adapter_.TickLookSensitivity(me, dt);   // a replica while a client holds it: its tick is blocked
        if (me && !hooksBound_ && adapter_.BindInputs(me)) BindClientInputHooks();
        HostInputDelayTick(me);
        HostSendOwnInput(dt);
        if (me && !ownLeased_) owned.push_back({HostEntity(), me});
        for (auto& [e, ref] : bots_) if (adapter_.StillValid(ref.obj, ref.index)) owned.push_back({e, ref.obj});
        Timed(kPumpHandOff, [&] { HandOffTick(now); });
        // Host physics: every client's fighter is simulated here, from its input, and streamed like the host's own; a
        // hand-off: those held here, for now.
        if (HostPhysics() || leases_.Any()) {
            Timed(kPumpDrive, [&] {
                for (uint8_t slot = 1; slot < kMaxFighters; ++slot)
                    if (UObject* w = HostDrives(Roster::EntityForSlot(slot)) ? FighterActor(slot) : nullptr) {
                        DriveFighter(fighters_[slot].input, w, dt);
                        owned.push_back({Roster::EntityForSlot(slot), w});
                        if (!HostPhysics()) ++heldDrives_[Roster::EntityForSlot(slot)];
                    }
            });
            if (owned.size() > 1 && now >= nextListenerMs_) {   // a client's fighter took my audio listener (item 17)
                nextListenerMs_ = now + 500.0;
                if (UObject* me = MyWillieLive()) adapter_.KeepAudioListener(me);
            }
        }
        Timed(kPumpStreams, [&] {
            streams_.Tick(adapter_, now, dt, owned, [this](const std::vector<uint8_t>& b) { net_->Broadcast(kChanUnreliable, b, false); });
        });
        Timed(kPumpServo, [&] {
            standIns_.Tick(adapter_, now, dt, OwnedPelvises(owned));
            RecoverLostStandIns(now);
        });
        Timed(kPumpClaims, [&] { claims_.Tick(now); });
        Timed(kPumpSevers, [&] { severs_.Tick(now, owned); });
        Timed(kPumpGore, [&] { gore_.Tick(now); });
        Timed(kPumpGrabs, [&] { grabs_.Tick(now, dt, owned); });
        chests_.Tick(now, StandInActors());
        Timed(kPumpProps, [&] { props_.Tick(now, owned); });
        Timed(kPumpEquip, [&] { equip_.Tick(now, owned); });
        fx_.Tick();
        if (!peers_.empty() && NowMs() >= nextKeepAlive_) {
            nextKeepAlive_ = NowMs() + kKeepAliveMs;
            net_->Broadcast(kChanUnreliable, EncodePing(NowMs(), false), false);
        }
    }

    // Owner sync, host: a client's stand-in the game destroyed while the client is still live is spawned again where it
    // was announced (at most every RecreateLimiter::kRetryMs); the servo then snaps it onto the client's stream.
    void RecoverLostStandIns(double now) {
        if (!FighterLevel()) return;
        for (uint8_t slot = 1; slot < kMaxFighters; ++slot) if (fighters_[slot].actor.obj) FighterActor(slot);   // records losses
        for (auto it = lostSlots_.begin(); it != lostSlots_.end();) {
            const uint8_t slot = *it, e = Roster::EntityForSlot(slot);
            const Spawn* sp = nullptr;
            std::vector<Spawn> live = director_.LiveSpawns();
            for (const Spawn& s : live) if (s.entity == e) sp = &s;
            if (!roster_.BySlot(slot) || !sp || fighters_[slot].actor.obj) { it = lostSlots_.erase(it); continue; }
            if (!recreate_.Allow(e, now)) { ++it; continue; }
            game::Adapter::SpawnOptions o; o.blob = sp->blobId ? hostBlobs_.Get(sp->blobId) : nullptr;
            o.at = {sp->x, sp->y, sp->z}; o.yawDeg = sp->yaw; o.teamInt = TeamIntFor(sp->team, e); o.brain = HostDrives(e);
            UObject* w = SpawnFighterNow(o);
            if (!w) { HSM_LOG("host: re-creating entity %u's stand-in failed; retrying", e); ++it; continue; }
            fighters_[slot].actor.Set(w);
            standIns_.Detach(e);
            if (!HostDrives(e)) standIns_.Attach(e, w);
            QueueTint(w, sp->team);
            HSM_LOG("host: entity %u's stand-in was destroyed by the game; re-created", e);
            it = lostSlots_.erase(it);
        }
    }

    // Where the fighters simulated here stand: a stand-in is never snapped onto one of them.
    std::vector<sync::V3> OwnedPelvises(const std::vector<std::pair<uint8_t, UObject*>>& owned) const {
        std::vector<sync::V3> out;
        for (const auto& [e, w] : owned) if (sync::V3 p; streams_.PelvisOf(e, p)) out.push_back(p);
        return out;
    }

    std::vector<uint8_t> ConnectedSlots() const {
        std::vector<uint8_t> out{kHostSlot};
        for (const auto& [peer, p] : peers_) out.push_back(p.slot);
        return out;
    }
    std::optional<uint32_t> PeerOfSlot(uint8_t slot) const {
        for (const auto& [peer, p] : peers_) if (p.slot == slot) return peer;
        return std::nullopt;
    }

    // A Pose or Vitals from a client: kept only for an entity that client owns, applied to its stand-in here and relayed
    // as-is to every other client.
    void HostOwnerStream(const NetEvent& ev, const Peer& peer, MsgType t) {
        const uint8_t* d = ev.data.data(); size_t n = ev.data.size();
        if (t == MsgType::Pose) {
            PoseMsg m; sync::PoseFrame frame;
            // A hand-off: a stream of another epoch than the lease's for that fighter is a straggler from before a switch.
            if (!Decode(d, n, m, &frame) || !MayStream(peer.slot, m.entity, leases_.holders()) || m.epoch != leases_.Epoch(m.entity)) { ++streamsDropped_; return; }
            standIns_.OnPose(m, frame, ArrivalMs(ev));
        } else {
            VitalsMsg v;
            if (!Decode(d, n, v) || !MayStream(peer.slot, v.entity, leases_.holders()) || v.epoch != leases_.Epoch(v.entity)) { ++streamsDropped_; return; }
            standIns_.OnVitals(v);
            gore_.OnVitals(v.entity, v.severed, v.dislocated, game::SteadyNowMs());
            severs_.OnVitals(v, game::SteadyNowMs());
            NoteVitals(v);
            VoiceVitals(v);
        }
        // A client that hears this sender directly (its MeshHeard mask) is skipped; with mesh off every mask is 0 and all get it.
        const std::vector<uint8_t> all = ConnectedSlots();
        const std::vector<uint8_t> to = RelayTargets(peer.slot, all, meshMasks_);
        meshRelaySkipped_ += uint32_t(RelayTargets(peer.slot, all).size() - to.size());
        for (uint8_t slot : to)
            if (std::optional<uint32_t> p = PeerOfSlot(slot)) net_->Send(*p, kChanUnreliable, ev.data, false);
    }

    // ------------------------------------------------------------------ mesh (host side)
    // Offered only on a Steam lobby with owner sync.
    bool MeshHost() const { return cfg_.mesh && steamNet_; }
    // The PeerMap goes out for the mesh and for per-player pings (peer_ping=1, on with the mesh off).
    bool PeerMapHost() const { return steamNet_ && (cfg_.mesh || cfg_.peerPing); }

    // A client joined, left or was dropped: its own report is void, nobody hears it any more, and the clients get a new map.
    void MeshRosterChanged(uint8_t slot) {
        if (!PeerMapHost()) return;
        meshMasks_.Reset(slot);
        pingTables_.erase(slot);
        routeLinks_.Forget(slot); routePlanner_.Forget(slot);
        pairKeys_.Forget(slot);   // a returning player gets new keys
        peerMapDirty_ = true;
        if (DetourHost()) detourDirty_ = true;
    }

    // Every joined Steam client's slot and SteamID, to every joined client (each skips its own entry).
    void SendPeerMap() {
        peerMapDirty_ = false;
        if (!PeerMapHost()) return;
        std::vector<PeerMapEntry> all;
        for (const auto& [peer, p] : peers_) if (!(p.key >> 63)) all.push_back({p.slot, p.key});
        const PeerMapMsg m = MakePeerMap(all);
        const std::vector<uint8_t> msg = Encode(m);
        for (const auto& [peer, p] : peers_) net_->Send(peer, kChanReliable, msg, true);
        HSM_LOG("host: mesh: peer map of %zu sent to %zu clients", m.peers.size(), peers_.size());
    }

    // The life of a client-owned fighter as its owner's vitals tell it: the host's fallback when a DeathReport is missing.
    void NoteVitals(const VitalsMsg& v) {
        OwnerLife& l = lives_[v.entity];
        if (!(v.flags & 1)) { l.aliveSeen = true; l.deadSince = 0; }
        else if (l.aliveSeen && !l.deadSince) l.deadSince = std::max(NowMs(), 1u);
    }

    // Body lives (rev 19): the Spawn.life of an entity's current body as this machine knows it (0 = unknown).
    uint8_t LifeOf(uint8_t e) const { return IsHost() ? lifeBook_.Current(e) : (e <= kMaxWillies ? clientLife_[e] : uint8_t(0)); }
    // A claim or report that names an older body of the victim: dropped (logged for the first few, counted).
    bool StaleLife(uint8_t victim, uint8_t claimed) {
        if (LifeCurrent(claimed, LifeOf(victim))) return false;
        if (++staleLife_ <= 3) HSM_LOG("session: dropped a claim against an old body of entity %u (life %u, now %u)", victim, claimed, LifeOf(victim));
        return true;
    }

    // A claim made on the host (senderSlot 0) or received from a client: checked against who owns what and the
    // friendly-fire rule, credited for the kill log, then replayed by the victim's owner and shown to everyone else.
    void RouteHostClaim(uint8_t senderSlot, const combat::HitClaim& c) {
        if (StaleLife(c.victim, c.victimLife)) return;
        const ClaimRoute r = RouteClaim(senderSlot, c.attacker, c.victim, ConnectedSlots(), leases_.holders());
        if (!r.accept || !HitAllowed(true, KnownTeam(c.attacker), KnownTeam(c.victim), match_.config().friendlyFire)) { ++claimsRejected_; return; }
        kills_.OnHit(c.victim, c.attacker, NowMs());
        draw_.OnHit(c.attacker, NowMs());
        if (r.replaySlot == kHostSlot) claims_.Replay(c, game::SteadyNowMs());
        else if (std::optional<uint32_t> to = PeerOfSlot(r.replaySlot)) net_->Send(*to, kChanReliable, Encode(HitClaimMsg{c}), true);
        if (!r.fxSlots.empty()) {
            const std::vector<uint8_t> fx = Encode(HitFxMsg{c});
            for (uint8_t slot : r.fxSlots) if (std::optional<uint32_t> to = PeerOfSlot(slot)) net_->Send(*to, kChanReliable, fx, true);
        }
        if (r.hostFx) claims_.Effect(c);
    }

    // A sever made on the host or received from a client, routed as its blow is: the victim's owner replays it on its
    // own fighter; every other machine keeps it until that owner's vitals confirm the part (the attacker kept its own).
    void RouteHostSever(uint8_t senderSlot, const combat::SeverClaim& c) {
        if (StaleLife(c.victim, c.victimLife)) return;
        const ClaimRoute r = RouteClaim(senderSlot, c.attacker, c.victim, ConnectedSlots(), leases_.holders());
        if (!r.accept || !HitAllowed(true, KnownTeam(c.attacker), KnownTeam(c.victim), match_.config().friendlyFire)) { ++claimsRejected_; return; }
        const double now = game::SteadyNowMs();
        const std::vector<uint8_t> msg = Encode(combat::SeverClaimMsg{c});
        if (r.replaySlot == kHostSlot) severs_.Replay(c, now);
        else if (std::optional<uint32_t> to = PeerOfSlot(r.replaySlot)) net_->Send(*to, kChanReliable, msg, true);
        for (uint8_t slot : r.fxSlots) if (std::optional<uint32_t> to = PeerOfSlot(slot)) net_->Send(*to, kChanReliable, msg, true);
        if (r.hostFx) severs_.Remember(c, now);
    }

    // A sever the owner's fighter took natively (combat::SeverNoticeMsg; the sender owns that fighter): every other
    // machine keeps it as it keeps a claim, until the owner's vitals confirm the part. Nobody replays it.
    void RouteHostNotice(uint8_t senderSlot, const combat::SeverClaim& c) {
        const std::vector<uint8_t> msg = Encode(combat::SeverNoticeMsg{c});
        for (uint8_t slot : RelayTargets(senderSlot, ConnectedSlots()))
            if (std::optional<uint32_t> to = PeerOfSlot(slot)) net_->Send(*to, kChanReliable, msg, true);
        if (senderSlot != kHostSlot) severs_.Remember(c, game::SteadyNowMs());
    }

    // A blade stuck in / freed from a stand-in: only the victim's owner holds it. A release always passes, so a blade
    // stuck before the friendly-fire rule changed is still let go.
    void RouteHostStuck(uint8_t senderSlot, const combat::StuckBladeMsg& m) {
        const ClaimRoute r = RouteClaim(senderSlot, m.attacker, m.victim, ConnectedSlots(), leases_.holders());
        if (!r.accept || (m.stuck && !HitAllowed(true, KnownTeam(m.attacker), KnownTeam(m.victim), match_.config().friendlyFire))) { ++claimsRejected_; return; }
        if (r.replaySlot == kHostSlot) severs_.OnStuck(m, game::SteadyNowMs());
        else if (std::optional<uint32_t> to = PeerOfSlot(r.replaySlot)) net_->Send(*to, kChanReliable, Encode(m), true);
    }

    // Sync audit T3 (hsm_remote.h): a write the game made on a stand-in here, for the fighter's owner. Under host physics
    // nothing is a stand-in of a simulated fighter, so nothing is sent.
    void SendRemote(const sync::RemoteWriteMsg& m) {
        if (HostPhysics()) return;
        if (IsHost()) RouteHostRemote(kHostSlot, m);
        else if (net_ && hostPeer_) net_->Send(hostPeer_, kChanReliable, Encode(m), true);
    }
    // The sender owns `from` (checked on receipt); the write goes to the target's owner alone, never back to the sender.
    void RouteHostRemote(uint8_t senderSlot, const sync::RemoteWriteMsg& m) {
        const uint8_t owner = OwnerSlotOf(m.target, leases_.holders());
        if (owner == 0xFF || owner == senderSlot) { ++remoteDropped_; return; }
        if (owner == kHostSlot) ApplyRemote(m);
        else if (std::optional<uint32_t> to = PeerOfSlot(owner); to && net_) net_->Send(*to, kChanReliable, Encode(m), true);
    }
    // The owner makes the write on its own fighter, after its own checks: a push as it came (Decode capped it), a revive
    // only from a teammate (Team Int, as Get_Up_Pal tests it, here) and once a second, a let-go only of that hold.
    void ApplyRemote(const sync::RemoteWriteMsg& m) {
        UObject* me = OwnedActorOf(m.target);
        if (!me) { ++remoteDropped_; return; }
        bool done = false;
        switch (m.kind) {
            case sync::RemoteKind::Push:
                done = adapter_.Push(me, sync::kPoseBoneNames[m.bone], FVector{ m.vec.x, m.vec.y, m.vec.z });
                break;
            case sync::RemoteKind::Revive: {
                UObject* pal = standIns_.ActorOf(m.from);
                const int32_t* theirs = pal ? Field<int32_t>(pal, "Team Int") : nullptr;
                const int32_t* mine = Field<int32_t>(me, "Team Int");
                bool downed = false;
                GetBool(me, "Downed", downed);
                if (!theirs || !mine || *theirs != *mine || *mine == 0 || downed || !remoteGate_.Revive(m.target, game::SteadyNowMs())) break;
                adapter_.Revive(me);
                done = true;
                HSM_LOG("session: e%u helped up by e%u (Get Up, Pal on its machine)", m.target, m.from);
                break;
            }
            case sync::RemoteKind::GrabBreak:
                grabs_.OnBreak(me, m.from, m.bone);
                done = true;
                break;
        }
        if (done) ++remoteApplied_;
        else ++remoteDropped_;
    }

    UObject* FighterActor(uint8_t slot) {
        if (slot == 0) return InFightLevel() && localFighter_ ? adapter_.PlayerWillie(0) : nullptr;
        ActorRef& a = fighters_[slot].actor;
        if (a.obj && !adapter_.StillValid(a.obj, a.index)) {
            HSM_LOG("host: fighter in slot %u was destroyed by the game", slot);
            a.Clear();
            lostSlots_.insert(slot);   // its stand-in is spawned again (RecoverLostStandIns)
        }
        return a.obj;
    }

    // The game's lock-on and "pal" checks compare Team Int (equal and non-zero = allies). Allies get the local
    // player's 1; enemies a distinct non-zero value per team, or per fighter when there are no teams.
    int TeamIntFor(int8_t team, uint8_t entity) {
        int8_t mine = kNoTeam;
        if (IsHost()) mine = TeamOfSlot(0);
        else if (const Spawn* own = world_.Find(world_.myEntity())) mine = own->team;
        if (team != kNoTeam && team == mine) return 1;
        return team != kNoTeam ? 10 + team : 2 + entity;
    }

    // Free Mode keeps respawning its foes onto the spawn points our fighters use; once the level's spawn has run
    // (our fighter exists) the foe count goes to 0 for the rest of the fight. The player's own value comes back on
    // the next non-fight level or when the session ends.
    void RememberFoes() { if (savedFoes_ < 0) savedFoes_ = adapter_.FreeModeFoes(); }
    void StopStockFoes() {
        if (foesStopped_ || !FighterLevel() || IsBotFormatHere()) return;
        foesStopped_ = true;
        adapter_.SetFreeModeFoes(0);
        HSM_LOG("session: stock Free Mode foes stopped for this fight");
    }
    void RestoreFoes() { if (savedFoes_ >= 0) { adapter_.SetFreeModeFoes(savedFoes_); savedFoes_ = -1; } }
    bool IsBotFormatHere() const { return IsHost() ? IsBotFormat(match_.config().format) || adoptStock_ : IsBotFormat(Format(info_.format)); }

    // The point spawns are placed around: the host's own fighter, or on role=server the stock pawn's place captured before
    // the pawn was stripped (ServerAnchorTick). False while there is none yet.
    bool HostAnchor(FVector& pos, AnchorKind& kind) {
        if (!IsServer()) {
            UObject* host = adapter_.PlayerWillie(0);
            if (!host) return false;
            pos = adapter_.ActorLocation(host); kind = AnchorKind::StockPawn;
            return true;
        }
        if (!serverAnchor_) return false;
        pos = *serverAnchor_; kind = serverAnchorKind_;
        return true;
    }

    // role=server: capture the stock pawn's place the first tick it exists in a fighter level. Without one, from 3 s after the
    // level loaded the sources of hsm_spawn_anchor.h decide (spawners, PlayerStart, navmesh, a traced floor; the origin after
    // 20 s), a pass a second; the source used is logged once. Then the pawn is stripped so no fighter stands in the arena.
    void ServerAnchorTick() {
        if (!IsServer() || !FighterLevel()) return;
        const uint32_t now = NowMs();
        UObject* pawn = adapter_.PlayerWillie(0);
        if (pawn && !serverPawnSeenAt_) serverPawnSeenAt_ = now ? now : 1;   // also one that turns up after another source anchored
        if (!serverAnchor_ && pawn) {
            const FVector v = adapter_.ActorLocation(pawn);
            serverAnchor_ = v; serverAnchorKind_ = AnchorKind::StockPawn;
            HSM_LOG("server: spawn anchor %.0f %.0f %.0f from the stock pawn in %s", v.x, v.y, v.z, currentLevel_.c_str());
        }
        if (!serverAnchor_ && anchorSearch_.Due(now)) {
            anchorSearch_.Passed(now);
            const std::vector<UObject*>& ignore = SpawnIgnore();
            AnchorSources src;
            src.spawners = [&] { return SpawnerPoints(ignore); };
            src.playerStarts = [&] {
                std::vector<Point3> out;
                for (const FVector& v : adapter_.PlayerStarts()) out.push_back({v.x, v.y, v.z});
                return out;
            };
            src.navmesh = [&]() -> std::optional<Point3> {
                FVector v;
                if (adapter_.NavigablePoint(v)) return Point3{v.x, v.y, v.z};
                return std::nullopt;
            };
            src.floor = [&]() -> std::optional<Point3> {
                FVector v;
                if (adapter_.StaticMeshFloor(ignore, v)) return Point3{v.x, v.y, v.z};
                return std::nullopt;
            };
            const SpawnAnchor a = SearchAnchor(src, anchorSearch_.GiveUp(now));
            if (a.kind != AnchorKind::None) {
                static const char* const kSource[] = {"the stock pawn", "a level spawner", "a PlayerStart", "a navmesh point", "a traced floor",
                                                      "the level origin (nothing else answered in time)"};
                serverAnchor_ = FVector{a.pos.x, a.pos.y, a.pos.z}; serverAnchorKind_ = a.kind;
                HSM_LOG("server: spawn anchor %.0f %.0f %.0f from %s in %s (no stock pawn; pass %u)", a.pos.x, a.pos.y, a.pos.z,
                        kSource[size_t(a.kind)], currentLevel_.c_str(), anchorSearch_.passes());
                if (a.kind == AnchorKind::Origin) HSM_LOG("server: WARNING %s has no pawn, spawner, PlayerStart, navmesh or static mesh floor; fighters are placed around the origin", currentLevel_.c_str());
            } else if (anchorSearch_.passes() == 1) HSM_LOG("server: no stock pawn and no spawner, PlayerStart, navmesh point or floor yet in %s; trying again each second", currentLevel_.c_str());
        }
        // The stock pawn goes 3 s after it was seen (the level start-up cleanup is over), once per second, at most 5 times.
        if (pawn && serverPawnSeenAt_ && now - serverPawnSeenAt_ >= 3000 && now - serverStripAt_ >= 1000 && serverStripped_ < 5) {
            serverStripAt_ = now; ++serverStripped_;
            HSM_LOG("server: stripping the stock pawn (%d)", serverStripped_);
            adapter_.DestroyFighter(pawn, ObjectIndex(pawn));
            if (serverStripped_ == 5) HSM_LOG("server: WARNING the stock flow keeps respawning a pawn; leaving it alone now");
        }
    }

    // Spawn height: the stock spawner put the host's body at the right height above its floor; every spawn point
    // gets that same height above the floor under it (spawner actors sit at varying heights, so players popped out of
    // the ground).
    // The trace looks through every fighter and what it carries (test 5: a respawn landed on top of the fighter standing
    // at the spot); the stand height is measured once per level and never negative (SaneStandOffset; the Abyss read
    // -96 cm), so a spawn is never put below the floor it traced. Unchanged when a trace fails.
    FVector Grounded(FVector at, const std::vector<UObject*>& ignore) {
        if (!groundValid_) {
            FVector hp; AnchorKind ak = AnchorKind::StockPawn;
            if (!HostAnchor(hp, ak)) return at;
            if (ak != AnchorKind::StockPawn) {   // role=server without a stock pawn: a PlayerStart's height is not a stand height
                groundOffset_ = StandOffsetFor(ak, 0); groundValid_ = true;
                HSM_LOG("server: no stock pawn to measure in %s; fighters stand %.0f cm above the floor", currentLevel_.c_str(), groundOffset_);
                double fz2;
                if (adapter_.FloorZ(at, ignore, fz2)) at.z = fz2 + groundOffset_;
                return at;
            }
            double fz;
            std::string hit;
            if (!adapter_.FloorZ(hp, ignore, fz, &hit)) return at;
            const double raw = hp.z - fz;
            groundOffset_ = StandOffsetFor(AnchorKind::StockPawn, raw); groundValid_ = true;
            if (groundOffset_ != raw)
                HSM_LOG("host: the floor under the host traced %.0f cm below it in %s (hit %s); fighters stand %.0f cm above the floor",
                        raw, currentLevel_.c_str(), hit.c_str(), groundOffset_);
            else HSM_LOG("host: fighters stand %.0f cm above the floor in %s (hit %s)", groundOffset_, currentLevel_.c_str(), hit.c_str());
        }
        double fz;
        if (adapter_.FloorZ(at, ignore, fz)) at.z = fz + groundOffset_;
        return at;
    }

    // What a spawn's floor traces look through (Adapter::BodiesAndGear). That walks the object array, so the spawns of
    // one Tick (a round start, a wave) share one walk; never kept past it (garbage collection runs between frames and
    // the engine dereferences the list), and a spawn outside Tick (a lobby button) walks on its own.
    const std::vector<UObject*>& SpawnIgnore() {
        if (spawnIgnoreState_ == SpawnIgnoreState::Cached) return spawnIgnore_;
        spawnIgnore_ = adapter_.BodiesAndGear();
        if (spawnIgnoreState_ == SpawnIgnoreState::InTick) spawnIgnoreState_ = SpawnIgnoreState::Cached;
        return spawnIgnore_;
    }
    // Every spawn goes through these: a body spawned this Tick is not in a list walked before it (Yard 2026-10-25: a
    // round start's respawn traced the floor through a list walked before the stand-in it replaced was spawned, hit that
    // stand-in's head and put the new one 157 cm up), so the next trace walks again.
    void SpawnedThisTick() { if (spawnIgnoreState_ == SpawnIgnoreState::Cached) spawnIgnoreState_ = SpawnIgnoreState::InTick; }
    UObject* SpawnFighterNow(const game::Adapter::SpawnOptions& o) { UObject* w = adapter_.SpawnFighter(o); SpawnedThisTick(); return w; }
    UObject* RespawnLocalNow(const game::Adapter::SpawnOptions& o) { UObject* w = adapter_.RespawnLocalPlayer(o); SpawnedThisTick(); return w; }

    // The floor a spawn traces under p (FloorZ); nullopt for no floor, or a trap (the Pit's spikes).
    std::optional<double> SpawnFloor(const Point3& p, const std::vector<UObject*>& ignore) {
        double z;
        std::string hit;
        if (!adapter_.FloorZ({p.x, p.y, p.z}, ignore, z, &hit)) return std::nullopt;
        if (hit.find("Trap") != std::string::npos) return std::nullopt;
        return z;
    }

    // StandableSpot's engine checks of slot p around its centre c: a floor under the whole fighter within kSpotStepCm of
    // c's own (FootprintFits), and nothing in the way from c to p kWallProbeCm above c's floor (the Pit's fence).
    bool Stands(const Point3& c, const Point3& p, const std::vector<UObject*>& ignore) {
        const std::optional<double> cf = SpawnFloor(c, ignore);
        if (!FootprintFits(cf, p, [&](const Point3& q) { return SpawnFloor(q, ignore); })) return false;
        const double z = (cf ? *cf : c.z) + kWallProbeCm;
        return adapter_.LineClear({c.x, c.y, z}, {p.x, p.y, z}, ignore);
    }

    // The spot for `self` around c, from spread slot `first` on: clear of the other bodies, inside `area` and standable.
    Point3 StandableNear(const Point3& c, uint8_t self, size_t first, const SpawnArea& area, const std::vector<UObject*>& ignore) {
        size_t skipped = 0;
        const Point3 p = StandableSpot(c, BodiesExcept(self), first, kMinFighterGapCm, area,
                                       [&](const Point3& a, const Point3& b) { return Stands(a, b, ignore); }, &skipped);
        if (skipped)
            HSM_LOG("host: spawn of #%u near (%.0f, %.0f): %zu slot(s) passed over (outside the arena, no floor under a fighter or a wall "
                    "in the way); at (%.0f, %.0f)", self, c.x, c.y, skipped, p.x, p.y);
        return p;
    }

    // The level's spawners a fighter can stand at (one with no floor under it is dropped), logged once per level.
    std::vector<Point3> SpawnerPoints(const std::vector<UObject*>& ignore) {
        std::vector<Point3> out;
        size_t noFloor = 0;
        for (const FVector& v : adapter_.SpawnCandidates()) {
            const Point3 p{v.x, v.y, v.z};
            if (SpawnFloor(p, ignore)) out.push_back(p); else ++noFloor;
        }
        if (spawnersLogged_ != currentLevel_) {
            spawnersLogged_ = currentLevel_;
            HSM_LOG("host: %zu spawn candidates in %s (%zu without a floor dropped; the level destroys the ones not made for this mode)",
                    out.size(), currentLevel_.c_str(), noFloor);
        }
        return out;
    }

    // Where every body of the session lies or stands now (corpses too), but `self`'s: a (re)spawn keeps clear of them.
    std::vector<Point3> BodiesExcept(uint8_t self) {
        std::vector<Point3> out;
        auto add = [&](uint8_t e) {
            if (e == self) return;
            if (UObject* w = EntityActor(e)) { FVector v = adapter_.ActorLocation(w); out.push_back({v.x, v.y, v.z}); }
        };
        for (const RosterMember& m : roster_.members()) add(m.entity);
        for (auto& [e, ref] : bots_) add(e);
        return out;
    }

    // Spawner-less maps (the Abyss): points near a ring around the host on the navmesh that the host can walk to
    // (NavigableRing over ReachablePointNear); without two of those, the ring's points where a fighter can stand
    // (StandableRing). Either way a point is kept only with nothing in the way from the host to it kWallProbeCm above
    // the floor (test 2026-10-25: the Yard's 900 cm ring put fighters outside its fence, on navmesh past the fence).
    std::vector<Point3> RingCandidates(double radius, const std::vector<UObject*>& ignore) {
        FVector c; AnchorKind ak;
        double hostFloor;
        if (!HostAnchor(c, ak) || !adapter_.FloorZ(c, ignore, hostFloor)) return {};
        auto floorAt = [&](const Point3& p) -> std::optional<double> {
            double z;
            if (!adapter_.FloorZ({p.x, p.y, p.z}, ignore, z)) return std::nullopt;
            return z;
        };
        auto navAt = [&](const Point3& aim, double tolerance) -> std::optional<Point3> {
            FVector v;
            if (!adapter_.ReachablePointNear(c, {aim.x, aim.y, aim.z}, tolerance, v)) return std::nullopt;
            return Point3{v.x, v.y, v.z};
        };
        size_t walled = 0;
        auto inView = [&](std::vector<Point3> pts) {   // points are at their floor height
            std::vector<Point3> out;
            for (const Point3& p : pts)
                if (adapter_.LineClear({c.x, c.y, hostFloor + kWallProbeCm}, {p.x, p.y, p.z + kWallProbeCm}, ignore)) out.push_back(p);
                else ++walled;
            return out;
        };
        const std::vector<Point3> fighters = BodiesExcept(1);
        std::vector<Point3> nav = inView(NavigableRing({c.x, c.y, c.z}, hostFloor, navAt, floorAt, fighters, radius));
        if (nav.size() >= 2) {
            HSM_LOG("host: no spawners in %s; %zu navmesh points near a %.0f cm ring around the host (%zu behind a wall dropped)",
                    currentLevel_.c_str(), nav.size(), radius, walled);
            return nav;
        }
        std::vector<Point3> ring = inView(StandableRing({c.x, c.y, c.z}, hostFloor, floorAt, fighters, radius));
        HSM_LOG("host: no spawners in %s; %zu navmesh points, %zu of 8 points on a %.0f cm ring around the host can be stood on "
                "(%zu behind a wall dropped)", currentLevel_.c_str(), nav.size(), ring.size(), radius, walled);
        return ring;
    }

    // Spawn points: the host's own fighter is candidate 0, the level's Willie spawners (or, without any, a ring of
    // standable points) follow; farthest-point sampling gives slot k its own spot, SpreadPositions keeps every spot
    // kMinFighterGapCm from the others, and FreeSpot from the bodies standing there by now (a late join).
    FVector SpawnPoint(uint8_t slot, double& yawDeg) {
        const std::vector<UObject*>& ignore = SpawnIgnore();
        if (!picksValid_) {
            AnchorKind ak;
            if (!HostAnchor(spawnCenter_, ak)) spawnCenter_ = FVector{};
            std::vector<Point3> cands{{spawnCenter_.x, spawnCenter_.y, spawnCenter_.z}};
            for (const Point3& p : SpawnerPoints(ignore)) cands.push_back(p);
            if (cands.size() == 1) for (const Point3& p : RingCandidates(kNoSpawnerRingCm, ignore)) cands.push_back(p);
            spawnArea_ = AreaOf(cands);
            spawnSpots_ = StandableSpread(cands, kMaxFighters, spawnArea_, [&](const Point3& a, const Point3& b) { return Stands(a, b, ignore); });
            picksValid_ = true;
        }
        const Point3 c = StandableNear(spawnSpots_[slot], Roster::EntityForSlot(slot), 0, spawnArea_, ignore);
        FVector at{c.x, c.y, c.z};
        double dx = spawnCenter_.x - at.x, dy = spawnCenter_.y - at.y;
        yawDeg = (dx == 0 && dy == 0) ? 0 : std::atan2(dy, dx) * 57.29577951308232;   // face the host
        return Grounded(at, ignore);
    }

    void SpawnDue() {
        std::vector<uint8_t> due = director_.Due(NowMs());
        if (due.empty()) return;
        UObject* host = adapter_.PlayerWillie(0);
        if (localFighter_ && !host) return;             // the stock spawner has not placed the host yet
        if (!localFighter_) { FVector ap; AnchorKind ak; if (!HostAnchor(ap, ak)) return; }   // role=server: wait for the anchor
        for (uint8_t slot : due) {
            // K1/K2: a rejoiner the match counts down gets its body from the match's respawn (RespawnEntity), not at once:
            // it was invulnerable (hits on a fighter the match counts dead are ignored) for the rest of the round.
            if (slot != 0 && match_.AwaitsRespawn(Roster::EntityForSlot(slot))) continue;
            Spawn sp; sp.entity = Roster::EntityForSlot(slot); sp.ownerSlot = slot; sp.kind = EntityKind::Fighter;
            sp.team = TeamOfSlot(slot); sp.life = lifeBook_.Bump(sp.entity);
            FVector at; double yaw = 0;
            if (slot == 0) {
                at = adapter_.ActorLocation(host); yaw = adapter_.ActorYaw(host);
                sp.blobId = hostBlobs_.LatestFor(0);
                QueueTint(host, sp.team);
            } else {
                at = sp.team != kNoTeam ? SidePoint(sp.team, sp.entity, yaw) : SpawnPoint(slot, yaw);
                if (match_.phase() != MatchPhase::Idle && !match_.Has(sp.entity)) JoinerGear(slot);   // joins a running match
                uint32_t id = director_.BlobFor(slot);
                const std::vector<uint8_t>* blob = hostBlobs_.Get(id);
                sp.blobId = blob ? id : 0;
                game::Adapter::SpawnOptions o; o.blob = blob; o.at = at; o.yawDeg = yaw; o.teamInt = TeamIntFor(sp.team, sp.entity);
                o.brain = HostPhysics();                // host physics: played here from its input; else a stand-in, moved by its owner's stream
                UObject* w = SpawnFighterNow(o);
                if (!w) HSM_LOG("host: spawning slot %u failed; it stays empty until the next level", slot);
                fighters_[slot].actor.Set(w);
                if (w && !HostPhysics()) standIns_.Attach(sp.entity, w);
                if (w) QueueTint(w, sp.team);
            }
            if (match_.phase() != MatchPhase::Idle && !match_.Has(sp.entity)) { match_.AddFighter({sp.entity, sp.team, true, ScoreId(sp.entity)}); infoDirty_ = true; }
            sp.x = float(at.x); sp.y = float(at.y); sp.z = float(at.z); sp.yaw = float(yaw);
            director_.MarkSpawned(sp);
            net_->Broadcast(kChanReliable, Encode(sp), true);
            HSM_LOG("host: entity %u (slot %u) live, loadout blob %08x", sp.entity, slot, sp.blobId);
            if (slot == 0) StopStockFoes();
        }
    }

    void DespawnFighter(uint8_t slot) {
        Fighter& f = fighters_[slot];                   // a stand-in: unbound first, its weapons go with it
        f.input.ReleaseAll();                           // host physics: the next body starts with nothing held
        standIns_.Detach(Roster::EntityForSlot(slot));
        props_.ForgetEntity(Roster::EntityForSlot(slot));
        adapter_.DestroyFighter(f.actor.obj, f.actor.index);
        f.actor.Clear();
    }

    // The host's own passport, shared like any client's (slot 0).
    void ShareHostLoadout() {
        if (!localFighter_ || roster_.size() == 0) return;
        std::vector<uint8_t> bytes;
        if (!adapter_.ExportPassport(bytes) || bytes == hostExport_) return;
        hostExport_ = bytes;
        const std::vector<uint8_t> own = bytes;
        KeepOwnLoadout(0, own, StoreAndRelay(0, std::move(bytes), 0));
    }

    // Assigns a fresh host blob id for the slot, keeps it, and sends it to every peer except `skipPeer`; 0 when not stored.
    uint32_t StoreAndRelay(uint8_t slot, std::vector<uint8_t> bytes, uint32_t skipPeer) {
        uint32_t id = MakeBlobId(slot, ++blobVersion_[slot]);
        std::vector<BlobChunk> chunks = SplitBlob(id, bytes);
        if (chunks.empty() || !hostBlobs_.Put(id, std::move(bytes))) { HSM_LOG("host: loadout for slot %u not storable", slot); return 0; }
        director_.OnBlob(slot, id);
        if (!net_) return id;
        for (const auto& [peer, p] : peers_) {
            if (peer == skipPeer) continue;
            for (const BlobChunk& c : chunks) net_->Send(peer, kChanReliable, Encode(c), true);
        }
        return id;
    }

    // A player's own uploaded loadout and its blob id, kept apart from the slot's current blob: a generated-gear match
    // replaces the slot's blob, and the next Own-gear match has to find the upload again (#62: after a Random-tier match
    // the Own-gear match that followed respawned everyone in the generated gear). 0.8.0-pre7.
    void KeepOwnLoadout(uint8_t slot, const std::vector<uint8_t>& bytes, uint32_t id) {
        if (slot >= ownLoadout_.size() || !id) return;
        ownLoadout_[slot] = bytes; ownBlobId_[slot] = id;
    }
    void RestoreOwnLoadouts(const char* why) {
        int restored = 0;
        for (const RosterMember& m : roster_.members()) {
            const uint8_t slot = m.slot;
            if (slot >= ownLoadout_.size() || !ownBlobId_[slot] || ownLoadout_[slot].empty()) continue;
            const uint32_t cur = slot == 0 ? hostBlobs_.LatestFor(0) : director_.BlobFor(slot);
            if (cur == ownBlobId_[slot]) continue;
            const uint32_t id = StoreAndRelay(slot, ownLoadout_[slot], 0);
            if (!id) continue;
            ownBlobId_[slot] = id; ++restored;
        }
        if (restored) HSM_LOG("host: %s: %d player(s) back in their own loadout", why, restored);
    }

    void SendAllBlobs(uint32_t peer) {
        for (uint8_t slot = 0; slot < kMaxFighters; ++slot) {
            uint32_t id = hostBlobs_.LatestFor(slot);
            if (const std::vector<uint8_t>* b = id ? hostBlobs_.Get(id) : nullptr)
                for (const BlobChunk& c : SplitBlob(id, *b)) net_->Send(peer, kChanReliable, Encode(c), true);
        }
    }

    // M3: a message over its class's budget was dropped; the first few of a class (then every 2000th) are logged.
    void BudgetDropped(const MsgBudget& b, MsgType t, const char* who, unsigned slot) {
        const BudgetClass c = BudgetClassOf(t);
        ++streamsDropped_;
        if (MsgBudget::ShouldLog(b.Dropped(c)))
            HSM_LOG("%s: %s traffic from slot %u is over its budget; %llu %s messages dropped so far", who, MsgBudget::ClassName(c), slot,
                    static_cast<unsigned long long>(b.Dropped(c)), MsgBudget::ClassName(c));
    }

    void HostReceive(const NetEvent& ev) {
        const uint8_t* d = ev.data.data(); size_t n = ev.data.size();
        MsgType t;
        const bool typed = PeekType(d, n, t);
        peerSilence_.Seen(ev.peer, NowMs(), typed && t == MsgType::Ping);
        if (!typed) { if (IsForeignHello(d, n)) OnHello(ev); return; }
        if (t == MsgType::Hello) { OnHello(ev); return; }
        if (t == MsgType::Ping) {
            Ping p; bool pong; DecodePing(d, n, p, pong);
            net_->Send(ev.peer, kChanUnreliable, EncodePing(p.timeMs, true), false);
            return;
        }
        auto it = peers_.find(ev.peer);
        if (it == peers_.end()) return;                // nothing but Hello/Ping before Welcome
        Peer& peer = it->second;
        if (!peer.budget.Allow(t, NowMs())) { BudgetDropped(peer.budget, t, "host", peer.slot); return; }   // M3: a flood ends here, unrelayed
        if (IsFightMsg(t) && !FightMsgsOpen()) { ++streamsDropped_; return; }   // a straggler from the last fight, in a menu
        // Host physics: no client simulates anything; a hand-off: that client's fighter is held elsewhere for now (and a
        // machine whose fighter is held holds nothing).
        if (ClientSimMsg(t) && (HostPhysics() || ClientHanded(peer.slot))) { ++streamsDropped_; return; }
        if (t == MsgType::Input) {   // played under host physics and while held here; kept always, so a hand-off starts from what is held
            InputFrame f;
            if (peer.slot == 0 || peer.slot >= kMaxFighters || !Decode(d, n, f)) { ++streamsDropped_; return; }
            Fighter& fr = fighters_[peer.slot];
            const uint8_t e = Roster::EntityForSlot(peer.slot), holder = leases_.Holder(e);
            if (fr.input.Receive(f)) {
                ++fr.inputsRx; fr.lastInputAt = NowMs(); if (HeldHere(e)) ++heldRx_[e];
                if (f.lockTarget != fr.lockTarget) {   // the player's camera lock (sync/hsm_lockon.h): its yaw already rides in the frame
                    fr.lockTarget = f.lockTarget;
                    if (f.lockTarget) HSM_LOG("input: e%u lock-on on (target e%u)", e, f.lockTarget); else HSM_LOG("input: e%u lock-on off", e);
                }
            }
            // Held by another client: played there.
            if (!HostPhysics() && holder != kNoSlot && holder != kHostSlot)
                if (std::optional<uint32_t> to = PeerOfSlot(holder)) { RelayInput r; r.entity = e; r.frame = f; net_->Send(*to, kChanUnreliable, Encode(r), false); }
        } else if (t == MsgType::Voice) {
            VoiceFromClient(ev, peer);
        } else if (t == MsgType::Pose || t == MsgType::Vitals) {
            HostOwnerStream(ev, peer, t);
        } else if (t == MsgType::HitClaim) {
            HitClaimMsg m; Decode(d, n, m);
            RouteHostClaim(peer.slot, m.claim);
        } else if (t == MsgType::SeverClaim) {
            combat::SeverClaimMsg m; Decode(d, n, m);
            RouteHostSever(peer.slot, m.claim);
        } else if (t == MsgType::SeverNotice) {
            combat::SeverNoticeMsg m;
            if (!Decode(d, n, m) || !MayStream(peer.slot, m.claim.victim, leases_.holders())) { ++streamsDropped_; return; }
            RouteHostNotice(peer.slot, m.claim);
        } else if (t == MsgType::StuckBlade) {
            combat::StuckBladeMsg m; Decode(d, n, m);
            RouteHostStuck(peer.slot, m);
        } else if (t == MsgType::RemoteWrite) {
            sync::RemoteWriteMsg m;
            if (!Decode(d, n, m) || !MayStream(peer.slot, m.from, leases_.holders())) { ++streamsDropped_; return; }
            RouteHostRemote(peer.slot, m);
        } else if (t == MsgType::PaintFx) {
            combat::PaintFxMsg m;
            if (!Decode(d, n, m) || !fx_.Accept(m)) { ++streamsDropped_; return; }   // names and rate (M1, M2, M7): before it is applied or relayed
            if (combat::IsStripRequest(m)) {
                // A strip of a fighter the sender does not own goes to that fighter's owner alone, who checks it is fallen.
                const uint8_t owner = OwnerSlotOf(m.entity, leases_.holders());
                if (owner == 0xFF || owner == peer.slot) { ++streamsDropped_; return; }
                if (owner == kHostSlot) fx_.Apply(m);
                else if (std::optional<uint32_t> to = PeerOfSlot(owner)) net_->Send(*to, kChanReliable, ev.data, true);
                return;
            }
            if (!MayStream(peer.slot, m.entity, leases_.holders())) { ++streamsDropped_; return; }
            fx_.Apply(m);
            for (uint8_t slot : RelayTargets(peer.slot, ConnectedSlots()))
                if (std::optional<uint32_t> to = PeerOfSlot(slot)) net_->Send(*to, kChanReliable, ev.data, true);
        } else if (t == MsgType::Equip) {
            combat::EquipStateMsg m;
            if (!Decode(d, n, m) || !MayStream(peer.slot, m.entity, leases_.holders())) { ++streamsDropped_; return; }
            equip_.Apply(m);
            for (uint8_t slot : RelayTargets(peer.slot, ConnectedSlots()))
                if (std::optional<uint32_t> to = PeerOfSlot(slot)) net_->Send(*to, kChanReliable, ev.data, true);
        } else if (t == MsgType::GrabState) {
            GrabStateMsg m;
            if (!Decode(d, n, m) || !MayStream(peer.slot, m.grabber, leases_.holders())) { ++streamsDropped_; return; }
            grabs_.OnGrabState(m, game::SteadyNowMs());
            for (uint8_t slot : RelayTargets(peer.slot, ConnectedSlots()))
                if (std::optional<uint32_t> to = PeerOfSlot(slot)) net_->Send(*to, ev.channel, ev.data, ev.channel == kChanReliable);
        } else if (t == MsgType::BodyState) {
            if (props_.OnBodyState(d, n, peer.slot))      // only the granted streamer's newer state is relayed
                for (uint8_t slot : RelayTargets(peer.slot, ConnectedSlots()))
                    if (std::optional<uint32_t> to = PeerOfSlot(slot)) net_->Send(*to, kChanUnreliable, ev.data, false);
        } else if (t == MsgType::ObjectAnnounce || t == MsgType::ObjectEvent) {
            // Same gate as a lease: what a client says before it reported Ready for this travel belongs to the previous level.
            if (!LeaseSeqCurrent(peer.slot, director_.ReadySeq(peer.slot), travelSeq_)) { ++streamsDropped_; }
            else if (t == MsgType::ObjectAnnounce) props_.OnObjectAnnounce(d, n, peer.slot);   // the canonical answer goes to everyone
            else if (props_.OnObjectEvent(d, n, peer.slot))                                     // the streamer's, about an object the host knows
                for (uint8_t slot : RelayTargets(peer.slot, ConnectedSlots()))
                    if (std::optional<uint32_t> to = PeerOfSlot(slot)) net_->Send(*to, kChanReliable, ev.data, true);
        } else if (t == MsgType::PropLease) {
            // A client that has not reported Ready for this travel is still in the previous level: what it asked for there
            // is not granted here (W6; LeaseSeqCurrent).
            if (LeaseSeqCurrent(peer.slot, director_.ReadySeq(peer.slot), travelSeq_))
                props_.OnPropLease(d, n, peer.slot);      // LeaseFromPeerOk inside; Grants go out reliable to everyone
            else
                ++streamsDropped_;
        } else if (t == MsgType::HandOff || t == MsgType::HandOffAsk) {
            // Same gate as a prop lease: what a client wrote before it reported Ready for this travel is the last level's.
            if (HostPhysics() || !LeaseSeqCurrent(peer.slot, director_.ReadySeq(peer.slot), travelSeq_)) { ++streamsDropped_; return; }
            if (t == MsgType::HandOff) {
                sync::HandOffMsg m;
                if (Decode(d, n, m) && !m.full) HostLeaseIn(peer.slot, m); else ++streamsDropped_;
            } else {
                sync::HandOffAskMsg m;
                if (!Decode(d, n, m) || OwnerSlotOf(m.by) != peer.slot) { ++streamsDropped_; return; }
                const uint8_t owner = sync::LeaseOwner(m.victim);
                if (owner == kHostSlot) AnswerAsk(m, peer.slot);
                else if (std::optional<uint32_t> to = PeerOfSlot(owner)) net_->Send(*to, kChanReliable, ev.data, true);
            }
        } else if (t == MsgType::DeathReport) {
            DeathReport r; Decode(d, n, r);
            const Holders held = leases_.holders();   // a hand-off: the holder reports the death of a fighter it holds
            if (MayStream(peer.slot, r.victim, held) && StaleLife(r.victim, r.victimLife)) return;   // F3: a late report must not kill the next body
            if (!MayStream(peer.slot, r.victim, held) || !match_.Alive(r.victim) || RespawnQueued(r.victim)) return;
            const uint32_t now = NowMs();
            const bool named = ValidEntity(r.killer) && r.killer != r.victim && match_.Has(r.killer);
            HandleDeath(r.victim, named ? r.killer : kills_.KillerOf(r.victim, now), now);
        } else if (t == MsgType::GiveUp) {
            GiveUpMsg g;
            if (!Decode(d, n, g) || !MayStream(peer.slot, g.entity)) { ++streamsDropped_; return; }
            HandleGiveUp(g.entity, NowMs());
        } else if (t == MsgType::MeshHeard) {
            MeshHeardMsg m;
            if (!MeshHost() || !Decode(d, n, m)) return;   // 0.6.5 behaviour: the host relays everything
            meshMasks_.Set(peer.slot, m.mask);
            HSM_LOG("host: mesh: slot %u hears 0x%03x directly", peer.slot, unsigned(m.mask));
        } else if (t == MsgType::PingTable) {
            PingTableMsg m;
            if (!PeerMapHost() || !cfg_.peerPing || !Decode(d, n, m)) return;
            routeLinks_.ClearRow(peer.slot);
            for (const PingTableEntry& e : m.entries) routeLinks_.Note(peer.slot, e.slot, e.rttMs, e.lossPct);
            pingTables_[peer.slot] = std::move(m);   // logged in the health window (LogPingHealth)
        } else if (t == MsgType::Ready) {
            Ready r; Decode(d, n, r);
            director_.OnReady(peer.slot, r.travelSeq, NowMs(), travelSeq_);
            if (r.travelSeq == travelSeq_) peerSilence_.Loaded(ev.peer, NowMs());   // its 15 s count from here
            for (const Spawn& sp : director_.LiveSpawns()) net_->Send(ev.peer, kChanReliable, Encode(sp), true);
            // The level props' census, then every current lease (a client refuses a level prop's Grant its census lacks),
            // after the level load that cleared the joiner's view (props_.Clear).
            for (const std::vector<uint8_t>& c : props_.CensusForJoiner()) net_->Send(ev.peer, kChanReliable, c, true);
            for (const std::vector<uint8_t>& o : props_.ObjectsForJoiner()) net_->Send(ev.peer, kChanReliable, o, true);   // announces, then events
            for (const std::vector<uint8_t>& g : props_.GrantsForJoiner()) net_->Send(ev.peer, kChanReliable, g, true);
            HSM_LOG("host: slot %u ready in %s (travel #%u)", peer.slot, r.level.c_str(), r.travelSeq);
        } else if (t == MsgType::Chat) {
            ChatLine c; Decode(d, n, c);
            if (!peer.chat.Allow(NowMs())) return;
            if (ModerationLine(c.text, ev.peer, false)) return;   // /kick, /ban, /votekick, /yes...: answered, not chat
            c.fromSlot = peer.slot;
            net_->Broadcast(kChanReliable, Encode(c), true);
            ui::ShowLine(NameOfSlot(peer.slot), ChatNameInk(info_, Roster::EntityForSlot(peer.slot)), c.text);
        } else if (t == MsgType::ReportShare) {
            ReportShareMsg m; Decode(d, n, m);
            uint32_t now = NowMs();
            if (peer.reportShared && now - peer.reportShareAt < kReportShareGapMs) return;
            peer.reportShared = true; peer.reportShareAt = now;
            m.fromSlot = peer.slot;
            const std::vector<uint8_t> msg = Encode(m);
            for (uint8_t slot : RelayTargets(peer.slot, ConnectedSlots()))
                if (std::optional<uint32_t> to = PeerOfSlot(slot)) net_->Send(*to, kChanReliable, msg, true);
            OfferLog(m);
        } else if (t == MsgType::BlobChunk) {
            BlobChunk c; Decode(d, n, c);
            std::vector<uint8_t> blob;
            BlobAssembler::Result res = peer.blobs.Add(c, GetTickCount64(), blob);
            if (res == BlobAssembler::Result::Rejected) HSM_LOG("host: rejected loadout chunk from slot %u", peer.slot);
            if (res != BlobAssembler::Result::Complete) return;
            std::string why;
            if (!CheckPassportSave(blob.data(), blob.size(), &why)) {
                HSM_LOG("host: refused loadout from slot %u (%zu bytes): %s", peer.slot, blob.size(), why.c_str());
                return;
            }
            HSM_LOG("host: loadout from slot %u (%zu bytes)", peer.slot, blob.size());
            const std::vector<uint8_t> own = blob;
            KeepOwnLoadout(peer.slot, own, StoreAndRelay(peer.slot, std::move(blob), ev.peer));
        }
    }

    void OnHello(const NetEvent& ev) {
        helloWatch_.Hello(ev.peer);
        Hello h;
        if (!Decode(ev.data.data(), ev.data.size(), h)) {        // another revision: only its header revision is readable
            h = Hello{}; h.revision = ev.data.size() > 2 ? ev.data[2] : 0;
            if (h.revision == kProtocolRevision) return;
        }
        // Each Hello resends every loadout (megabytes); a peer repeating it is ignored until it could be a rejoin.
        if (auto it = peers_.find(ev.peer); it != peers_.end() && NowMs() - it->second.helloAt < kHelloRepeatMs) {
            HSM_LOG("host: ignoring repeated Hello from peer %u", ev.peer);
            return;
        }
        Reject r;
        std::optional<uint8_t> slot;
        bool rejoined = false;
        uint64_t key = ev.steamId ? ev.steamId : (uint64_t(1) << 63) | ev.peer;
        if (h.revision != kProtocolRevision) r.reason = RejectReason::BadRevision;
        else if (h.buildKey != BuildKey()) r.reason = RejectReason::BuildMismatch;
        else if (const BanEntry* b = BannedEntry(ev)) r.reason = b->kick ? RejectReason::Kicked : RejectReason::Banned;
        else if (!(slot = roster_.Add(key, h.name))) r.reason = RejectReason::Full;
        if (slot && reservations_.Claim(key, NowMs())) {        // rejoin: score kept, fighter respawns fresh
            HSM_LOG("host: slot %u rejoined within its reservation", *slot);
            rejoined = true; match_.Return(Roster::EntityForSlot(*slot));   // it stays down until the next respawn / round (K2)
            DespawnFighter(*slot);
            const uint8_t e = Roster::EntityForSlot(*slot);
            director_.Forget(e);   // not live any more: SpawnDue respawns it and sends everyone a fresh Spawn
            // A restarted machine: its stream, claim ids, gore and holds start afresh, here and everywhere.
            standIns_.Forget(e); severs_.Forget(e); gore_.Forget(e); claims_.Forget(e); fx_.Forget(e); equip_.Forget(e); grabs_.Forget(e); props_.ForgetEntity(e); lives_.erase(e);
            Despawn dp; dp.entity = e;   // the others drop its old body
            for (const auto& [peer, p] : peers_)
                if (peer != ev.peer && p.key != key) net_->Send(peer, kChanReliable, Encode(dp), true);
        }
        if (!slot) {
            HSM_LOG("host: rejecting peer %u (reason %d; its revision %u build %016llx, ours %u build %016llx)", ev.peer, int(r.reason),
                    h.revision, (unsigned long long)h.buildKey, kProtocolRevision, (unsigned long long)BuildKey());
            RejectPeer(ev.peer, r.reason);
            return;
        }
        // A reconnect can arrive before the old connection's Disconnected: forget the stale peer so its late
        // disconnect does not remove the member's fighter.
        std::erase_if(peers_, [&](const auto& kv) { return kv.first != ev.peer && kv.second.key == key; });
        Peer& p = peers_[ev.peer]; p = Peer{}; p.slot = *slot; p.key = key; p.helloAt = NowMs(); p.name = util::StripControlChars(h.name); p.modBuild = h.modBuild;
        MeshRosterChanged(*slot);
        if (h.modBuild != game::ModBuild())
            HSM_LOG("host: slot %u (%s) runs mod build '%s', ours '%s'", *slot, h.name.c_str(), h.modBuild.c_str(), game::ModBuild().c_str());
        director_.OnJoin(*slot, NowMs());
        if (match_.phase() != MatchPhase::Idle && !(rejoined && slotTeam_.count(*slot))) AssignLateTeam(*slot);   // K6: a rejoiner keeps its team
        Welcome w; w.sessionId = sessionId_; w.slot = *slot; w.entity = Roster::EntityForSlot(*slot);
        w.snapshotHz = uint16_t(SnapshotHz()); w.map = currentLevel_; w.syncMode = uint8_t(syncMode_); w.modBuild = game::ModBuild();
        if (IsServer()) w.flags |= kWelcomeNoHostFighter;   // role=server: no host fighter to spectate or score
        if (MeshHost()) w.flags |= kWelcomeMesh;            // the PeerMap follows on the next tick
        if (PeerMapHost() && cfg_.peerPing) w.flags |= kWelcomePeerPing;   // rev 22: clients ping each other and report
        net_->Send(ev.peer, kChanReliable, Encode(w), true);
        net_->Send(ev.peer, kChanReliable, Encode(leases_.Snapshot()), true);   // rev 32: every hand-off lease (its own stream's epoch too)
        SendAllBlobs(ev.peer);
        if (InFightLevel()) {                          // late joiner: follow the host into the running level
            Travel t;
            if (lastTravel_ && lastTravel_->level.find(currentLevel_) != std::string::npos) t = *lastTravel_;
            else { t.level = currentLevel_; t.settings = game::CaptureGameSettings(); }   // short map name resolves too
            if (!t.seq) t.seq = ++travelSeq_;
            net_->Send(ev.peer, kChanReliable, Encode(HostModMapFor(t.level)), true);   // modded maps: the pak it needs, before the Travel
            if (!game::shareddice::Active()) game::shareddice::SetSeed(NewLevelSeed());   // hosting started inside this level: its dice start now
            net_->Send(ev.peer, kChanReliable, Encode(dice::LevelSeedMsg{t.seq, game::shareddice::Seed()}), true);   // the running level's seed
            net_->Send(ev.peer, kChanReliable, Encode(t), true);
        }
        HSM_LOG("host: '%s' joined as slot %u (%zu/%d fighters)", h.name.c_str(), *slot, roster_.size(), kMaxPlayers);
    }

    // A Steam member who drops mid-match keeps slot, fighter and score for ReservationTable's window.
    void OnClientLeft(uint32_t peer) {
        peerSilence_.Forget(peer);
        auto it = peers_.find(peer);
        if (it == peers_.end()) return;
        uint8_t slot = it->second.slot;
        uint64_t key = it->second.key;
        vote_.Left(key);
        peers_.erase(it);
        MeshRosterChanged(slot);
        props_.SlotLeft(slot);
        FreeSlotLeases(slot);                           // a hand-off it held or was in ends
        ReleaseInputs(slot);                            // host physics: its held body lets go of everything
        bool steamKey = !(key >> 63);
        if (match_.phase() != MatchPhase::Idle && steamKey && reservations_.Reserve(key, slot, NowMs())) {
            LeaverFalls(slot);   // K1: the held body can't be hit (claims to a gone owner are dropped), so the fall is recorded now
            HSM_LOG("host: slot %u dropped mid-match; holding it for a rejoin", slot);
            return;
        }
        RemoveMember(slot, key);
    }

    // K1/K2: a leaver in a fought phase falls (no kill credited; a Duel round goes to whoever is left) and stays down.
    void LeaverFalls(uint8_t slot) {
        const uint8_t e = Roster::EntityForSlot(slot);
        if (!match_.Has(e) || !CombatPhase(match_.phase())) return;
        std::erase(respawnQueue_, e);
        if (match_.Alive(e)) HandleDeath(e, 0, NowMs());
        match_.Leave(e, NowMs());
    }

    void RemoveMember(uint8_t slot, uint64_t key) {
        LeaverFalls(slot);
        slotTeam_.erase(slot);   // K6: the slot's next player picks a team afresh
        kills_.Forget(Roster::EntityForSlot(slot)); kills_.ForgetAttacker(Roster::EntityForSlot(slot));   // K3
        roster_.Remove(key);
        DespawnFighter(slot);
        standIns_.Forget(Roster::EntityForSlot(slot)); severs_.Forget(Roster::EntityForSlot(slot)); gore_.Forget(Roster::EntityForSlot(slot)); lives_.erase(Roster::EntityForSlot(slot));
        claims_.Forget(Roster::EntityForSlot(slot)); fx_.Forget(Roster::EntityForSlot(slot)); equip_.Forget(Roster::EntityForSlot(slot));
        grabs_.Forget(Roster::EntityForSlot(slot));
        VoiceForget(Roster::EntityForSlot(slot));
        props_.SlotLeft(slot); props_.ForgetEntity(Roster::EntityForSlot(slot));
        hostBlobs_.DropSlot(slot);
        if (slot < ownLoadout_.size()) { ownLoadout_[slot].clear(); ownBlobId_[slot] = 0; }
        if (uint8_t e = director_.OnLeave(slot)) { Despawn dp; dp.entity = e; net_->Broadcast(kChanReliable, Encode(dp), true); }
        match_.RemoveFighter(Roster::EntityForSlot(slot)); infoDirty_ = true;
        HSM_LOG("host: slot %u left", slot);
    }

    // ------------------------------------------------------------------ match (host)
    int8_t TeamOfSlot(uint8_t slot) const { auto it = slotTeam_.find(slot); return it == slotTeam_.end() ? kNoTeam : it->second; }

    uint8_t EntityOf(UObject* willie) {
        if (!willie) return 0;
        if (willie == hostWillie_) return 1;
        for (uint8_t s = 1; s < kMaxFighters; ++s) if (fighters_[s].actor.obj == willie) return Roster::EntityForSlot(s);
        for (auto& [e, ref] : bots_) if (ref.obj == willie) return e;
        return 0;
    }

    std::vector<UObject*> StandInActors() const {
        std::vector<UObject*> out;
        for (uint8_t e = 1; e <= kMaxWillies; ++e) if (UObject* w = standIns_.ActorOf(e)) out.push_back(w);
        return out;
    }
    UObject* EntityActor(uint8_t e) {
        if (e >= 1 && e <= kMaxFighters) return FighterActor(uint8_t(e - 1));
        auto it = bots_.find(e);
        return it != bots_.end() && adapter_.StillValid(it->second.obj, it->second.index) ? it->second.obj : nullptr;
    }

    // Owner sync: every machine resolves contacts between the fighters it simulates itself, natively, with the same
    // rule; a contact with a stand-in runs on to Deal Complex Damage, where game::Claims decides it. A clash of my
    // weapon with a stand-in's weapon is noted for the parry rule.
    bool HitHook(UObject* obj, UFunction* fn, void* params) {
        if (params && recorder_.Recording()) RecordHit(obj, fn, params);
        if (!net_ || !params || !inFight_) return false;
        int32_t off = OtherActorOffset(fn);
        if (off < 0) return false;
        UObject* other = *reinterpret_cast<UObject**>(static_cast<uint8_t*>(params) + off);
        props_.NoteContact(obj, other);
        // Cheap first (test 5: bodies against the ground were most of the 8970 calls of one Abyss frame): a contact
        // with no weapon on either side has no attacker, so it is never blocked or credited; what a class
        // is gets looked up once per class, whom an actor belongs to once per frame.
        const HitKind selfKind = HitKindOf(obj), otherKind = HitKindOf(other);
        const bool selfWeapon = selfKind == HitKind::Weapon, otherWeapon = otherKind == HitKind::Weapon;
        if (selfWeapon && otherWeapon) { NoteWeaponClash(obj, other); return false; }
        if (!selfWeapon && !otherWeapon) return false;
        UObject* weapon = selfWeapon ? obj : other;
        UObject* victim = selfWeapon ? (otherKind == HitKind::Willie ? other : FighterOfThisFrame(other)) : obj;
        if (!victim || HitKindOf(victim) != HitKind::Willie) return false;
        UObject* attacker = FighterOfThisFrame(weapon);
        uint8_t ve = OwnedEntityOf(victim);
        uint8_t ae = attacker ? OwnedEntityOf(attacker) : 0;
        if (!ve) return false;                        // not a session fighter (e.g. a stock foe being removed), or a stand-in
        if (!CombatEnabled()) { ++hitsBlocked_; return true; }
        bool ff = IsHost() ? match_.config().friendlyFire : info_.friendlyFire;
        if (!HitAllowed(true, ae ? KnownTeam(ae) : kNoTeam, KnownTeam(ve), ff)) { ++hitsBlocked_; return true; }
        if (ae && ae != ve && IsHost()) kills_.OnHit(ve, ae, NowMs());
        else if (ae && ae != ve && ve <= kMaxFighters) nativeHits_[ve] = {ae, game::SteadyNowMs()};   // a hand-off: two fighters simulated here
        return false;
    }

    // The recorder's Hit events, single player too: a weapon against a fighter or another weapon, or fighter against
    // fighter. Contacts with anything else (the ground, walls) and a fighter's own gear against itself are skipped.
    void RecordHit(UObject* obj, UFunction* fn, void* params) {
        const int32_t off = OtherActorOffset(fn);
        if (off < 0) return;
        UObject* other = *reinterpret_cast<UObject**>(static_cast<uint8_t*>(params) + off);
        const HitKind a = HitKindOf(obj), b = HitKindOf(other);
        if (a == HitKind::Other || b == HitKind::Other || obj == other) return;
        UObject* fa = a == HitKind::Willie ? obj : FighterOfThisFrame(obj);
        UObject* fb = b == HitKind::Willie ? other : FighterOfThisFrame(other);
        if (fa == fb) return;   // the same fighter's own gear, or two loose weapons
        FVector impulse{};
        if (const int32_t imp = NormalImpulseOffset(fn); imp >= 0)
            std::memcpy(&impulse, static_cast<uint8_t*>(params) + imp, sizeof impulse);
        if (a != HitKind::Weapon && b == HitKind::Weapon) recorder_.Hit(other, obj, fb, fa, true, impulse);   // the weapon strikes
        else recorder_.Hit(obj, other, fa, fb, a == HitKind::Weapon, impulse);
    }

    void NoteWeaponClash(UObject* a, UObject* b) {
        UObject* fa = FighterOfThisFrame(a);
        UObject* fb = FighterOfThisFrame(b);
        const uint8_t sa = standIns_.EntityOf(fa), sb = standIns_.EntityOf(fb);
        if (sa && !sb && OwnedEntityOf(fb)) claims_.NoteClash(sa, a);
        else if (sb && !sa && OwnedEntityOf(fa)) claims_.NoteClash(sb, b);
    }

    // What a contact's actor is, per class: hit events fire thousands of times a frame. Not remembered while the
    // fighter class is not known yet (a Willie would read as other); dropped on a level change.
    enum class HitKind : uint8_t { Other, Willie, Weapon };
    HitKind HitKindOf(UObject* o) {
        if (!o) return HitKind::Other;
        UClass* c = ClassOf(o);
        auto it = hitKind_.find(c);
        if (it != hitKind_.end()) return it->second;
        const HitKind k = adapter_.IsWillie(o) ? HitKind::Willie : adapter_.IsWeapon(o) ? HitKind::Weapon : HitKind::Other;
        if (k != HitKind::Other || adapter_.WillieClassKnown()) hitKind_.emplace(c, k);
        return k;
    }
    // Adapter::FighterOf (attach parents, a weapon's holder) once per actor per frame; Tick starts each frame empty.
    UObject* FighterOfThisFrame(UObject* actor) {
        if (!actor) return nullptr;
        auto it = fighterOfFrame_.find(actor);
        if (it != fighterOfFrame_.end()) return it->second;
        UObject* f = adapter_.FighterOf(actor);
        fighterOfFrame_.emplace(actor, f);
        return f;
    }

    // Hit events fire thousands of times a second: the parameter offset is looked up once per function.
    int32_t OtherActorOffset(UFunction* fn) {
        auto it = otherActorOffset_.find(fn);
        if (it != otherActorOffset_.end()) return it->second;
        FProperty* p = FindProperty(fn, "OtherActor");
        int32_t off = p ? PropertyOffset(p) : -1;
        otherActorOffset_[fn] = off;
        return off;
    }

    // "NormalImpulse" (an FVector) of a hit signature, -1 without one; looked up once per function like OtherActor.
    int32_t NormalImpulseOffset(UFunction* fn) {
        auto it = normalImpulseOffset_.find(fn);
        if (it != normalImpulseOffset_.end()) return it->second;
        FProperty* p = FindProperty(fn, "NormalImpulse");
        int32_t off = p && PropertyType(p) == "StructProperty" ? PropertyOffset(p) : -1;
        normalImpulseOffset_[fn] = off;
        return off;
    }

    int8_t KnownTeam(uint8_t e) {
        if (IsHost()) return match_.TeamOf(e);
        for (const FighterEntry& f : info_.fighters) if (f.entity == e) return f.team;
        return kNoTeam;
    }

    // A player joining a running match keeps everyone else's team and goes to their pick or the smallest team.
    void AssignLateTeam(uint8_t slot) {
        std::vector<TeamRequest> req;
        for (const auto& [s, t] : slotTeam_) req.push_back({s, kNoTeam, t});
        TeamRequest mine; mine.slot = slot;
        if (steamMode()) if (const RosterMember* m = roster_.BySlot(slot))
            for (const game::LobbyMember& lm : lobby_.members()) if (lm.id == m->key) { mine.pick = int8_t(lm.team); mine.hostOverride = int8_t(lm.hostOverride); }
        req.push_back(mine);
        slotTeam_[slot] = ResolveTeams(req, match_.config().format)[slot];
    }

    // Item 8: a match starts only while every joined player runs this mod build (their Hello), unless allow_build_mismatch=1.
    // Logs each new warning once.
    bool BuildsAllowStart() {
        std::vector<PeerBuild> others;
        for (const auto& [peer, p] : peers_) others.push_back({p.name, p.modBuild.empty() ? std::string("an older build") : p.modBuild});
        const std::string mine = game::ModBuild();
        const std::string warn = BuildWarning(mine, others);
        if (warn != buildWarnLogged_) {
            buildWarnLogged_ = warn;
            if (!warn.empty())
                HSM_LOG("host: %s (ours '%s')%s", warn.c_str(), mine.c_str(),
                        cfg_.allowBuildMismatch ? "; allow_build_mismatch=1, starting anyway" : "; no match starts until every build matches");
        }
        return MayStartMatch(mine, others, cfg_.allowBuildMismatch);
    }

    // role=server: the server_* keys. teams = two teams, survival = players vs bots.
    Format ServerFormat() const {
        Format f = Format::FreeForAll;
        if (!ParseServerFormat(cfg_.serverFormat, &f)) return Format::FreeForAll;
        return f;
    }
    MatchConfig ServerMatchConfig() const {
        MatchConfig c;
        c.format = ServerFormat();
        c.scoreLimit = cfg_.serverScoreLimit > 0 ? uint16_t(cfg_.serverScoreLimit) : DefaultScoreLimit(c.format);
        c.timeLimitS = uint16_t(cfg_.serverTimeLimitS);
        c.friendlyFire = false; c.gear = Gear::Own;
        c.hostPhysics = cfg_.hostPhysics;
        c.damagePct = cfg_.serverDamagePct < 0 ? kDamageStock : uint16_t(cfg_.serverDamagePct);
        return Sanitize(c);
    }

    // The player damage every machine runs (match/hsm_damage.h): the host's lobby or server_damage; a client takes the
    // Steam lobby's, or the host's MatchInfo on a direct connection.
    uint16_t SessionDamage() const {
        if (IsServer()) return ServerMatchConfig().damagePct;
        if (steamMode() && lobby_.InLobby()) return lobby_.config().damagePct;
        return IsHost() ? kDamageStock : info_.damagePct;
    }

    // The match StartMatch would start now: the lobby's settings, or Progression co-op outside Free Mode.
    MatchConfig StartConfig() {
        MatchConfig cfg = IsServer() ? ServerMatchConfig() : steamMode() ? lobby_.config() : MatchConfig{};
        if (!adapter_.FreeModeActive()) { cfg.format = Format::PlayersVsBots; cfg.scoreLimit = 100; cfg.timeLimitS = 0; }   // co-op against the event's opponents, no clock
        if (cfg.format == Format::Duel) cfg.friendlyFire = true;   // two fighters, no teams: every hit counts
        return cfg;
    }

    // The identity a player's score follows (FighterInfo::id): their SteamID, the host's own included; 0 when unknown.
    uint64_t ScoreId(uint8_t entity) const {
        const RosterMember* m = steamMode() ? roster_.ByEntity(entity) : nullptr;
        return !m ? 0 : m->key == kHostKey ? steam::MySteamId() : (m->key >> 63) ? 0 : m->key;
    }

    void StartMatch() {
        MatchConfig cfg = StartConfig();
        bool progression = !adapter_.FreeModeActive();
        std::vector<TeamRequest> req;
        for (const RosterMember& m : roster_.members()) {
            TeamRequest r; r.slot = m.slot;
            if (steamMode()) for (const game::LobbyMember& lm : lobby_.members())
                if (lm.id == (m.key == kHostKey ? steam::MySteamId() : m.key)) { r.pick = int8_t(lm.team); r.hostOverride = int8_t(lm.hostOverride); }
            req.push_back(r);
        }
        slotTeam_ = ResolveTeams(req, cfg.format);
        std::vector<FighterInfo> list;
        for (const RosterMember& m : roster_.members()) list.push_back({m.entity, TeamOfSlot(m.slot), true, ScoreId(m.entity)});
        match_.Start(cfg, list, NowMs(), migrateBoard_);   // after a host migration: the old host's scores
        if (!migrateBoard_.empty())
            HSM_LOG("host: scores restored for %zu players%s", migrateBoard_.players.size(), migrateBoard_.format == cfg.format ? "" : " (not applied: other format)");
        migrateBoard_ = ScoreCarry{};
        kills_ = KillLog{}; wave_ = 0; adoptStock_ = IsBotFormat(cfg.format); wavesEnabled_ = adoptStock_ && !progression; looted_.clear();
        respawnQueue_.clear();
        Regear startGear = RegearFor(match_.config(), RespawnCause::Rematch);   // everybody starts together; None for Own and Brawl
        adapter_.SetBare(IsBrawl(match_.config()));      // every body from the match start on (RespawnAll below) comes up bare
        adapter_.SetBuild(ForcedWeight(match_.config()), cfg_.forceStrength && !progression, int(match_.config().tier));   // and in the tier's build
        progression_ = progression; oneFight_ = progression;
        sidesValid_ = false;
        HSM_LOG("host: match started (format %d, limit %u, time %us, friendly fire %s)%s", int(cfg.format), cfg.scoreLimit, cfg.timeLimitS,
                cfg.friendlyFire ? "on" : "off", progression ? " - Progression co-op" : "");
        HSM_LOG("host: match gear %d, tier %u", int(cfg.gear), cfg.tier);
        if (progression) HSM_LOG("host: Progression start: %zu player(s), stock fighters adopted as bots %s, level %s", roster_.size(), adoptStock_ ? "on" : "OFF", currentLevel_.c_str());
        RegearSlots(match_.config(), AllSlots(), startGear, "match start");
        if (startGear == Regear::None && cfg.gear == Gear::Own) RestoreOwnLoadouts("match start");   // #62: after a generated-gear match
        BroadcastInfo(NowMs());                          // the clients know the match's gear before their Spawns arrive
        RespawnAll();   // team sides / generated loadouts take effect; F2: and everyone starts alive in the arena's start spots, own gear too
    }

    // role=server: the lobby-less loop (session/hsm_autoflow.h). Announcements are SERVER chat lines (ServerSay).
    // Keeps the public lobby up: opens it once Steam is ready, reopens it if it is lost, and publishes the server's
    // match settings so the lobby browser and the members' lobby page show them.
    void ServerLobbyTick() {
        const uint32_t now = NowMs();
        if (lobby_.state() == game::Lobby::State::Idle && DueAt(now, serverLobbyAt_)) {
            serverLobbyAt_ = now + 15000;
            HSM_LOG("server: opening a public Steam lobby");
            lobby_.Host(steam::LobbyType::Public, true);   // one member more: the server is not a player
        }
        if (lobby_.IsHost() && DueAt(now, serverConfigAt_)) {
            serverConfigAt_ = now + 5000;
            lobby_.SetConfig(ServerMatchConfig());
            // The browser shows the server's name (name= in HSMMod.ini) instead of "<account>'s fight".
            const std::string shown = cfg_.playerName.empty() || cfg_.playerName == "Player" ? MyName() + " (server)" : cfg_.playerName;
            if (steam::LobbyData(lobby_.id(), "name") != shown) steam::SetLobbyData(lobby_.id(), "name", shown);
        }
    }

    void ServerFlowTick() {
        if (!flow_) {
            std::vector<std::string> unknown;
            Format f = Format::FreeForAll;
            if (!ParseServerFormat(cfg_.serverFormat, &f)) HSM_LOG("server: unknown server_format=%s; using ffa", cfg_.serverFormat.c_str());
            std::vector<size_t> maps = ParseServerMaps(cfg_.serverMaps, f, &unknown);
            for (const std::string& u : unknown) HSM_LOG("server: server_maps names an unknown map '%s'", u.c_str());
            AutoFlowConfig fc;
            fc.minPlayers = cfg_.serverMinPlayers; fc.countdownMs = uint32_t(cfg_.serverCountdownS) * 1000u;
            flow_ = std::make_unique<AutoFlow>(fc, maps);
            HSM_LOG("server: rotation of %zu map(s) for format %d", maps.size(), int(f));
        }
        FlowInput in; in.nowMs = NowMs(); in.clients = int(roster_.size());
        if (!serverTravelling_) in.phase = match_.phase() == MatchPhase::Idle ? FlowPhase::None : match_.phase() == MatchPhase::Results ? FlowPhase::Results : FlowPhase::Active;
        const FlowAction a = flow_->Tick(in);
        if (a.kind == FlowAction::Announce) {
            ServerSay(a.secondsLeft > 0 ? "Match starts in " + std::to_string(a.secondsLeft) + " s" : "Match starting");
        } else if (a.kind == FlowAction::Travel) {
            if (BuildsAllowStart()) {
                HSM_LOG("server: next map %s", kMaps[a.map].display);
                serverTravelling_ = true;
                StartOnMap(kMaps[a.map].path);
            } else HSM_LOG("server: a peer mod build differs; not starting %s", kMaps[a.map].display);
        } else if (a.kind == FlowAction::ToMenu) {
            HSM_LOG("server: empty; back to the menu");
            serverTravelling_ = false;
            HostTravel(kMenuLevel);
        }
    }

    void HostMatchTick() {
        hostWillie_ = localFighter_ && MatchLevel() ? adapter_.PlayerWillie(0) : nullptr;
        if (!MatchLevel()) return;
        if (serverTravelling_) return;                   // role=server: the next map is on its way; no new match on this one
        uint32_t now = NowMs();
        bool wanted = roster_.size() >= 2 || (steamMode() && IsBotFormat(lobby_.config().format));
        if (IsServer()) wanted = roster_.size() >= (IsBotFormat(ServerFormat()) ? size_t(1) : size_t(cfg_.serverMinPlayers));
        if (match_.phase() == MatchPhase::Idle) {
            // Start once every member's fighter is in (the director's ready/loadout timeouts bound the wait).
            bool allIn = true;
            std::string notLive;
            for (const RosterMember& m : roster_.members()) if (!director_.Live(m.entity)) { allIn = false; notLive += " e" + std::to_string(m.entity); }
            if (!adapter_.FreeModeActive() && DueAt(now, nextIdleLog_)) {   // #37: a Progression match that never starts (nothing else says why)
                nextIdleLog_ = now + 5000;
                HSM_LOG("host: Progression match idle: roster %zu, wanted %d, host fighter %s, fighters not live:%s, builds %s", roster_.size(),
                        int(wanted), hostWillie_ ? "yes" : localFighter_ ? "MISSING" : "none", notLive.empty() ? " none" : notLive.c_str(),
                        BuildsAllowStart() ? "ok" : "BLOCK");
            }
            if (!(wanted && (hostWillie_ || !localFighter_) && allIn)) { if (wanted) MakeStartGearAhead(now); return; }
            if (!BuildsAllowStart()) return;
            StartMatch();
        }
        if (DueAt(now, nextScan_)) { nextScan_ = now + kScanIntervalMs; ScanStockWillies(); }
        DetectDeaths(now);
        GiveUpFallbackTick(now);
        match_.SetPlayers(int(roster_.members().size()));   // the host's own fighter is in it as slot 0
        Match::Events ev = match_.Tick(now);
        if (ev.live || ev.ended || ev.rematch || ev.waveStarted || ev.waveCleared || ev.roundReset) infoDirty_ = true;
        if (ev.waveStarted) HSM_LOG("host: wave %d starts (%d bots)", match_.wave() + 1, match_.botsLeft());
        if (ev.waveCleared) HSM_LOG("host: wave cleared");
        if (ev.roundReset) HSM_LOG("host: duel round reset");
        if (ev.live || ev.roundReset || ev.rematch) draw_.Reset(now);
        CheckDraw(now);
        if (dropWatch_.Observe(match_.config().format, uint8_t(match_.phase()), uint16_t(match_.round()))) {
            ClearDroppedWeapons();
            HostMapReset(ev.rematch || match_.config().format != Format::Duel);
        }
        GroundClearTick(now);
        if (ev.ended) HSM_LOG("host: match over, winner key %d", match_.winner());
        MakeGearAhead(now);
        if (ev.rematch && IsServer()) {                  // role=server: the results are over, the flow picks the next map
            serverTravelling_ = true;
            if (flow_) flow_->ResultsOver();
            return;
        }
        if (ev.rematch && progression_) {                // Progression fights end back in the Tavern, like the stock game
            HSM_LOG("host: Progression fight over; returning everyone to the hub");
            HostTravel(kHubLevel);
            return;
        }
        if (ev.regear != Regear::None) {                 // the blobs go out on the reliable channel before the Spawns
            std::vector<uint8_t> slots;
            if (ev.rematch) slots = AllSlots();
            else for (uint8_t e : ev.respawn) if (e >= 1 && e <= kMaxFighters && roster_.BySlot(uint8_t(e - 1))) slots.push_back(uint8_t(e - 1));
            RegearSlots(match_.config(), slots, ev.regear, ev.rematch ? "rematch" : "respawn");
            infoDirty_ = true;
        }
        for (uint8_t e : ev.respawn)
            if (!RespawnQueued(e)) respawnQueue_.push_back(e);
        if (ev.rematch) RespawnAll();
        if (!respawnQueue_.empty()) { const uint8_t e = respawnQueue_.front(); respawnQueue_.pop_front(); RespawnEntity(e); }
        if (progression_ && !bots_.empty() && match_.phase() == MatchPhase::Live) {
            bool anyBot = false;
            for (auto& [e, ref] : bots_) anyBot = anyBot || match_.Alive(e);
            if (!anyBot) { match_.Finish(kHumanTeam, now); infoDirty_ = true; }
        }
        if (wavesEnabled_ && ev.spawnBots) SpawnWaveBots(ev.spawnBots, ev.botRank);
        if (wavesEnabled_ && DueAt(now, nextCorpseSweep_)) {
            nextCorpseSweep_ = now + kScanIntervalMs;
            for (uint8_t e : match_.StaleBotCorpses(now, Match::kCorpseMs)) RemoveBot(e);
            // Sync audit 37: a bot whose body something else destroyed stayed alive in the match, holding its entity and
            // its wave; it goes.
            std::vector<uint8_t> vanished;
            for (auto& [e, ref] : bots_) if (!adapter_.StillValid(ref.obj, ref.index)) vanished.push_back(e);
            for (uint8_t e : vanished) { HSM_LOG("host: bot %u's body is gone (not destroyed by the mod); removed", e); RemoveBot(e); }
        }
        if (wavesEnabled_ && match_.config().format == Format::PlayersVsBots)   // zombies disintegrate (the stock death)
            for (uint8_t e : match_.StaleBotCorpses(now, Match::kLootDelayMs)) {
                auto it = bots_.find(e);
                if (it == bots_.end() || !looted_.insert(e).second || !adapter_.StillValid(it->second.obj, it->second.index)) continue;
                int weapons = 0;
                const int pieces = adapter_.DropLoot(it->second.obj, uint32_t(e) * 2654435761u ^ now, weapons);
                HSM_LOG("host: bot %u dropped its loot: %d weapon(s), %d armour piece(s)", e, weapons, pieces);
            }
        if (infoDirty_ || now >= nextInfo_) BroadcastInfo(now);
    }

    void DetectDeaths(uint32_t now) {
        std::vector<uint8_t> entities;
        for (const RosterMember& m : roster_.members()) entities.push_back(m.entity);
        for (auto& [e, ref] : bots_) entities.push_back(e);
        for (uint8_t e : entities) {
            if (!match_.Alive(e) || RespawnQueued(e)) continue;   // its old body is still dead until its respawn
            // Owner sync, a fighter simulated elsewhere (a client's, or mine held by a client): its DeathReport, else its vitals.
            if ((e >= 2 && e <= kMaxFighters && !HostDrives(e)) || (e == HostEntity() && MyFighterHanded())) {
                auto it = lives_.find(e);
                if (it != lives_.end() && it->second.deadSince && now - it->second.deadSince >= kVitalsDeathMs) {
                    HSM_LOG("host: entity %u reported dead by its vitals only", e);
                    HandleDeath(e, kills_.KillerOf(e, now), now);
                }
                continue;
            }
            UObject* w = EntityActor(e);
            if (!w || !adapter_.IsDead(w)) continue;
            HandleDeath(e, kills_.KillerOf(e, now), now);
        }
    }

    void HandleDeath(uint8_t e, uint8_t killer, uint32_t now) {
        {
            bool gaveUp = false;
            if (auto g = gaveUp_.find(e); g != gaveUp_.end()) { gaveUp = GaveUpDeath(g->second, now); gaveUp_.erase(g); }
            if (gaveUp && (killer == e || IsBotFormat(match_.config().format))) killer = 0;   // a give-up never scores -1: the attacker's kill, or nobody's (a bot's never)
            match_.OnDeath(e, killer, now);
            Kill k; k.killer = killer; k.victim = e;
            if (gaveUp) k.flags = 8;
            else if (killer == 0) k.flags = 4;
            else if (killer == e) k.flags = 2;
            else if (match_.TeamOf(killer) != kNoTeam && match_.TeamOf(killer) == match_.TeamOf(e)) k.flags = 1;
            HSM_LOG("host: entity %u died (killer %u, flags %u, round %d)", e, killer, unsigned(k.flags), match_.round());
            net_->Broadcast(kChanReliable, Encode(k), true);
            PushFeed(k);
            infoDirty_ = true;
        }
    }

    // Duel: a round nobody can finish is drawn (hsm_draw.h; test 2026-10-25: two rounds of almost four minutes with
    // both on the ground). No point to anyone; the usual gap, then both respawn. Every fighter of the round must be
    // seen, or nothing is decided this look.
    void CheckDraw(uint32_t now) {
        if (!match_.RoundInPlay() || !DueAt(now, nextDraw_)) return;
        nextDraw_ = now + kDrawSampleMs;
        std::vector<DownSample> live;
        for (const RosterMember& m : roster_.members()) {
            if (!match_.Has(m.entity) || !match_.Alive(m.entity) || RespawnQueued(m.entity)) continue;
            DownSample d;
            if (!SampleDown(m.entity, d)) return;
            live.push_back(d);
        }
        const DrawReason why = draw_.Update(live, now);
        if (why == DrawReason::None || live.empty() || !match_.Draw(now)) return;
        Kill k; k.victim = live[0].entity; k.killer = live.size() > 1 ? live[1].entity : 0;
        k.flags = why == DrawReason::Stalled ? 32 : 16;
        HSM_LOG("match: round %d drawn (%s, %zu fighters)", match_.round(), why == DrawReason::Stalled ? "no hits" : "all down", live.size());
        net_->Broadcast(kChanReliable, Encode(k), true);
        PushFeed(k);
        infoDirty_ = true;
    }

    // Item 9e: the first key or mouse button after a fight level loads, and where it went. For the -hsmauto -hsmmirror
    // case where the player's input never reached the fighter: a key seen with the game in front means the keys arrive
    // and the game's controller ignores them; another window in front, or nothing for a minute, means they never came.
    void InputProbeTick() {
        if (!probeSince_) return;
        const uint32_t now = NowMs();
        static constexpr int kKeys[] = {'W', 'A', 'S', 'D', VK_SPACE, VK_LBUTTON, VK_RBUTTON, VK_SHIFT, VK_CONTROL, 'E', 'Q', 'F', 'G', 'R'};
        int vk = 0;
        for (int k : kKeys)
            if (GetAsyncKeyState(k) & 0x8000) { vk = k; break; }
        if (!vk) {
            if (now - probeSince_ >= 60000) {
                HSM_LOG("input: no key or mouse button in the first 60 s of %s", currentLevel_.c_str());
                probeSince_ = 0;
            }
            return;
        }
        DWORD pid = 0;
        if (HWND w = GetForegroundWindow()) GetWindowThreadProcessId(w, &pid);
        UObject* own = adapter_.PlayerWillie(0);
        HSM_LOG("input: first key after level load: vk 0x%02X, %u ms into %s; game window %s, chat %s, local fighter %s, game %s", vk,
                now - probeSince_, currentLevel_.c_str(), pid == GetCurrentProcessId() ? "in front" : "NOT in front",
                ui::Typing() ? "typing" : "closed", own ? (adapter_.IsDead(own) ? "dead" : "present") : "none",
                adapter_.GamePaused() ? "paused" : "running");
        probeSince_ = 0;
    }

    // Item 9c: the HUD's down mark (MatchInfo), from the draw check's last look (every kDrawSampleMs) while a round is fought.
    bool DownInRound(uint8_t e) const { return match_.RoundInPlay() && match_.Alive(e) && draw_.Down(e); }

    // What the draw check needs of a fighter: its body from the bones (a stand-in's follow its owner), its state from
    // its vitals (the owner's, for a stand-in).
    bool SampleDown(uint8_t e, DownSample& d) {
        UObject* w = EntityActor(e);
        FVector pelvis, footL, footR, handR, handL;
        if (!w || !adapter_.BoneLocation(w, "pelvis", pelvis) || !adapter_.BoneLocation(w, "foot_l", footL) ||
            !adapter_.BoneLocation(w, "foot_r", footR) || !adapter_.BoneLocation(w, "hand_r", handR) ||
            !adapter_.BoneLocation(w, "hand_l", handL)) return false;
        VitalsMsg own;
        const VitalsMsg* v = nullptr;
        if (e != MyEntity()) v = standIns_.Vitals(e);
        else if (adapter_.ReadVitals(w, e, own)) v = &own;
        if (!v) return false;
        d.entity = e;
        d.pelvisAboveFeetCm = pelvis.z - std::min(footL.z, footR.z);
        d.handR = {handR.x, handR.y, handR.z}; d.handL = {handL.x, handL.y, handL.z};
        d.fallen = (v->flags & 2) != 0; d.knockedOut = (v->flags & 4) != 0; d.health = v->health;
        return true;
    }

    // Held G: the fighter concedes the Duel round being fought (Match::Forfeit); the feed says they gave up. In the bot
    // formats' break between waves it is the player's vote to skip the break (Match::VoteSkip); in a fought wave of
    // Survival or Zombies it ends the fighter's life (GiveUpActionFor), so a stuck one is replaced at the next wave.
    // Host physics (and a held fighter): the body is simulated here, so the host kills it; otherwise its owner does.
    void KillDrivenBody(uint8_t e) {
        if (e == MyEntity() || e < 2 || e > kMaxFighters || !HostDrives(e)) return;
        if (UObject* w = EntityActor(e)) adapter_.TriggerDeath(w);
    }
    void HandleGiveUp(uint8_t e, uint32_t now) {
        if (progression_ && !InWaveBreak(info_) && match_.GiveUpFight(e, now)) {
            HSM_LOG("match: #%u gave up the Progression fight", e);
            Kill k; k.victim = e; k.flags = 8;
            net_->Broadcast(kChanReliable, Encode(k), true);
            PushFeed(k);
            infoDirty_ = true;
            return;
        }
        if (IsBotFormat(match_.config().format)) {
            if (match_.BreakLeftMs(now) == 0) {                // a wave being fought: G gives this life up
                const MatchPhase ph = match_.phase();
                if (progression_ || (ph != MatchPhase::Live && ph != MatchPhase::SuddenDeath) || RespawnQueued(e) || !match_.Alive(e)) {
                    HSM_LOG("match: #%u held G, but there is nothing to give up (dead, waiting, or no wave being fought)", e);
                    return;
                }
                gaveUp_[e] = now;                          // HandleDeath scores that death as a give-up (flag 8, no killer)
                HSM_LOG("match: #%u gave up the wave (%s)", e, match_.config().format == Format::Zombies ? "Zombies" : "Survival");
                KillDrivenBody(e);
                return;
            }
            if (!match_.VoteSkip(e, now)) { HSM_LOG("match: #%u held G, but it already voted to skip the break", e); return; }
            HSM_LOG("match: #%u votes to skip the break (%zu/%zu)", e, match_.SkipVotes(), match_.SkipVotesNeeded());
            infoDirty_ = true;
            return;
        }
        // Free-for-all, teams and Duel: G ends the fighter's life (its owner calls Death right after sending this). The
        // death that follows within kGaveUpWindowMs is scored as a give-up: the last attacker within a minute gets the
        // kill (in a Duel the round too), nobody otherwise. GiveUpFallbackTick ends it here if no death arrives.
        if (RespawnQueued(e) || !match_.Alive(e)) { HSM_LOG("match: #%u held G, but it is dead or waiting to respawn", e); return; }   // F6
        if (match_.config().format == Format::Duel && !match_.RoundInPlay()) { HSM_LOG("match: #%u held G, but there is no Duel round to give up", e); return; }
        gaveUp_[e] = now;                                  // HandleDeath scores that death as a give-up (flag 8)
        HSM_LOG("match: #%u gives up (last attacker #%u)", e, kills_.KillerOf(e, now));
        KillDrivenBody(e);
    }

    // A give-up whose death never came (the game's Death call did nothing, or the owner's report was lost): the host
    // scores it anyway once the window has passed, so G always ends the fight for that fighter.
    void GiveUpFallbackTick(uint32_t now) {
        for (auto it = gaveUp_.begin(); it != gaveUp_.end();) {
            const uint8_t e = it->first;
            if (GaveUpDeath(it->second, now)) { ++it; continue; }
            it = gaveUp_.erase(it);
            if (!match_.Alive(e) || RespawnQueued(e)) continue;
            HSM_LOG("match: #%u gave up but no death followed in %u ms; scoring it now", e, unsigned(kGaveUpWindowMs));
            uint8_t killer = kills_.KillerOf(e, now);
            if (killer == e || IsBotFormat(match_.config().format)) killer = 0;
            match_.OnDeath(e, killer, now);
            Kill k; k.killer = killer; k.victim = e; k.flags = 8;
            net_->Broadcast(kChanReliable, Encode(k), true);
            PushFeed(k);
            infoDirty_ = true;
        }
    }

    // Duel's new round, and free-for-all and teams' rematch, start without the weapons and armour pieces dropped before
    // (weapons piled up 5 -> 36 in test 13). Every machine clears its own copies; the prop sync lets go of the ones it
    // was moving. Armour the level had at the match start stays (LooseTimer's baseline).
    void ClearDroppedWeapons(const char* why = nullptr) {
        if (!MatchLevel()) return;
        const game::Adapter::DropClear weapons = adapter_.DestroyDroppedWeapons([this](UObject* w) { return props_.IsLevelWeaponAtStart(w); });
        int armor = 0;
        if (looseOn_) {
            std::unordered_map<uint64_t, UObject*> byId;
            const std::vector<uint64_t> ids = LooseIds(byId);
            const double now = double(NowMs());
            looseTimer_.Sweep(ids, now);                                   // a piece dropped since the last sweep is timed too
            for (uint64_t id : looseTimer_.Sweep(ids, now + kIdleDespawnMs)) { adapter_.DestroyActor(byId[id]); ++armor; }   // all but the baseline
        }
        HSM_LOG("drops: %s, cleared %d dropped weapon(s) (kept: held %d, sheathed %d, level %d) and %d armour piece(s)",
                why ? why : Format(info_.format) == Format::Duel ? "new round" : "rematch", weapons.destroyed, weapons.held,
                weapons.sheathed, weapons.level, armor);
    }

    // The "Clear ground" lobby setting (free-for-all, teams, Brawl): every clearGroundMin minutes of a fought phase the host
    // clears its own drops and counts it in MatchInfo.clearCount; each client clears when that count changes (MatchInfo).
    // Held and worn gear stays: ClearDroppedWeapons skips what is held and armour that is attached.
    void GroundClearTick(uint32_t now) {
        const MatchConfig& c = match_.config();
        if (!c.clearGroundMin || !CombatPhase(match_.phase())) { nextGroundClear_ = 0; return; }
        if (!nextGroundClear_) { nextGroundClear_ = now + uint32_t(c.clearGroundMin) * 60000u; return; }
        if (!DueAt(now, nextGroundClear_)) return;
        nextGroundClear_ = now + uint32_t(c.clearGroundMin) * 60000u;
        ++clearCount_; infoDirty_ = true;
        GroundCleared();
    }
    void GroundCleared() {
        ClearDroppedWeapons("timer");
        ui::ShowLine("MATCH", style::kBone, "Ground cleared");
    }

    // Map reset (session/hsm_map_reset.h): the host's setting map_reset sends one MapReset when the drops are cleared (a Duel
    // round, a rematch); every machine, the host too, runs it once the own respawn and the drop clear are done (MapResetGate).
    // Not before a travel (role=server, the Progression hub): the level is going away.
    void HostMapReset(bool rematch) {
        if (!cfg_.mapReset || !MatchLevel() || !net_ || (rematch && (IsServer() || progression_))) return;
        MapResetMsg m;
        m.seq = ++mapResetSeq_;
        m.round = uint16_t(match_.round());
        m.flags = rematch ? kMapResetRematch : 0;
        net_->Broadcast(kChanReliable, Encode(m), true);
        mapGate_.Queue(m, NowMs());
    }
    void MapResetTick() {
        if (!mapGate_.pending()) return;
        MapResetMsg m;
        const bool busy = ownDeadPending_ || dropClearPending_ || !respawnQueue_.empty();
        if (!mapGate_.Poll(NowMs(), busy, m)) return;
        if (MatchLevel()) props_.MapReset(game::SteadyNowMs(), m.flags);
    }

    // The loose armour pieces here, keyed by object (LooseTimer ids).
    std::vector<uint64_t> LooseIds(std::unordered_map<uint64_t, UObject*>& byId) {
        std::vector<uint64_t> ids;
        for (UObject* a : adapter_.LooseArmor()) { const uint64_t id = uint64_t(reinterpret_cast<uintptr_t>(a)); ids.push_back(id); byId[id] = a; }
        return ids;
    }

    // Every machine, every frame: what clears the drops in this format (DropClearFor). The bot formats: weapons a
    // minute after they went FREE (the prop sync), loose armour a minute after it was first seen loose here. The timer's
    // baseline is taken when a match starts in this level, so armour the level had stays.
    void DropsTick() {
        const bool running = MatchLevel() && info_.phase != uint8_t(MatchPhase::Idle);
        if (running) props_.MarkBaseline();              // the map reset's baseline (the first call counts)
        const bool idle = running && DropClearFor(Format(info_.format)) == DropClear::AfterIdle;
        props_.SetIdleDespawn(idle);
        if (!running) { if (looseOn_) { looseTimer_.Reset(); looseOn_ = false; } return; }
        const uint32_t now = NowMs();
        if (looseOn_ && now < nextLooseSweep_) return;
        looseOn_ = true; nextLooseSweep_ = now + kLooseSweepMs;
        std::unordered_map<uint64_t, UObject*> byId;
        const std::vector<uint64_t> due = looseTimer_.Sweep(LooseIds(byId), double(now));
        if (!idle) return;                                                 // timed only, for the next clear
        for (uint64_t id : due) {
            if (++loosePurged_ <= 20)
                HSM_LOG("drops: loose armour %s despawned: a minute loose", ObjectName(byId[id]).c_str());
            adapter_.DestroyActor(byId[id]);
        }
    }

    // Hold G to give up a fought round or wave (GiveUpHold), or in a bot format's break to vote to skip it: the host's
    // own here, a client's by asking the host.
    void GiveUpTick() {
        if (!localFighter_) return;
        const uint32_t now = NowMs();
        bool down = false;
        const MatchInfo mi = Extrapolate(info_, infoAt_, now);     // F6: the clocks run on between the once-a-second infos
        UObject* own = adapter_.PlayerWillie(0);
        const bool alive = own && !adapter_.IsDead(own) && EntityAlive(MyEntity());
        // GiveUpActionFor (hsm_giveup_action.h): free-for-all, teams, Duel and the waves of Survival and Zombies: G ends
        // this life at any time, downed or not (the game's knocked-down flags were missed in the pre1 test); the death
        // path credits the last attacker. Progression co-op: the stock give-up screen is suppressed, so G gives the fight
        // up. The break between waves: G votes to skip it. F6: G while out of the fight used to be dropped silently.
        const GiveUpAction act = GiveUpActionFor(mi, alive, !own || adapter_.IsDead(own) || ownDeadPending_, oneFight_);
        const bool pvp = act == GiveUpAction::GiveUp;
        const bool skip = act == GiveUpAction::SkipBreak;
        const bool coop = act == GiveUpAction::GiveUpFight;
        const bool waiting = act == GiveUpAction::Waiting;
        if (MatchLevel() && act != GiveUpAction::None && !ui::Typing()) {
            if (GetAsyncKeyState('G') & 0x8000) {   // the key first: asking whose window is in front is the dearer call
                DWORD pid = 0;
                if (HWND w = GetForegroundWindow()) GetWindowThreadProcessId(w, &pid);
                down = pid == GetCurrentProcessId();
            }
        }
        if (down) ProbeGiveUpHeld(own, now, skip);
        if (down && waiting) {
            if (!giveUpWait_) HSM_LOG("match: G held, but there is nothing to give up (dead or waiting for the next round)");
            giveUpWait_ = true;
            giveUp_.Update(false, now);
            return;
        }
        giveUpWait_ = false;
        if (!giveUp_.Update(down, now)) return;
        const uint8_t me = MyEntity();
        HSM_LOG("match: G held %u ms: %s (#%u)", unsigned(GiveUpHold::kHoldMs),
                skip ? "voting to skip the break" : coop ? "giving up the fight"
                : IsBotFormat(Format(mi.format)) ? "giving up the wave" : "giving up", me);
        if (IsHost()) HandleGiveUp(me, now);
        else if (hostPeer_) { GiveUpMsg g; g.entity = me; net_->Send(hostPeer_, kChanReliable, Encode(g), true); }
        // The normal death path: ReportOwnDeath / DetectDeaths credit the attacker. Only where this machine simulates the
        // fighter: under host physics (or while another machine holds it) mine is a replica, and the host kills it.
        if (pvp && own && !HostPhysics() && !MyFighterHanded()) adapter_.TriggerDeath(own);
    }

    // Team side point for a (re)spawn, farthest from living enemies; each entity has its own spread slot around it
    // (SpreadSlot: humans on ring 1, bots further out), moved on to the next free one when a body is there.
    FVector SidePoint(int8_t team, uint8_t entity, double& yawDeg) {
        const std::vector<UObject*>& ignore = SpawnIgnore();
        if (!sidesValid_) {
            sidePoints_.clear();
            sidePoints_ = SpawnerPoints(ignore);
            if (sidePoints_.size() < 2) sidePoints_ = RingCandidates(kSideRingCm, ignore);   // no spawners: a ring around the host
            if (sidePoints_.size() < 2) sidePoints_ = RingCandidates(kNoSpawnerRingCm, ignore);   // a tighter one (a small yard)
            if (sidePoints_.size() < 2) {                  // nothing standable in view: around the host itself (StandableNear
                FVector c = hostWillie_ ? adapter_.ActorLocation(hostWillie_) : FVector{};
                if (IsServer()) { AnchorKind ak; if (!HostAnchor(c, ak)) c = FVector{}; }   // spreads with the wall checks);
                sidePoints_.assign(2, Point3{c.x, c.y, c.z});   // never an unchecked ring, which lands past fences
            }
            sides_ = PartitionSides(sidePoints_, std::max(TeamCount(match_.config().format), 1));
            sidesArea_ = AreaOf(sidePoints_);
            sidesValid_ = true;
        }
        std::vector<Point3> enemies;
        for (const RosterMember& m : roster_.members()) AddEnemy(enemies, m.entity, team, entity);
        for (auto& [e, ref] : bots_) AddEnemy(enemies, e, team, entity);
        const Point3& p = sidePoints_[PickRespawn(sidePoints_, sides_, team, enemies)];
        const Point3 spot = StandableNear(p, entity, entity, sidesArea_, ignore);
        FVector at = Grounded({spot.x, spot.y, spot.z}, ignore);
        double cx = 0, cy = 0;
        for (const Point3& q : sidePoints_) { cx += q.x; cy += q.y; }
        cx /= double(sidePoints_.size()); cy /= double(sidePoints_.size());
        yawDeg = std::atan2(cy - at.y, cx - at.x) * 57.29577951308232;   // face the middle
        return at;
    }

    void AddEnemy(std::vector<Point3>& out, uint8_t e, int8_t team, uint8_t self) {
        if (e == self || !match_.Alive(e) || (team != kNoTeam && match_.TeamOf(e) == team)) return;
        if (UObject* w = EntityActor(e)) { FVector v = adapter_.ActorLocation(w); out.push_back({v.x, v.y, v.z}); }
    }

    void RespawnEntity(uint8_t e) {
        if (e < 1 || e > kMaxFighters) return;            // bots come back as waves, not respawns
        uint8_t slot = uint8_t(e - 1);
        if (!roster_.BySlot(slot)) { match_.MarkRespawned(e); return; }
        int8_t team = match_.TeamOf(e);
        double yaw = 0;
        FVector at = SidePoint(team, e, yaw);
        game::Adapter::SpawnOptions o; o.at = at; o.yawDeg = yaw; o.teamInt = slot == 0 ? 1 : TeamIntFor(team, e);
        o.brain = slot == 0 || HostPhysics();
        uint32_t id = slot == 0 ? hostBlobs_.LatestFor(0) : director_.BlobFor(slot);
        o.blob = hostBlobs_.Get(id);
        UObject* w = nullptr;
        severs_.Forget(e);                           // a new body: its stuck blades, holds and severs start afresh
        claims_.Forget(e); fx_.Forget(e); equip_.Forget(e);
        if (leases_.Active(e)) WriteLease(e, kNoSlot, 0, 0, "respawn");   // a new body is its owner's: given back ahead of its Spawn
        if (slot == 0) { w = RespawnLocalNow(o); hostWillie_ = w; }
        else {
            DespawnFighter(slot);
            standIns_.Forget(e); gore_.Forget(e); grabs_.Forget(e); props_.ForgetEntity(e); lives_.erase(e);   // the new body's stream starts afresh
            w = SpawnFighterNow(o); fighters_[slot].actor.Set(w);
            if (w && !HostPhysics()) standIns_.Attach(e, w);
        }
        Spawn sp; sp.entity = e; sp.ownerSlot = slot; sp.kind = EntityKind::Fighter; sp.team = team; sp.blobId = o.blob ? id : 0; sp.life = lifeBook_.Bump(e);
        sp.x = float(at.x); sp.y = float(at.y); sp.z = float(at.z); sp.yaw = float(yaw);
        director_.MarkSpawned(sp);
        Despawn dp; dp.entity = e;
        net_->Broadcast(kChanReliable, Encode(dp), true);
        net_->Broadcast(kChanReliable, Encode(sp), true);
        match_.MarkRespawned(e); kills_.Forget(e);
        if (w) QueueTint(w, team); else HSM_LOG("host: respawn of entity %u failed", e);
    }

    // Owner sync: respawns wait here and go one per tick (about 60-120 ms each). A Duel round reset did them all in one
    // tick (two-player log 9: 410-470 ms). The match counts a queued fighter alive, but its old body is still the dead
    // one, so deaths, DeathReports and give-ups are ignored for it until its turn.
    bool RespawnQueued(uint8_t e) const { return std::find(respawnQueue_.begin(), respawnQueue_.end(), e) != respawnQueue_.end(); }

    void RespawnAll() {
        respawnQueue_.clear();
        for (const RosterMember& m : roster_.members()) RespawnEntity(m.entity);
        ClearBots();
        wave_ = 0;
    }

    void ClearBots() {
        for (auto& [e, ref] : bots_) {
            if (adapter_.StillValid(ref.obj, ref.index)) adapter_.DestroyActor(ref.obj);
            Despawn dp; dp.entity = e; net_->Broadcast(kChanReliable, Encode(dp), true);
            director_.Forget(e); match_.RemoveFighter(e); kills_.Forget(e); kills_.ForgetAttacker(e); severs_.Forget(e); gore_.Forget(e); claims_.Forget(e); fx_.Forget(e); equip_.Forget(e); grabs_.Forget(e); props_.ForgetEntity(e);
        }
        bots_.clear();
    }

    uint8_t FreeBotEntity() const {
        for (uint8_t e = kFirstBotEntity; e <= kLastBotEntity; ++e) if (!bots_.count(e)) return e;
        return 0;
    }

    void AnnounceBot(uint8_t e, UObject* w) {
        bots_[e].Set(w);
        std::vector<uint8_t> bytes;
        if (adapter_.ExportPassportOf(w, bytes)) StoreAndRelay(uint8_t(e - 1), std::move(bytes), 0);
        FVector at = adapter_.ActorLocation(w);
        Spawn sp; sp.entity = e; sp.ownerSlot = 0xFF; sp.kind = EntityKind::Bot; sp.team = kBotTeam; sp.blobId = hostBlobs_.LatestFor(uint8_t(e - 1)); sp.life = lifeBook_.Bump(uint8_t(e));
        sp.x = float(at.x); sp.y = float(at.y); sp.z = float(at.z); sp.yaw = float(adapter_.ActorYaw(w));
        director_.MarkSpawned(sp);
        net_->Broadcast(kChanReliable, Encode(sp), true);
        match_.AddFighter({e, kBotTeam, false});
        infoDirty_ = true;
    }

    // Stock NPCs: adopted as synced bots in players-vs-bots / Progression, removed otherwise.
    void ScanStockWillies() {
        int purged = 0, seen = 0, adopted = 0;
        for (UObject* w : adapter_.AllWillies()) {
            if (EntityOf(w)) continue;
            ++seen;
            // The Abyss's own spawner is held shut in a session (AbyssTick), so what it spawned before that goes too: the
            // zombies are the host's waves alone (adopting them kept the level's tier-sized pack on the host, over the cap).
            uint8_t e = adoptStock_ && !adapter_.IsDead(w) ? FreeBotEntity() : 0;
            if (e) { AnnounceBot(e, w); ++adopted; HSM_LOG("host: stock fighter adopted as bot entity %u", e); }
            else { adapter_.DestroyActor(w); ++purged; }
        }
        if (seen && (adopted || purged)) HSM_LOG("host: stock scan: %d stock fighter(s) seen, %d adopted, %d purged (adopt %s)", seen, adopted, purged, adoptStock_ ? "on" : "off");
        if (purged) HSM_LOG("host: removed %d unsynced stock fighter(s)", purged);
    }

    // The match decides how many bots and of which rank; entities of dead bots are reclaimed when none are free.
    void SpawnWaveBots(int count, int rank) {
        int spawned = 0;
        for (int i = 0; i < count; ++i) {
            uint8_t e = FreeBotEntity();
            if (!e) for (auto& [b, ref] : bots_) if (!match_.Alive(b)) { e = b; break; }
            if (!e) break;
            if (bots_.count(e)) RemoveBot(e);
            double yaw = 0;
            FVector at = SidePoint(kBotTeam, e, yaw);
            UObject* bot = match_.config().format == Format::Zombies ? adapter_.SpawnAbyssNpc(at) : nullptr;
            if (!bot) bot = adapter_.SpawnBot(rank, at, yaw, 2);
            if (!bot) break;
            AnnounceBot(e, bot); ++spawned;
        }
        if (spawned < count) { match_.SpawnFailed(count - spawned); HSM_LOG("host: spawned %d of %d bots", spawned, count); }
    }

    void RemoveBot(uint8_t e) {
        auto it = bots_.find(e);
        if (it == bots_.end()) return;
        adapter_.DestroyFighter(it->second.obj, it->second.index);   // its armour and weapons too (DestroyActor left them)
        looted_.erase(e);
        Despawn dp; dp.entity = e; net_->Broadcast(kChanReliable, Encode(dp), true);
        director_.Forget(e); match_.RemoveFighter(e); kills_.Forget(e); kills_.ForgetAttacker(e); severs_.Forget(e); gore_.Forget(e); claims_.Forget(e); fx_.Forget(e); equip_.Forget(e); grabs_.Forget(e); props_.ForgetEntity(e);
        bots_.erase(it);
        infoDirty_ = true;
    }

    std::vector<uint8_t> AllSlots() const {
        std::vector<uint8_t> s;
        for (const RosterMember& m : roster_.members()) s.push_back(m.slot);
        return s;
    }

    // Generated gear (RegearFor): Each rolls a fresh loadout per slot, Shared one loadout for all of them. StoreAndRelay
    // makes it the slot's blob for its next Spawn; the chunks precede that Spawn on the reliable channel.
    void RegearSlots(const MatchConfig& cfg, const std::vector<uint8_t>& slots, Regear mode, const char* why) {
        if (mode == Regear::None || slots.empty()) return;
        const double t0 = game::SteadyNowMs();
        int ahead = 0;
        auto next = [&] {                                // one made ahead in the gap if there is one, else made now
            std::vector<uint8_t> b = gearPool_.Take(cfg.tier);
            if (!b.empty()) { ++ahead; return b; }
            return game::RandomPassportBlob(adapter_, cfg.tier);
        };
        std::vector<uint8_t> shared;
        if (mode == Regear::Shared) shared = next();
        int generated = mode == Regear::Shared && !shared.empty() ? 1 : 0, stored = 0;
        for (uint8_t slot : slots) {
            std::vector<uint8_t> blob = mode == Regear::Shared ? shared : next();
            if (blob.empty()) { HSM_LOG("host: no generated gear for slot %u; it keeps its loadout", slot); continue; }
            if (mode == Regear::Each) {
                ++generated;
                HSM_LOG("host: entity %u respawns with a new tier-%u loadout", unsigned(Roster::EntityForSlot(slot)), cfg.tier);
            }
            StoreAndRelay(slot, std::move(blob), 0); ++stored;
            if (slot == 0) NewGearNotice(cfg.tier);
        }
        if (mode == Regear::Shared && stored) {
            mirrorBlob_ = shared;
            if (match_.round()) HSM_LOG("host: round %d: new loadout for everyone (tier %u)", match_.round(), cfg.tier);
            else HSM_LOG("host: %s: new loadout for everyone (tier %u)", why, cfg.tier);
        }
        HSM_LOG("host: %d loadout(s) generated in %.1f ms (%s; %d made ahead)", generated, game::SteadyNowMs() - t0, why, ahead);
    }

    // Owner sync, before the match starts: its generated start loadouts, made one at a time while the players' fighters
    // come in. Made at the start they stalled that frame along with everyone's respawn (two-player log 9: 208 ms of a
    // 456 ms frame); RegearSlots takes them from the pool. Any not made by then are made there, as before.
    void MakeStartGearAhead(uint32_t now) {
        if (now < nextGearAhead_) return;
        const MatchConfig cfg = StartConfig();
        const size_t want = StartLoadoutsAhead(cfg, roster_.size());
        if (!want || !gearPool_.Wants(cfg.tier, want)) return;
        std::vector<uint8_t> blob = game::RandomPassportBlob(adapter_, cfg.tier);
        nextGearAhead_ = now + (blob.empty() ? 1000 : 250);
        if (blob.empty()) return;
        gearPool_.Put(cfg.tier, std::move(blob));
        HSM_LOG("host: start loadout %zu of %zu made ahead (tier %u)", gearPool_.size(), want, unsigned(cfg.tier));
    }

    // The coming respawns' generated loadouts, made one at a time while nobody fights: each stalls the frame about
    // 110 ms, and made together at the round reset they stalled it 230-260 ms just as the round began (test 13).
    // The first waits a second into the gap (not on the deciding blow), the next a quarter second after it.
    void MakeGearAhead(uint32_t now) {
        const uint8_t tier = match_.config().tier;
        const size_t want = match_.LoadoutsAhead();
        if (!want) { nextGearAhead_ = now + 1000; return; }
        if (now < nextGearAhead_ || !gearPool_.Wants(tier, want)) return;
        std::vector<uint8_t> blob = game::RandomPassportBlob(adapter_, tier);
        nextGearAhead_ = now + (blob.empty() ? 1000 : 250);
        if (blob.empty()) return;
        gearPool_.Put(tier, std::move(blob));
        HSM_LOG("host: loadout %zu of %zu made ahead (tier %u)", gearPool_.size(), want, unsigned(tier));
    }

    // A fighter joining a running match takes its generated gear: the current shared loadout, or a fresh one.
    void JoinerGear(uint8_t slot) {
        const MatchConfig& c = match_.config();
        if (RegearFor(c, RespawnCause::Rematch) == Regear::Shared && !mirrorBlob_.empty()) {
            StoreAndRelay(slot, mirrorBlob_, 0);
            HSM_LOG("host: entity %u joins with the match's shared loadout", unsigned(Roster::EntityForSlot(slot)));
        } else RegearSlots(c, {slot}, IsBotFormat(c.format) ? JoinRegearFor(c) : RegearFor(c, RespawnCause::Individual), "join");
    }

    uint16_t PingOfSlot(uint8_t slot) {
        if (!net_ || slot == 0) return 0;
        for (const auto& [peer, p] : peers_) if (p.slot == slot) return uint16_t(std::min<uint32_t>(net_->RoundTripMs(peer), 65535u));
        return 0;
    }

    void BroadcastInfo(uint32_t now) {
        infoDirty_ = false; nextInfo_ = now + kInfoIntervalMs;
        MatchInfo m;
        const MatchConfig& c = match_.config();
        m.format = uint8_t(c.format); m.scoreLimit = c.scoreLimit; m.timeLimitS = c.timeLimitS; m.respawnDelayS = c.respawnDelayS;
        m.friendlyFire = c.friendlyFire; m.phase = uint8_t(match_.phase()); m.timeLeftMs = match_.TimeLeftMs(now);
        m.winner = int16_t(match_.winner());
        m.gear = uint8_t(c.gear); m.tier = c.tier;
        m.forceStrength = cfg_.forceStrength ? 1 : 0;    // rev 34: every machine builds its players the way the host does
        m.round = uint16_t(match_.round()); m.roundGapLeftMs = match_.RoundGapLeftMs(now); m.roundWinner = match_.roundWinner();
        m.damagePct = SessionDamage();
        m.physics = game::OwnPhysics();   // rev 31: the host's Gore Rate, Bone Snapping and Dead Weight Scale
        m.waveBreakS = c.waveBreakS; m.clearGroundMin = c.clearGroundMin; m.clearCount = clearCount_;
        m.syncMode = uint8_t(nextSync_);   // rev 29: the mode the next map runs
        if (IsBotFormat(c.format)) {                     // the break between waves in the Duel gap's fields (InWaveBreak)
            m.roundGapLeftMs = match_.BreakLeftMs(now);
            m.roundWinner = uint8_t(std::min<size_t>(match_.SkipVotes(), 255));
        }
        for (const auto& [key, score] : match_.scores()) if (m.scores.size() < kMaxWillies) m.scores.push_back({int16_t(key), int16_t(score)});
        for (const RosterMember& r : roster_.members())
            m.fighters.push_back({r.entity, match_.TeamOf(r.entity), int16_t(match_.Kills(r.entity)), int16_t(match_.Deaths(r.entity)), r.name,
                                  PingOfSlot(r.slot), match_.Alive(r.entity), DownInRound(r.entity), ScoreId(r.entity)});
        for (auto& [e, ref] : bots_)
            if (m.fighters.size() < kMaxWillies)
                m.fighters.push_back({e, kBotTeam, int16_t(match_.Kills(e)), int16_t(match_.Deaths(e)), "Bot", 0, match_.Alive(e), false});
        net_->Broadcast(kChanReliable, Encode(m), true);
        info_ = std::move(m); infoAt_ = now;
        banner_.Observe(info_, MyEntity(), now);
    }

    // ------------------------------------------------------------------ match (both)
    uint8_t MyEntity() const { return IsHost() ? HostEntity() : world_.myEntity(); }
    void PushFeed(const Kill& k) {
        feed_.push_back({NowMs(), MakeFeedRow(info_, k, MyEntity())});
        while (feed_.size() > kFeedRows) feed_.pop_front();
    }
    // This machine's fighter comes back in generated gear.
    void NewGearNotice(uint8_t tier) { banner_.Fire("NEW GEAR", "TIER " + std::to_string(tier), style::kWarn, NowMs()); }

    void QueueTint(UObject* w, int8_t team) {
        if (team == kNoTeam || !w) return;
        Tint t; t.actor.Set(w); t.team = team; t.next = NowMs() + kTintRetryMs; t.tries = kTintTries;
        tints_.push_back(t);
    }

    // Armor pieces attach over the first moments after a spawn; re-apply a few times, then stop.
    void TintTick() {
        uint32_t now = NowMs();
        for (Tint& t : tints_) {
            if (t.tries <= 0 || now < t.next) continue;
            t.next = now + kTintRetryMs; --t.tries;
            if (!adapter_.StillValid(t.actor.obj, t.actor.index)) { t.tries = 0; continue; }
            Rgba c = TeamColor(t.team);
            adapter_.TintArmor(t.actor.obj, c.r, c.g, c.b);
        }
        std::erase_if(tints_, [](const Tint& t) { return t.tries <= 0; });
    }

    HudScene BuildHud() {
        if (!localFighter_ || !MatchLevel() || info_.phase == uint8_t(MatchPhase::Idle)) return {};
        uint32_t now = NowMs();
        UObject* own = adapter_.PlayerWillie(0);
        bool dead = own && adapter_.IsDead(own);
        if (dead && !ownDiedAt_) ownDiedAt_ = now;
        if (!dead && !ownDeadPending_) ownDiedAt_ = 0;
        LocalHud l;
        l.me = MyEntity();
        l.host = hostHasFighter_ ? Roster::EntityForSlot(kHostSlot) : uint8_t(0);   // no HOST badge when the host has no fighter
        l.infoAtMs = infoAt_;            // MatchInfo comes once a second; the scene runs its clocks on between
        l.dead = ownDiedAt_ != 0; l.diedAtMs = ownDiedAt_;
        l.tab = ui::HudTabHeld();
        l.oneFight = oneFight_;
        if (const MapInfo* map = FindMap(currentLevel_)) l.map = map->display;
        l.banner = banner_.shot();
        l.feed.assign(feed_.begin(), feed_.end());
        l.giveUp = giveUp_.progress();
        l.giveUpWait = giveUpWait_;
        return BuildHudScene(info_, l, now);
    }

    // Name tags: the other living fighters whose heads we can place. The HUD projects and draws them; empty when off.
    std::vector<NameTagSrc> NameTags(const HudScene& hud) {
        std::vector<NameTagSrc> out;
        if (!cfg_.nameTags || !hud.visible || !MatchLevel()) return out;
        const uint8_t me = MyEntity();
        const uint32_t now = NowMs();
        tagSight_.Begin(now);
        FVector eye; int eyeKnown = -1;   // the camera, asked for once a frame and only when a trace is due
        for (const FighterEntry& f : info_.fighters) {
            if (f.entity == me || !f.alive || f.name.empty() || out.size() >= kMaxWillies) continue;
            UObject* actor = IsHost() ? EntityActor(f.entity) : ClientActor(f.entity);
            FVector head;
            if (!actor || !adapter_.BoneLocation(actor, "head", head)) continue;
            if (tagSight_.Due(f.entity)) {
                if (eyeKnown < 0) eyeKnown = adapter_.CameraLocation(eye) ? 1 : 0;
                // Our own body and the target's, with their weapons and armour, must not hide the tag. No camera: shown.
                bool clear = true;
                if (eyeKnown) {
                    std::vector<UObject*> skip = adapter_.CarriedActors(actor);
                    skip.push_back(actor);
                    if (UObject* own = adapter_.PlayerWillie(0)) { for (UObject* c : adapter_.CarriedActors(own)) skip.push_back(c); skip.push_back(own); }
                    clear = adapter_.LineClear(eye, {head.x, head.y, head.z + 10.0}, skip);
                }
                tagSight_.Report(f.entity, clear);
                CountTagTrace(now);
            }
            NameTagSrc t;
            t.name = voice_.Talking(f.entity, now) ? "> " + f.name : f.name;   // talking (proximity voice)
            t.wx = head.x; t.wy = head.y; t.wz = head.z + 25.0; t.team = f.team; t.alive = f.alive;
            t.seen = tagSight_.Seen(f.entity);
            out.push_back(std::move(t));
        }
        return out;
    }

    // ------------------------------------------------------------------ voice chat (gameplay/voice.h, voice/hsm_voice.h)
    // 0.7.1: voice is each player's own setting (voice + voice_reach: Off / Proximity / Everyone; voice_cut), the host's
    // too, and the host relays every client's voice whatever its own setting. Each player's microphone (push to talk by
    // default, open, or off), device and volume are their own as well. A client sends its chunks to the host, which
    // relays each one to the machines that may hear it (near the speaker, everyone living, or dead with the dead); the
    // chunk's flags carry the speaker's reach and whether it keeps talking when cut off. A listener hears a speaker at
    // the narrower of the two reaches (PairReach). A fighter below voice_cutoff of full consciousness, knocked out or
    // dead is cut off from the living, unless its own voice_cut=0: by its own machine, by the host and by each
    // listener. With the mesh on, voice still goes through the host. A role=server host relays only: it records and
    // plays nothing.
    voice::Reach MyReach() const { return cfg_.voice ? cfg_.voiceReach : voice::Reach::Off; }
    bool VoiceOn() const { return net_ && MyReach() != voice::Reach::Off; }
    static voice::Reach FlagsReach(uint8_t flags) { return (flags & kVoiceEveryone) ? voice::Reach::Everyone : voice::Reach::Proximity; }
    voice::Range VoiceRange() const { return {cfg_.voiceNearM * 100.f, cfg_.voiceFarM * 100.f}; }

    // Alive: in the match list, not marked dead there or (when known) by its vitals. Not listed (waiting, spectating):
    // counts as dead, so it hears the dead and the living, and talks only to the dead.
    bool VoiceAlive(uint8_t e) const {
        if (e == 0 || e >= voice::kMaxSpeakers) return false;
        for (const FighterEntry& f : info_.fighters)
            if (f.entity == e) return f.alive && !(voiceState_[e].vitalsAt && voiceState_[e].dead);
        return false;
    }
    // Conscious enough to talk. Nothing known yet: an owner-sync fighter waits for its first vitals; with host sync a
    // client trusts the host's relay (the host reads the real bodies).
    bool VoiceAwake(uint8_t e) const {
        if (e == 0 || e >= voice::kMaxSpeakers) return false;
        const VoiceState& s = voiceState_[e];
        if (!s.vitalsAt) return false;
        return s.gate.ok();
    }
    void VoiceVitals(const VitalsMsg& v) {
        if (v.entity == 0 || v.entity >= voice::kMaxSpeakers) return;
        VoiceState& s = voiceState_[v.entity];
        s.vitalsAt = std::max(NowMs(), 1u);
        s.dead = (v.flags & 1) != 0;
        const bool was = s.gate.ok();
        s.gate.Update(v.consciousness, (v.flags & 5) != 0, cfg_.voiceCutoff);   // dead or knocked out: cut off
        if (s.gate.ok() != was && voiceCutLogs_ < 200) {
            ++voiceCutLogs_;
            HSM_LOG("voice: entity %u %s (consciousness %.1f, full %.1f%s)", unsigned(v.entity), s.gate.ok() ? "may talk again" : "cut off",
                    double(v.consciousness), double(s.gate.peak()), s.dead ? ", dead" : (v.flags & 4) ? ", knocked out" : "");
        }
    }
    void VoiceForget(uint8_t e) {
        if (e == 0 || e >= voice::kMaxSpeakers) return;
        voice_.Forget(e);
        voiceState_[e] = VoiceState{};
    }
    bool VoiceHead(uint8_t e, FVector& out) {
        UObject* a = IsHost() ? EntityActor(e) : ClientActor(e);
        return a && adapter_.BoneLocation(a, "head", out);
    }
    // The game window in front and nobody typing: push to talk may read its key.
    static bool VoiceKeyAllowed() {
        DWORD pid = 0;
        if (HWND w = GetForegroundWindow()) GetWindowThreadProcessId(w, &pid);
        return pid == GetCurrentProcessId() && !ui::Typing();
    }
    game::VoiceSettings VoiceSettingsFromConfig() const {
        game::VoiceSettings s;
        s.mic = cfg_.voiceMic; s.levelDb = cfg_.voiceLevelDb; s.key = cfg_.voiceKey; s.volume = cfg_.voiceVolume; s.device = cfg_.voiceDevice;
        return s;
    }
    bool VoiceMeterWanted(uint32_t now) const { return voiceMeterUntil_ && int32_t(voiceMeterUntil_ - now) > 0; }

    // No session: a menu's microphone meter and HEAR MYSELF still record; nothing is sent.
    void VoiceIdleTick() {
        const uint32_t now = NowMs();
        if (!voiceConfigured_) { voiceConfigured_ = true; voice_.Configure(VoiceSettingsFromConfig()); }
        const bool meter = VoiceMeterWanted(now);
        if (!meter && !voice_.Recording()) { if (voice_.Monitor()) voice_.SetMonitor(false); return; }
        if (meter && !steam::Ready() && !voiceSteamTried_) { voiceSteamTried_ = true; steam::Init(); }
        voice_.Capture(false, VoiceKeyAllowed(), meter, now, voiceOut_);
        if (!meter) { voice_.Stop(); voice_.SetMonitor(false); }
    }

    // Each frame, after the host or client tick.
    void VoiceTick() {
        if (!net_ || !localFighter_) return;   // role=server: VoiceRelay only, nothing recorded or played
        const uint32_t now = NowMs();
        const bool on = VoiceOn();
        if (!voiceConfigured_) { voiceConfigured_ = true; voice_.Configure(VoiceSettingsFromConfig()); }
        if ((on || VoiceMeterWanted(now)) && !steam::Ready() && !voiceSteamTried_) {   // Steam records the default microphone and decodes
            voiceSteamTried_ = true;
            if (!steam::Init()) HSM_LOG("voice: Steam is not running here; the microphone is recorded by the mod, Steam-compressed voices cannot be played");
        }
        const uint8_t me = MyEntity();
        if (on && now >= voiceVitalsAt_) {   // 10 Hz: my own fighter's consciousness, read here
            voiceVitalsAt_ = now + 100;
            if (me)
                if (UObject* own = adapter_.PlayerWillie(0)) { VitalsMsg v; if (adapter_.ReadVitals(own, me, v)) { v.entity = me; VoiceVitals(v); } }
        }
        // The dead talk (to the dead); the living only while conscious enough (or whatever, with voice_cut=0).
        const bool mayTalk = on && me && InFightLevel() && (!VoiceAlive(me) || VoiceAwake(me) || !cfg_.voiceCut);
        voice_.Capture(mayTalk, VoiceKeyAllowed(), VoiceMeterWanted(now), now, voiceOut_);
        for (size_t i = 0; i < voiceOut_.chunks.size(); ++i) {
            if (voiceOut_.chunks[i].size() > kMaxVoiceBytes) { ++voiceDrops_.big; continue; }   // a long hitch: dropped, never cut
            VoiceMsg m; m.entity = me; m.seq = ++voiceSeq_;
            m.flags = uint8_t(((i == 0 && voiceOut_.spurt) ? kVoiceSpurt : 0) | (voiceOut_.adpcm ? kVoiceAdpcm : 0) |
                                  (MyReach() == voice::Reach::Everyone ? kVoiceEveryone : 0) | (cfg_.voiceCut ? 0 : kVoiceNoCut));
            m.data = std::move(voiceOut_.chunks[i]);
            const std::vector<uint8_t> bytes = Encode(m);
            if (IsHost()) VoiceRelay(m, bytes);
            else if (hostPeer_) net_->Send(hostPeer_, kChanUnreliable, bytes, false);
        }
        if (on) VoiceGains(now);
    }

    // Host: a client's chunk, for its own fighter only and at most VoiceRate, then on to whoever may hear it.
    void VoiceFromClient(const NetEvent& ev, const Peer& peer) {
        VoiceMsg m;
        if (!Decode(ev.data.data(), ev.data.size(), m)) { ++voiceDrops_.off; return; }
        if (m.entity != Roster::EntityForSlot(peer.slot) || m.entity >= voice::kMaxSpeakers) { ++voiceDrops_.sender; return; }
        if (!voiceState_[m.entity].rate.Allow(m.data.size(), NowMs())) { ++voiceDrops_.rate; return; }
        VoiceRelay(m, ev.data);
    }

    // Host: one chunk of speaker m.entity (a client's or the host's own) to every machine that may hear it, and to the
    // host's own player, by the speaker's own reach (its Voice flags). Proximity: a living listener gets it only within
    // voice_far_m (and a margin); a dead one wherever it is. Everyone: every living listener. Each listener's machine
    // then applies its own reach too.
    void VoiceRelay(const VoiceMsg& m, const std::vector<uint8_t>& bytes) {
        const uint8_t from = m.entity;
        const voice::Reach reach = FlagsReach(m.flags);
        const bool alive = VoiceAlive(from), awake = VoiceAwake(from) || (m.flags & kVoiceNoCut);
        if (alive && !awake) { ++voiceDrops_.cut; return; }
        FVector at{};
        const bool placed = reach == voice::Reach::Proximity && VoiceHead(from, at);
        for (const auto& [peerId, p] : peers_) {
            const uint8_t e = Roster::EntityForSlot(p.slot);
            if (e == from) continue;
            const bool listenerAlive = VoiceAlive(e);
            const voice::Hear h = voice::AudibleIn(reach, alive, awake, listenerAlive);
            if (h == voice::Hear::None) { ++voiceDrops_.deaf; continue; }
            float dist = -1.f;
            FVector to{};
            if (placed && h == voice::Hear::Spatial && listenerAlive && VoiceHead(e, to)) {
                const double dx = at.x - to.x, dy = at.y - to.y, dz = at.z - to.z;
                dist = float(std::sqrt(dx * dx + dy * dy + dz * dz));
            }
            if (!voice::RelayVoice(h, dist, listenerAlive, VoiceRange())) { ++voiceDrops_.distant; continue; }
            net_->Send(peerId, kChanUnreliable, bytes, false);
            ++voiceRelayed_;
        }
        if (localFighter_ && from != HostEntity()) VoiceIn(m);
    }

    // A chunk this machine may play: checked again as this machine sees the speaker and itself, then decoded.
    void VoiceIn(const VoiceMsg& m) {
        if (!localFighter_ || !VoiceOn()) { ++voiceDrops_.off; return; }
        const uint8_t me = MyEntity();
        if (m.entity == 0 || m.entity > kMaxFighters || m.entity == me) { ++voiceDrops_.sender; return; }
        if (!IsHost() && !voiceState_[m.entity].rate.Allow(m.data.size(), NowMs())) { ++voiceDrops_.rate; return; }
        VoiceState& st = voiceState_[m.entity];
        st.reach = FlagsReach(m.flags); st.noCut = (m.flags & kVoiceNoCut) != 0;
        const bool alive = VoiceAlive(m.entity);
        const voice::Hear h = voice::AudibleIn(voice::PairReach(st.reach, MyReach()), alive, VoiceAwake(m.entity) || st.noCut, VoiceAlive(me));
        if (h == voice::Hear::None) { ++(alive ? voiceDrops_.cut : voiceDrops_.deaf); return; }
        voice_.Receive(m.entity, m.seq, (m.flags & kVoiceAdpcm) != 0, m.data.data(), m.data.size(), NowMs());
    }

    // This player's own mute and volume for a speaker, by name (kept while the game runs).
    // By the same name the menu's SetPlayerListen keyed it under (NameOfSlot), so a rejoin keeps the setting.
    voice::Listen ListenFor(uint8_t entity) const {
        if (listen_.empty() || entity < 1 || entity > kMaxFighters) return {};
        const auto it = listen_.find(NameOfSlot(uint8_t(entity - 1)));
        return it == listen_.end() ? voice::Listen{} : it->second;
    }

    // Each frame: every speaker heard in the last second gets its left/right gain from this machine's camera (the
    // living hear the living by distance and side, or all at full volume under Everyone; the dead hear the dead at full
    // volume; a cut-off speaker goes silent), times this player's own volume for that speaker (0 when muted).
    void VoiceGains(uint32_t now) {
        FVector eye{};
        bool haveEye = adapter_.CameraLocation(eye);
        if (!haveEye) if (UObject* own = adapter_.PlayerWillie(0)) haveEye = adapter_.BoneLocation(own, "head", eye);
        float yaw = 0, pitch = 0;
        adapter_.ControlRotation(0, yaw, pitch);
        const uint8_t me = MyEntity();
        const bool meAlive = VoiceAlive(me);
        for (uint8_t e = 1; e <= kMaxFighters; ++e) {
            if (e == me || !voice_.Talking(e, now, 1000)) continue;
            voice::Gains g{0.f, 0.f};
            switch (voice::AudibleIn(voice::PairReach(voiceState_[e].reach, MyReach()), VoiceAlive(e), VoiceAwake(e) || voiceState_[e].noCut, meAlive)) {
                case voice::Hear::Flat: g = {1.f, 1.f}; break;
                case voice::Hear::Spatial: {
                    FVector at{};
                    if (!VoiceHead(e, at)) break;                    // not placed here: silent, as if out of range
                    if (!haveEye) { g = {1.f, 1.f}; break; }        // no camera: heard, unplaced
                    g = voice::Spatialize({float(eye.x), float(eye.y), float(eye.z)}, yaw, {float(at.x), float(at.y), float(at.z)}, VoiceRange());
                    break;
                }
                case voice::Hear::None: break;
            }
            const float mine = voice::ListenGain(ListenFor(e));
            g.left *= mine; g.right *= mine;
            voice_.SetGains(e, g);
        }
    }

    // With the health line, when anything happened: what was sent, heard, relayed and dropped, and why.
    void VoiceHealth() {
        const game::Voice::Stats s = voice_.TakeStats();
        const VoiceDrops d = voiceDrops_;
        voiceDrops_ = VoiceDrops{};
        const uint32_t relayed = voiceRelayed_;
        voiceRelayed_ = 0;
        const uint32_t dropped = d.cut + d.distant + d.deaf + d.rate + d.sender + d.big + d.off + s.late + s.badChunks;
        if (!s.sent && !s.received && !relayed && !dropped && !s.recording) return;
        std::string full;
        for (const FighterEntry& f : info_.fighters) {
            if (f.entity == 0 || f.entity >= voice::kMaxSpeakers || !voiceState_[f.entity].vitalsAt) continue;
            char b[48];
            std::snprintf(b, sizeof b, " e%u=%.0f%s", unsigned(f.entity), double(voiceState_[f.entity].gate.peak()), voiceState_[f.entity].gate.ok() ? "" : "(cut)");
            full += b;
        }
        HSM_LOG("voice: %s, mic %s (%s, %s), sent %u chunks %.1f KB (gate open %u ms), heard %u chunks %.1f KB, relayed %u; dropped: cut off %u, "
                "out of range %u, not for this listener %u, rate %u, wrong sender %u, too big %u, voice off %u, late %u, undecodable %u; "
                "mixer dropped %llu samples; output %s; full consciousness%s",
                voice::ReachIni(MyReach()), s.recording ? "on" : "off", voice::MicIni(cfg_.voiceMic), s.adpcm ? "own recording" : "Steam",
                s.sent, s.sentBytes / 1024.0, s.gateOpenMs, s.received, s.receivedBytes / 1024.0, relayed,
                d.cut, d.distant, d.deaf, d.rate, d.sender, d.big, d.off, s.late, s.badChunks, (unsigned long long)s.mixerDropped,
                s.device ? "open" : "closed", full.empty() ? " unknown" : full.c_str());
    }

    // One log line, after the first second of name tag traces: the cost of the wall test.
    void CountTagTrace(uint32_t now) {
        if (tagTraceLogged_) return;
        if (!tagTraces_) tagTraceSince_ = now;
        ++tagTraces_;
        if (now - tagTraceSince_ < 1000) return;
        tagTraceLogged_ = true;
        HSM_LOG("tags: wall test does %.1f traces/s for %zu fighter(s) (at most %d a frame, %u ms apart per fighter)",
                tagTraces_ * 1000.0 / double(now - tagTraceSince_), tagSight_.Tracked(), kOcclusionPerFrame, kOcclusionIntervalMs);
    }

    // Dead and waiting: watch living fighters (teammates first); Left/Right arrows cycle.
    void SpectateTick() {
        UObject* own = adapter_.PlayerWillie(0);
        // ownDeadPending_ also marks "my fighter has not arrived yet" at a level start: only a death counts (V3).
        bool watching = MatchLevel() && info_.phase != uint8_t(MatchPhase::Idle) && info_.phase != uint8_t(MatchPhase::Results)
                        && (!own || adapter_.IsDead(own) || (ownDeadPending_ && (ownDiedAt_ != 0 || HostCountsMeDown())));
        if (!watching) {
            if (spectating_) {
                // A failed reset keeps the state so the next tick tries again (V4).
                if (game::ResetViewToPawn(adapter_) || !own) {
                    spectating_ = 0; spectateActor_ = nullptr; spectateIndex_ = -1;
                    game::EndFollowCamera(adapter_);
                    game::RestoreDeathOverlays(adapter_);
                }
            }
            return;
        }
        uint8_t me = IsHost() ? HostEntity() : world_.myEntity();
        int8_t myTeam = kNoTeam;
        std::vector<SpectateCycler::Candidate> cands;
        for (const FighterEntry& f : info_.fighters) {
            if (f.entity == me) { myTeam = f.team; continue; }
            if (NoSuchFighter(f.entity)) continue;   // the host has none to watch
            cands.push_back({f.entity, f.team, EntityAlive(f.entity)});
        }
        spectate_.Refresh(me, myTeam, cands);
        bool right = GetAsyncKeyState(VK_RIGHT) & 0x8000, left = GetAsyncKeyState(VK_LEFT) & 0x8000;
        if ((right || left) && (ui::Typing() || adapter_.PauseMenuOpen())) right = left = false;   // arrows belong to the chat / menu
        if (right || left) {   // the keys first: asking whose window is in front costs more
            DWORD pid = 0;
            if (HWND w = GetForegroundWindow()) GetWindowThreadProcessId(w, &pid);
            if (pid != GetCurrentProcessId()) right = left = false;
        }
        if (right && !spectateKeyDown_) spectate_.Next();
        if (left && !spectateKeyDown_) spectate_.Prev();
        spectateKeyDown_ = right || left;
        if (spectateActor_ && !adapter_.StillValid(spectateActor_, spectateIndex_)) { spectateActor_ = nullptr; spectateIndex_ = -1; }   // V2
        std::optional<uint8_t> target = spectate_.Target();
        UObject* actor = target ? (IsHost() ? EntityActor(*target) : ClientActor(*target)) : nullptr;
        const uint32_t now = NowMs();
        // Our follow camera, moved every frame, is the view target: re-applied on a new fighter and every 500 ms, since
        // the game resets the camera on its own events (V2).
        const bool fresh = actor != spectateActor_;
        UObject* cam = actor ? game::FollowCamera(adapter_, actor, fresh, float(uint32_t(now - spectateMovedAt_)) / 1000.0f) : nullptr;
        spectateMovedAt_ = now;
        if (cam && ShouldApplyView(fresh ? -1 : int(*target), int(spectating_), spectateApplyAt_, now)
            && game::SetViewTarget(adapter_, cam, 0.0f)) {
            const bool started = !spectating_;
            spectating_ = *target; spectateActor_ = actor; spectateIndex_ = ObjectIndex(actor); spectateApplyAt_ = now;
            game::ClearCameraFade(adapter_);   // with the view target: the game's death fade would black it out (#8, #10)
            const bool logScreens = started && spectateScreensLogged_ < 10;
            if (logScreens) {
                ++spectateScreensLogged_;
                HSM_LOG("spectate: watching entity %u; widgets on screen: %s", unsigned(*target), game::WidgetsOnScreen().c_str());
            }
            if (started || now - spectateOverlaysAt_ >= 2000) {   // a scan of every object: not each 500 ms apply
                spectateOverlaysAt_ = now;
                game::HideDeathOverlays(own, logScreens);
            }
        }
    }

    // K1/K2: a rejoiner the host holds down until the next round has no fighter announced yet (ownDeadPending_ stays set) and
    // is listed dead: it watches the others meanwhile.
    bool HostCountsMeDown() const {
        if (IsHost() || !world_.myEntity()) return false;
        for (const FighterEntry& f : info_.fighters) if (f.entity == world_.myEntity()) return !f.alive;
        return false;
    }

    bool EntityAlive(uint8_t e) {
        if (IsHost()) return HostAlive(match_.Alive(e), RespawnQueued(e));   // V6: a queued respawn's old body is dead
        // Clients: the host's roster says who lives (V1); the stand-in / snapshot guesses lagged and showed corpses.
        for (const FighterEntry& f : info_.fighters) if (f.entity == e) return ClientAlive(&f.alive);
        return ClientAlive(nullptr);
    }

    // ------------------------------------------------------------------ client
    void OnOwnSpawn(const Spawn& sp) {
        if (!levelGate_.Open()) return;   // the level would wipe it; ClientGateOpen applies it later
        if (!ownDeadPending_) { if (UObject* me = adapter_.PlayerWillie(0)) QueueTint(me, sp.team); return; }
        const bool retry = ownRespawn_.Active();       // a later attempt: the first one did the one-time work
        ownRespawn_.Begin();
        const std::vector<uint8_t>* assigned = sp.blobId ? clientBlobs_.Get(sp.blobId) : nullptr;   // e.g. generated gear
        bool generated = info_.gear != uint8_t(Gear::Own) && info_.gear != uint8_t(Gear::Fists);   // own gear (and Brawl, stripped at the spawn): the Spawn names this client's upload, never relayed back
        if (generated && sp.blobId && !assigned)
            HSM_LOG("client: own Spawn names loadout %08x, which has not arrived; respawning in the uploaded loadout", sp.blobId);
        if (!retry && generated && assigned && ownBlob_ && sp.blobId != ownBlob_) NewGearNotice(info_.tier);
        ownBlob_ = sp.blobId;
        // Own gear: my upload, even when the Spawn still names a blob I have (a generated one from the last match, #62).
        game::Adapter::SpawnOptions o; o.blob = generated && assigned ? assigned : !lastUpload_.empty() ? &lastUpload_ : assigned;
        o.at = {sp.x, sp.y, sp.z}; o.yawDeg = sp.yaw; o.teamInt = 1;
        severs_.Forget(world_.myEntity());          // a new body: its stuck blades and holds start afresh
        claims_.Forget(world_.myEntity()); fx_.Forget(world_.myEntity()); equip_.Forget(world_.myEntity());
        UObject* w = RespawnLocalNow(o);
        if (!w) {   // F1: no body (no controller, or the spawn failed): the dead one stays possessed, so try again
            const bool more = ownRespawn_.Failed(NowMs());
            HSM_LOG("client: own respawn attempt %d/%d failed%s", ownRespawn_.attempts(), RespawnRetry::kMaxAttempts,
                    more ? "; retrying in 1 s" : "; giving up until the next round");
            if (!more) ownDeadPending_ = false;
            return;
        }
        if (retry) HSM_LOG("client: own respawn succeeded on attempt %d", ownRespawn_.attempts() + 1);
        ownRespawn_.Reset();
        ownDeadPending_ = false;
        if (dropClearPending_) dropClearDue_ = NowMs() + kDropClearDelayMs;   // F5: let the new body's armour attach first
        bodyWork_ = true;
        snappedOwn_ = false;                         // input hooks are per function and follow myWillie_
        QueueTint(w, sp.team);
    }

    // F1: the own respawn that returned no body is tried again.
    void RetryOwnRespawn() {
        if (!ownDeadPending_) { ownRespawn_.Reset(); return; }
        if (!ownRespawn_.Due(NowMs())) return;
        if (const Spawn* sp = world_.Find(world_.myEntity())) OnOwnSpawn(*sp);
    }

    // Everything that is not this player's fighter or a puppet of the host's world is local-only: remove it.
    void PurgeForeignClient() {
        UObject* me = adapter_.PlayerWillie(0);
        int purged = 0;
        for (UObject* w : adapter_.AllWillies()) {
            if (w == me) continue;
            bool puppet = false;
            for (auto& [e, ref] : puppets_) puppet = puppet || ref.obj == w;
            if (!puppet) { adapter_.DestroyActor(w); ++purged; }
        }
        if (purged) HSM_LOG("client: removed %d local-only fighter(s)", purged);
    }

    void SendHello() {
        Hello h; h.buildKey = BuildKey(); h.name = MyName(); h.modBuild = game::ModBuild();
        net_->Send(hostPeer_, kChanReliable, Encode(h), true);
        nextHello_ = NowMs() + 2000;
        HSM_LOG("client: hello sent to the host%s", hellosSent_++ ? " (no welcome yet; retrying)" : "");
    }

    // ------------------------------------------------------------------ host migration (session/hsm_migration.h)
    // The lobby's owner changed (Lobby::RefreshMembers): the old host left. The session closes at once (until the next
    // step this machine may be the lobby owner while its transport is still a client's) and waits for MigrationTick.
    bool OnLobbyOwnerChanged(steam::SteamId old, steam::SteamId now) {
        if (!cfg_.hostMigration || SteamServer()) return false;
        HSM_LOG("session: host migration: the lobby owner changed (%s)", now == steam::MySteamId() ? "now this machine" : "another member");
        BeginMigration(old);
        return true;
    }

    // A client's host went silent or closed its session (HostLossMonitor) in a Steam lobby: wait for Steam to name the
    // next owner instead of leaving. False: no migration (off, not in a lobby): back to the menu as before.
    bool MigrateAfterHostLoss() {
        if (!cfg_.hostMigration || !steamMode() || SteamServer() || !lobby_.InLobby()) return false;
        if (migration_.Active()) return true;
        HSM_LOG("client: host lost; host migration: waiting up to %u s for Steam to name a new host", kMigrationOwnerWaitMs / 1000);
        ui::ShowLine("HSMMod", style::kBone, "The host is gone. Waiting for a new host...");
        BeginMigration(hostSteam_);
        return true;
    }

    void BeginMigration(steam::SteamId oldHost) {
        if (!migration_.Active()) {
            migration_.Start(oldHost, NowMs());
            // The map to open again (the new host) or to wait in (the others): the fight level the old host had open.
            migrateLevel_ = InFightLevel() ? currentLevel_ : std::string();
            migrateBoard_ = ScoreCarryFrom(info_);   // the last MatchInfo heard: the new host restores these scores
            migrateObjects_ = props_.ExportKnown();  // the spawned objects this machine knows by id: the new host adopts them
            migrateReplica_ = (HostPhysics() || ownLeased_) && !IsHost() && InFightLevel();   // my fighter here is a replica: it cannot play on
        }
        ResetNetwork("host migration");
    }

    void MigrationTick() {
        if (!migration_.Active()) return;
        const uint32_t now = NowMs();
        const steam::SteamId owner = lobby_.InLobby() ? lobby_.owner() : 0;
        switch (migration_.Tick(owner, steam::MySteamId(), lobby_.InLobby(), cfg_.hostMigration, now)) {
            case MigrationStep::BecomeHost:
                HSM_LOG("session: host migration: this machine is the new host%s%s", migrateLevel_.empty() ? "" : "; reopening ",
                        migrateLevel_.c_str());
                ui::ShowLine("HSMMod", style::kBone, "The host left. You are the new host.");
                ResetNetwork("host migration");
                OnLobbyEntered();                 // as the owner: a host transport, every member allowed, BecomeHost next tick
                break;
            case MigrationStep::FollowHost:
                HSM_LOG("session: host migration: following the new host");
                ui::ShowLine("HSMMod", style::kBone, "The host left. " + (steam::PersonaName(owner).empty() ? std::string("Another player") : steam::PersonaName(owner)) + " is the new host.");
                ResetNetwork("host migration");
                OnLobbyEntered();                 // a client of the new owner: Hello, Welcome (migration_.Welcomed)
                break;
            case MigrationStep::GiveUp:
                HSM_LOG("session: host migration gave up (%s); returning to the main menu",
                        !cfg_.hostMigration ? "off" : !lobby_.InLobby() ? "left the lobby" : "no new host in time");
                migrateLevel_.clear(); migrateBoard_ = ScoreCarry{}; migrateObjects_.clear(); migrateReplica_ = false;
                if (lobby_.InLobby() || net_ || InFightLevel()) ReturnToMenuAfterHostLoss();
                return;
            default: break;
        }
        if (migration_.StartDue(now)) {
            // A fight map opens again with the lobby's settings (the scores come back with migrateBoard_ at the match start);
            // the Progression hub through the stock path; anything else stays where it is. Nobody else in the lobby: the
            // match stays on the open map instead, and starts there when the others are in.
            const std::string level = std::move(migrateLevel_);
            migrateLevel_.clear();
            const MapInfo* map = level.empty() ? nullptr : FindMap(level);
            const bool replica = migrateReplica_;
            migrateReplica_ = false;
            if (map && lobby_.members().size() <= 1 && !replica) HSM_LOG("session: host migration: no other member; staying on %s", level.c_str());
            else if (map && FindMap(kHubLevel) == map) StartProgression();
            else if (map) StartOnMap(map->path);
            else if (!level.empty()) HSM_LOG("session: host migration: %s is not a catalog map; staying here", level.c_str());
        }
    }

    void ReturnToMenuAfterHostLoss() {
        HSM_LOG("client: host lost; returning to the main menu");
        if (steamMode()) lobby_.Leave();
        ResetNetwork("host lost");
        game::OpenLevel(lastGameMode_, kMenuLevel, true, "");
    }

    void ClientTick(float dt) {
        bool hostGone = false;
        bodyWork_ = false;
        SetMyWillie(welcomed_ && FighterLevel() ? adapter_.PlayerWillie(0) : nullptr);   // before the pump: its messages read it
        Timed(kPumpNet, [&] {
            NetEvent ev;
            while (!bodyWork_ && net_->Poll(ev)) {   // the rest of the queue waits for the next frame (bodyWork_)
                if (!FromHost(ev)) { MeshEvent(ev); continue; }   // another client's session or stream, never the host's
                if (ev.kind == NetEvent::Connected) {
                    hostPeer_ = ev.peer;
                    hostBudget_ = MsgBudget{double(kMaxWillies)};   // what the host sends one client carries every fighter's streams
                    SendHello();
                } else if (ev.kind == NetEvent::Disconnected) {
                    HSM_LOG("client: disconnected from host");
                    const DisconnectAction act = ClientDisconnectAction(true, welcomed_, InFightLevel());
                    if (act == DisconnectAction::HostLossMonitor) hostGone = hostLoss_.OnDisconnect(NowMs()) == HostLossAction::ReturnToMenu;
                    welcomed_ = false; hostPeer_ = 0;
                } else { hostLoss_.OnPacket(NowMs()); ClientReceive(ev); }
            }
        });
        if ((meshOn_ || peerPingOn_) && welcomed_) MeshTick();
        // Only fights have steady host traffic (snapshots); menus, the Progression hub and loading screens never count
        // as silence.
        if (welcomed_ && FighterLevel()) {
            HostLossAction a = hostLoss_.Tick(NowMs());
            if (a == HostLossAction::ShowReconnecting) HSM_LOG("client: host silent; waiting for it to come back");
            hostGone = hostGone || a == HostLossAction::ReturnToMenu;
        } else hostLoss_.OnPacket(NowMs());
        if (rejected_) { const RejectReason why = *rejected_; rejected_.reset(); OnRejected(why); return; }
        if (modMapHost_ && !modMapChecked_) { ModMapTick(); if (!net_) return; }   // modded maps: the scan or a hash finished
        if (refuseNoHook_) { refuseNoHook_ = false; if (!ScriptHookMissing()) { HSM_LOG("client: refusing a host whose sync mode this build removed"); LeaveSession(); } return; }
        if (hostGone) { if (!MigrateAfterHostLoss()) ReturnToMenuAfterHostLoss(); return; }
        if (!welcomed_ && hostPeer_ && NowMs() >= nextHello_) SendHello();   // the first message of a Steam session can be lost
        if (welcomed_ && hostPeer_ && DueAt(NowMs(), nextKeepAlive_)) {   // in every level: the host's silence check (public #9)
            const uint32_t now = NowMs();
            nextKeepAlive_ = now + kKeepAliveMs;
            pings_.OnSent(kHostSlot, now);                                   // its Pong is the host row of the ping table
            net_->Send(hostPeer_, kChanUnreliable, EncodePing(now, false), false);
        }
        if (!welcomed_ || !FighterLevel()) { SetMyWillie(nullptr); return; }
        if (!ClientGateOpen()) { SetMyWillie(adapter_.PlayerWillie(0)); return; }
        RecoverLostPuppets();
        CreatePuppets();
        if (MatchLevel() && info_.phase != uint8_t(MatchPhase::Idle) && DueAt(NowMs(), nextScan_)) { nextScan_ = NowMs() + kScanIntervalMs; PurgeForeignClient(); }
        RetryOwnRespawn();
        if (dropClearPending_) {
            const uint32_t t = NowMs();
            if (ownDeadPending_ && !DueAt(t, dropClearGiveUp_)) dropClearDue_ = t + kDropClearDelayMs;   // the own body is not here yet
            else if (DueAt(t, dropClearDue_)) { dropClearPending_ = false; ClearDroppedWeapons(); }
        }
        UObject* me = adapter_.PlayerWillie(0);
        SetMyWillie(me);
        if (me) StopStockFoes();
        if (HostPhysics()) ClientReplicaTick(me, dt);
        else ClientOwnerTick(me, dt);
    }

    // Owner sync, client: nothing is spawned before the level's own start-up is over (BP_LevelManager's Clean Up Map
    // destroys every fighter, weapon and armour piece it did not spawn). Probed every 100 ms until it opens; then my
    // dressed fighter replaces the stock pawn and the stand-ins are created.
    bool ClientGateOpen() {
        if (levelGate_.Open()) return true;
        if (NowMs() < nextGateProbe_) return false;
        nextGateProbe_ = NowMs() + 100;
        LevelProbe p;
        std::vector<UObject*> managers = FindInstances("BP_LevelManager_C");
        p.manager = !managers.empty();
        if (p.manager) GetBool(managers.front(), "Player Spawned", p.playerSpawned);
        p.pawn = adapter_.PlayerWillie(0) != nullptr;
        const double now = game::SteadyNowMs();
        if (p.manager && !managerProbed_) {
            // Once per level: the delay the manager waits before it spawns the player (a Blueprint default the RE
            // tools cannot decode offline; test 5's line decides LevelSpawnGate::kMaxWaitMs).
            managerProbed_ = true;
            const double* delay = Field<double>(managers.front(), "Level Load Delay");
            if (delay) HSM_LOG("client: level manager found; Level Load Delay %.2f s, Player Spawned %d, pawn %d", *delay, p.playerSpawned, p.pawn);
            else HSM_LOG("client: level manager found; Level Load Delay unreadable, Player Spawned %d, pawn %d", p.playerSpawned, p.pawn);
            // Level props: 0.1 s into the level its manager destroys each prop tagged 'Random Delete' on a RandomBool of
            // this machine's own when "Purge Map" is set; the host's census decides which stay (ObjectSync purges the
            // rest), so the manager's own purge is turned off here. Best effort: the world time says whether in time.
            bool purge = false;
            const bool purgeRead = GetBool(managers.front(), "Purge Map", purge);
            const bool purgeOff = purgeRead && purge && SetBool(managers.front(), "Purge Map", false);
            HSM_LOG("client: level manager's Purge Map %s at world time %.2f s (its purge runs at 0.10 s)",
                    !purgeRead ? "unreadable" : !purge ? "already off" : purgeOff ? "turned off" : "on (could not turn it off)",
                    adapter_.WorldSeconds());
        }
        if (!levelGate_.Update(now, p)) return false;
        const char* why = levelGate_.why() == LevelSpawnGate::Why::LevelSpawned ? "the level spawned its player"
                        : levelGate_.why() == LevelSpawnGate::Why::NoManager ? "no level manager" : "timed out waiting for the level";
        const LevelProbe& last = levelGate_.lastProbe();
        if (levelGate_.why() == LevelSpawnGate::Why::TimedOut)
            HSM_LOG("client: level start-up over (%s, still waiting for %s; last probe: manager %d, Player Spawned %d, pawn %d); spawning fighters",
                    why, GateWaitingName(levelGate_.waitingFor()), last.manager, last.playerSpawned, last.pawn);
        else HSM_LOG("client: level start-up over (%s); spawning fighters", why);
        if (ownDeadPending_) if (const Spawn* sp = world_.Find(world_.myEntity())) OnOwnSpawn(*sp);
        return true;
    }

    // Owner sync, client: a stand-in the game destroyed while its owner is still announced is spawned again
    // (at most every RecreateLimiter::kRetryMs per entity); the servo then snaps it onto its owner's stream.
    void RecoverLostPuppets() {
        const double now = game::SteadyNowMs();
        for (auto it = puppets_.begin(); it != puppets_.end();) {
            const uint8_t e = it->first;
            if (adapter_.StillValid(it->second.obj, it->second.index) || !recreate_.Allow(e, now)) { ++it; continue; }
            standIns_.Detach(e);
            it = puppets_.erase(it);
            if (world_.MarkLost(e)) HSM_LOG("client: entity %u's stand-in was destroyed by the game; re-creating it", e);
        }
    }

    // Owner sync, client side: my fighter is mine to play. Moved once onto the spot the host announced for it (every
    // machine's stock spawner uses the same start), then streamed; every other fighter is a stand-in driven here.
    void ClientOwnerTick(UObject* me, float dt) {
        const double now = game::SteadyNowMs();
        const uint8_t mine = world_.myEntity();
        if (me && !snappedOwn_)
            if (const Spawn* sp = world_.Find(mine)) {
                snappedOwn_ = true;
                const FVector want{sp->x, sp->y, sp->z};
                if (Dist(adapter_.ActorLocation(me), want) > kTeleportDistance) {
                    adapter_.Teleport(me, want);
                    HSM_LOG("client: my fighter moved to its announced spawn");
                }
            }
        OwnHandOffTick(me, mine);
        if (me && ownLeased_) adapter_.TickLookSensitivity(me, dt);   // a replica while held elsewhere: its tick is blocked
        if (me && !hooksBound_ && adapter_.BindInputs(me)) BindClientInputHooks();
        SendInputs(dt);   // rev 32: always, so a machine can take my fighter over mid-grab with what I hold
        Timed(kPumpHandOff, [&] { HandOffTick(now); });
        std::vector<std::pair<uint8_t, UObject*>> owned;
        if (me && mine && snappedOwn_ && !ownLeased_) owned.push_back({mine, me});
        Timed(kPumpDrive, [&] {
            for (uint8_t e = 1; e <= kMaxFighters; ++e)   // a hand-off: another's fighter, held here from its player's input
                if (UObject* w = taken_[e] && HeldHere(e) ? ClientActor(e) : nullptr) { DriveFighter(heldInput_[e], w, dt); owned.push_back({e, w}); ++heldDrives_[e]; }
        });
        if (owned.size() > 1 && now >= nextListenerMs_) {   // a held fighter took my audio listener (item 17)
            nextListenerMs_ = now + 500.0;
            if (me) adapter_.KeepAudioListener(me);
        }
        Timed(kPumpStreams, [&] {
            streams_.Tick(adapter_, now, dt, owned, [this](const std::vector<uint8_t>& b) {
                if (hostPeer_) net_->Send(hostPeer_, kChanUnreliable, b, false);
                for (uint32_t p : meshSend_) net_->Send(p, kChanUnreliable, b, false);   // Pose and Vitals also go straight to the other clients
                DetourSend(b);                                                            // ...and through a relay where the host planned one
            });
            if (!ownLeased_) ReportOwnDeath(me, now);   // held elsewhere: its holder reports the death
        });
        Timed(kPumpServo, [&] { standIns_.Tick(adapter_, now, dt, OwnedPelvises(owned)); });
        Timed(kPumpClaims, [&] { claims_.Tick(now); });
        Timed(kPumpSevers, [&] { severs_.Tick(now, owned); });
        Timed(kPumpGore, [&] { gore_.Tick(now); });
        Timed(kPumpGrabs, [&] { grabs_.Tick(now, dt, owned); });
        chests_.Tick(now, StandInActors());
        Timed(kPumpProps, [&] { props_.Tick(now, owned); });
        Timed(kPumpEquip, [&] { equip_.Tick(now, owned); });
        fx_.Tick();
    }

    // Once per life: my fighter died here, natively; the host referees the match and credits the last blow I took.
    void ReportOwnDeath(UObject* me, double now) {
        if (!me || !adapter_.IsDead(me)) { if (me) ownDeathReported_ = false; return; }   // alive: a new life
        if (ownDeathReported_ || !hostPeer_) return;
        ownDeathReported_ = true;
        DeathReport r; r.victim = world_.myEntity(); r.killer = LastAttacker(r.victim, now); r.victimLife = LifeOf(r.victim);
        net_->Send(hostPeer_, kChanReliable, Encode(r), true);
        HSM_LOG("client: my fighter died; reported to the host (last blow from entity %u)", r.killer);
    }

    UObject* ClientActor(uint8_t entity) {
        if (entity == world_.myEntity()) return adapter_.PlayerWillie(0);
        auto it = puppets_.find(entity);
        if (it == puppets_.end()) return nullptr;
        // An actor the game destroyed is left in puppets_ for RecoverLostPuppets, which detaches it and marks it lost so it
        // is made again (0.8.0-pre7, #86: erased here first, inside the re-create limiter's 2 s, it was never re-created).
        if (!adapter_.StillValid(it->second.obj, it->second.index)) return nullptr;
        return it->second.obj;
    }

    void CreatePuppets() {
        if (bodyWork_) return;
        auto have = [this](uint32_t id) { return clientBlobs_.AtLeast(id) != nullptr; };
        for (const Spawn& sp : world_.PendingPuppets(have)) {
            game::Adapter::SpawnOptions o;
            o.blob = sp.blobId ? clientBlobs_.AtLeast(sp.blobId) : nullptr;
            o.at = {sp.x, sp.y, sp.z}; o.yawDeg = sp.yaw; o.teamInt = TeamIntFor(sp.team, sp.entity); o.brain = false;   // follows its owner's pose
            UObject* w = SpawnFighterNow(o);
            if (w) {
                puppets_[sp.entity].Set(w); QueueTint(w, sp.team); standIns_.Attach(sp.entity, w);
                if (sp.entity >= kFirstBotEntity) HSM_LOG("client: puppet created for bot entity %u", sp.entity);   // #37
            }
            else HSM_LOG("client: puppet for entity %u failed to spawn", sp.entity);
            world_.MarkCreated(sp.entity);           // no retry loop on failure; the next level tries again
            bodyWork_ = true;
            break;                                   // one per frame
        }
    }

    void DestroyPuppet(uint8_t entity) {
        auto it = puppets_.find(entity);
        if (it == puppets_.end()) return;
        bodyWork_ = true;
        standIns_.Detach(entity); adapter_.DestroyFighter(it->second.obj, it->second.index);
        puppets_.erase(it);
    }

    void UploadLoadout() {
        std::vector<uint8_t> bytes;
        if (!adapter_.ExportPassport(bytes) || bytes == lastUpload_) return;
        lastUpload_ = bytes;
        if (++uploadId_ == 0) uploadId_ = 1;
        std::vector<BlobChunk> chunks = SplitBlob(uploadId_, bytes);
        if (chunks.empty()) { HSM_LOG("client: loadout too large to send (%zu bytes)", bytes.size()); return; }
        for (const BlobChunk& c : chunks) net_->Send(hostPeer_, kChanReliable, Encode(c), true);
        HSM_LOG("client: loadout sent (%zu bytes, %zu chunks)", bytes.size(), chunks.size());
    }

    // ------------------------------------------------------------------ mesh (client side)
    // Whether an event is the host's session or stream. Only the lobby owner's Steam ID is the host; every other peer a
    // Steam client has is a mesh peer the host's PeerMap named.
    bool FromHost(const NetEvent& ev) const { return !steamNet_ || ev.steamId == hostSteam_; }

    // Pose and Vitals go direct only with the mesh on; with per-player pings alone the peers are only pinged.
    void RefreshMeshSend() { meshSend_ = meshOn_ ? meshPeers_.Peers() : std::vector<uint32_t>{}; }

    // The host's roster of mesh peers: allow the new ones (both ends allow each other, so SteamTransport admits them),
    // close the ones that went, and report what is heard again.
    void ApplyPeerMap(const PeerMapMsg& m) {
        const MeshPeers::Change c = meshPeers_.Apply(m, mySlot_, steam::MySteamId(), hostSteam_);
        for (uint64_t id : c.drop) steamNet_->DropPeer(id);
        for (uint64_t id : c.want) meshPeers_.Bind(id, steamNet_->AllowPeer(id));
        RefreshMeshSend();
        meshHearing_.Invalidate();
        HSM_LOG("client: mesh: %zu direct peers (%zu new, %zu gone)", meshPeers_.Named(), c.want.size(), c.drop.size());
    }

    // Anything but the host's own traffic: a peer connecting or going, or a message straight from another client: a ping
    // (per-player pings), a Pose or Vitals of the entity that peer's slot owns, or (Mesh v2) a direct copy of one of its
    // blows, cuts, stuck blades, paint, weapon, grab or prop states (MeshDirectType) that passes MeshSenderOk. Everything
    // else is dropped and counted.
    void MeshEvent(const NetEvent& ev) {
        if (ev.kind == NetEvent::Connected) return;
        if (ev.kind == NetEvent::Disconnected) {
            if (const uint8_t s = meshPeers_.SlotOfPeer(ev.peer); s < kMaxFighters) { pings_.Forget(s); detour_.Forget(s); }
            meshPeers_.ForgetPeer(ev.peer); RefreshMeshSend(); return;
        }
        const uint8_t* d = ev.data.data(); size_t n = ev.data.size();
        const uint8_t slot = meshPeers_.SlotOfPeer(ev.peer);
        MsgType t;
        if (slot >= kMaxFighters || !PeekType(d, n, t)) { ++meshDropped_; return; }
        if (t == MsgType::Ping || t == MsgType::Pong) { if (peerPingOn_) MeshPing(ev.peer, slot, d, n); else ++meshDropped_; return; }
        if (!meshOn_ || (t != MsgType::Pose && t != MsgType::Vitals && t != MsgType::Forward && !MeshDirectType(t))) { ++meshDropped_; return; }
        if (!meshBudget_[slot].Allow(t, NowMs())) { BudgetDropped(meshBudget_[slot], t, "client: mesh", slot); return; }
        if (t == MsgType::Forward) { DetourEvent(ev, slot); return; }
        if (MeshDirectType(t)) {
            if (!ApplyEventMsg(t, d, n, slot)) { ++meshDropped_; return; }
            meshHearing_.Note(slot, NowMs());
            ++meshRx_;
            return;
        }
        if (!MeshStreamIn(t, d, n, slot, ArrivalMs(ev))) ++meshDropped_;
    }

    // A Pose or Vitals of the entity fromSlot owns, straight from that client or (a detour) passed on by a relay.
    bool MeshStreamIn(MsgType t, const uint8_t* d, size_t n, uint8_t fromSlot, double arrivalMs) {
        if (t == MsgType::Pose) {
            PoseMsg m; sync::PoseFrame frame;
            if (!Decode(d, n, m, &frame) || !MayStream(fromSlot, m.entity, leases_.holders()) || !ClientStreamGate(m.entity, m.epoch)) return false;
            standIns_.OnPose(m, frame, arrivalMs);
        } else if (t == MsgType::Vitals) {
            VitalsMsg v;
            if (!Decode(d, n, v) || !MayStream(fromSlot, v.entity, leases_.holders()) || !ClientStreamGate(v.entity, v.epoch)) return false;
            standIns_.OnVitals(v);
            gore_.OnVitals(v.entity, v.severed, v.dislocated, game::SteadyNowMs());
            severs_.OnVitals(v, game::SteadyNowMs());
            VoiceVitals(v);
        } else {
            return false;
        }
        meshHearing_.Note(fromSlot, NowMs());   // heard without the host: the host stops relaying it (MeshHeard)
        ++meshRx_;
        return true;
    }

    // ------------------------------------------------------------------ detours (session/hsm_detour.h, step R1)
    // The host plans each client pair's path from the ping tables (LogRoutePlan) and sends every client its RouteTable:
    // per other client the path and, for a detour, the pair's key; and the pairs it relays for. A client sends each Pose
    // and Vitals direct (the mesh) and, where a detour is planned, also wrapped in a Forward through the relay, whose tag
    // only the two ends can make. The receiver takes whichever copy comes first; the host relays only what it does not
    // hear (MeshHeard counts forwarded arrivals too).
    bool DetourHost() const { return cfg_.detour && MeshHost(); }
    bool DetourClientOn() const { return cfg_.detour && meshOn_; }

    // The system's secure random source (RtlGenRandom), for the pair keys.
    static void SecureRandom(uint8_t* p, size_t n) {
        using Gen = BOOLEAN(WINAPI*)(PVOID, ULONG);
        static Gen gen = [] {
            HMODULE m = LoadLibraryW(L"advapi32.dll");
            return m ? reinterpret_cast<Gen>(reinterpret_cast<void*>(GetProcAddress(m, "SystemFunction036"))) : nullptr;
        }();
        if (gen && gen(p, ULONG(n))) return;
        std::random_device rd;   // rand_s on this runtime: also the system's source
        for (size_t i = 0; i < n; ++i) p[i] = uint8_t(rd());
    }

    // A new level's dice seed (session/hsm_dice.h): the secure source mixed with the clock, so no two travels agree.
    static uint64_t NewLevelSeed() {
        uint8_t b[8] = {};
        SecureRandom(b, sizeof b);
        uint64_t v = 0;
        for (uint8_t x : b) v = v << 8 | x;
        return dice::Mix64(v ^ uint64_t(GetTickCount64()));
    }

    // Host: a new epoch of tables to every client (the plan or the roster changed).
    void SendRouteTables() {
        detourDirty_ = false;
        if (!DetourHost() || !net_) return;
        ++detourEpoch_;
        const std::vector<uint8_t> clients = ConnectedSlots();
        int detours = 0;
        for (const auto& [peer, p] : peers_) {
            const RouteTableMsg t = MakeRouteTable(p.slot, clients, routePlanner_, pairKeys_, detourEpoch_, SecureRandom);
            for (const RouteKey& k : t.keys) if (k.via != kRouteViaDirect && k.via != kRouteViaHost) ++detours;
            net_->Send(peer, kChanReliable, Encode(t), true);
        }
        HSM_LOG("host: detour: route tables epoch %u to %zu clients, %d one-way detours", unsigned(detourEpoch_), peers_.size(), detours);
    }

    // Client: the host's table for this client.
    void ApplyRouteTable(const RouteTableMsg& t) {
        if (!DetourClientOn()) return;
        detour_.SetTable(mySlot_, t);
        if (detourLoggedEpoch_ != int(t.epoch)) {
            detourLoggedEpoch_ = int(t.epoch);
            int via = 0;
            for (uint8_t s = 1; s < kMaxFighters; ++s) if (detour_.RelayFor(s)) ++via;
            HSM_LOG("client: detour: table epoch %u, %d peer(s) through a relay, relaying %d pair(s)", unsigned(t.epoch), via, detour_.RelayPairs());
        }
    }

    // Client: one encoded Pose/Vitals of mine, through each relay the table names.
    void DetourSend(const std::vector<uint8_t>& b) {
        if (!DetourClientOn() || !detour_.HasTable()) return;
        for (uint8_t s : meshPeers_.Slots()) {
            const uint8_t via = detour_.RelayFor(s);
            const uint32_t p = via ? meshPeers_.PeerOfSlot(via) : 0;
            ForwardMsg f;
            if (!p || !detour_.MakeForward(s, b, f)) continue;
            net_->Send(p, kChanUnreliable, Encode(f), false);
            ++detourStats_.sent;
        }
    }

    // Client: a Forward from mesh peer `slot`: passed on unchanged when this client relays the pair, taken as the sender's
    // own stream when it is for this client and checks out, else dropped and counted by reason.
    void DetourEvent(const NetEvent& ev, uint8_t slot) {
        ForwardMsg m;
        if (!DetourClientOn() || !Decode(ev.data.data(), ev.data.size(), m)) { ++meshDropped_; return; }
        const RelayVerdict rv = detour_.MayRelay(slot, m, NowMs());
        if (rv == RelayVerdict::Ok) {
            const uint32_t p = meshPeers_.PeerOfSlot(m.to);
            if (!p) { ++detourStats_.refused[size_t(RelayVerdict::NotListed)]; return; }
            net_->Send(p, kChanUnreliable, ev.data, false);
            ++detourStats_.relayed;
            return;
        }
        if (rv != RelayVerdict::ForMe) { ++detourStats_.refused[size_t(rv)]; return; }
        const DetourVerdict v = detour_.Accept(slot, m);
        if (v != DetourVerdict::Ok) { ++detourStats_.rejected[size_t(v)]; return; }
        MsgType inner;
        if (!PeekType(m.payload.data(), m.payload.size(), inner) || !MeshStreamIn(inner, m.payload.data(), m.payload.size(), m.from, ArrivalMs(ev))) {
            ++detourStats_.rejected[size_t(DetourVerdict::NotForMe)];
            return;
        }
        ++detourStats_.accepted;
    }

    void LogDetourHealth(const char* who) {
        if (!DetourClientOn()) return;
        const DetourStats d = detourStats_;
        detourStats_ = DetourStats{};
        int via = 0;
        for (uint8_t s = 1; s < kMaxFighters; ++s) if (detour_.RelayFor(s)) ++via;
        if (!d.sent && !d.relayed && !d.accepted && !via && !detour_.RelayPairs()) return;
        HSM_LOG("%s: detour: epoch %u, %d peer(s) through a relay, relaying %d pair(s); sent %u, relayed %u, accepted %u; rejected: "
                "not for me %u, epoch %u, relay %u, tag %u, seq %u; not passed on: not listed %u, sender %u, budget %u",
                who, unsigned(detour_.Epoch()), via, detour_.RelayPairs(), d.sent, d.relayed, d.accepted,
                d.rejected[size_t(DetourVerdict::NotForMe)], d.rejected[size_t(DetourVerdict::Epoch)], d.rejected[size_t(DetourVerdict::Relay)],
                d.rejected[size_t(DetourVerdict::Tag)], d.rejected[size_t(DetourVerdict::Seq)], d.refused[size_t(RelayVerdict::NotListed)],
                d.refused[size_t(RelayVerdict::Sender)], d.refused[size_t(RelayVerdict::Budget)]);
    }

    // Tells the host which slots arrive directly (held 2 s after the last packet, at most one report a second), so it
    // relays the others as before. Per-player pings: a Ping to every mesh peer every 2 s, the table to the host every 10 s.
    void MeshTick() {
        uint16_t mask = 0;
        if (meshOn_ && hostPeer_ && meshHearing_.TakeChange(NowMs(), &mask)) {
            MeshHeardMsg m; m.mask = mask;
            net_->Send(hostPeer_, kChanReliable, Encode(m), true);
            HSM_LOG("client: mesh: hearing 0x%03x directly", unsigned(mask));
        }
        if (!peerPingOn_ || !net_) return;
        const uint32_t now = NowMs();
        for (uint8_t s : meshPeers_.Slots()) {
            const uint32_t p = meshPeers_.PeerOfSlot(s);
            if (!p || !pings_.Due(s, now)) continue;
            pings_.OnSent(s, now);
            net_->Send(p, kChanUnreliable, EncodePing(now, false), false);
        }
        if (hostPeer_ && DueAt(now, nextPingTable_)) {
            nextPingTable_ = now + kPingTableGapMs;
            const PingTableMsg m = MyPingTable(now);
            if (!m.entries.empty()) net_->Send(hostPeer_, kChanReliable, Encode(m), true);
        }
    }

    // A Ping from a mesh peer is answered (at most one Pong a second per peer: a flood gets nothing back); its Pong is
    // that peer's round trip.
    void MeshPing(uint32_t peer, uint8_t slot, const uint8_t* d, size_t n) {
        Ping p; bool pong = false;
        if (!DecodePing(d, n, p, pong)) { ++meshDropped_; return; }
        if (pong) { pings_.OnPong(slot, p.timeMs, NowMs()); return; }
        if (pongs_.Allow(slot, NowMs())) net_->Send(peer, kChanUnreliable, EncodePing(p.timeMs, true), false);
    }

    // This client's row: the host (its keep-alive Pong) and every mesh peer, with what Steam measures for the same link.
    PingTableMsg MyPingTable(uint32_t now) {
        PingTableMsg m;
        auto add = [&](uint8_t slot, uint32_t peer) {
            if (!pings_.Pinged(slot)) return;
            const PingSummary s = pings_.Summary(slot, now);
            PingTableEntry e; e.slot = slot; e.rttMs = s.medianMs; e.lossPct = s.lossPct;
            const uint32_t steam = peer ? net_->RoundTripMs(peer) : kPingUnknown;
            e.steamMs = uint16_t(std::min<uint32_t>(steam, kPingUnknown));
            m.entries.push_back(e);
        };
        add(kHostSlot, hostPeer_);
        for (uint8_t s : meshPeers_.Slots()) add(s, meshPeers_.PeerOfSlot(s));
        return m;
    }

    // Mesh v2: a copy of an event message straight to the mesh peers it concerns (MeshDirectSlots), next to the one to the
    // host. The host still checks, scores and relays its copy; whichever arrives first is used (MeshDedupe).
    void MeshCopy(MsgType t, uint8_t victim, const std::vector<uint8_t>& msg, bool reliable) {
        if (!meshOn_ || !net_) return;
        for (uint8_t s : MeshDirectSlots(t, victim, meshPeers_.Slots(), leases_.holders()))
            if (const uint32_t p = meshPeers_.PeerOfSlot(s)) { net_->Send(p, reliable ? kChanReliable : kChanUnreliable, msg, reliable); ++meshDirectTx_; }
    }

    // A client's event message, from the host (fromSlot kHostSlot: checked there) or straight from the mesh peer holding
    // fromSlot (checked here as the host would: MeshSenderOk, the body life, the friendly-fire rule). With the mesh on, the
    // second copy of anything is dropped (MeshDedupe; grab and prop states keep the newest on their own). False: a direct
    // copy refused.
    bool ApplyEventMsg(MsgType t, const uint8_t* d, size_t n, uint8_t fromSlot) {
        if (!FightMsgsOpen()) return false;
        const bool direct = fromSlot != kHostSlot;
        const uint8_t me = ClientOwnEntity();   // host physics: none, every fighter here is the host's
        const double now = game::SteadyNowMs();
        auto sender = [&](uint8_t actor, uint8_t victim, bool strip) {
            if (!direct) return true;
            MeshMsgFacts f; f.actor = actor; f.victim = victim; f.stripRequest = strip;
            return MeshSenderOk(t, fromSlot, f, me, leases_.holders());
        };
        auto blowOk = [&](uint8_t a, uint8_t v) { return !direct || (CombatEnabled() && HitAllowed(true, KnownTeam(a), KnownTeam(v), info_.friendlyFire)); };
        auto first = [&](MeshStream s, uint8_t e, uint32_t id) {
            if (!meshOn_ || meshDedupe_.Accept(s, e, id)) return true;
            ++meshDupes_;
            return false;
        };
        switch (t) {
            case MsgType::HitClaim: {
                HitClaimMsg m;
                if (!Decode(d, n, m) || !sender(m.claim.attacker, m.claim.victim, false) || !blowOk(m.claim.attacker, m.claim.victim)) return false;
                if (m.claim.victim == me && !StaleLife(m.claim.victim, m.claim.victimLife) && first(MeshStream::HitClaim, m.claim.attacker, m.claim.claimId))
                    claims_.Replay(m.claim, now);
                return true;
            }
            case MsgType::HitFx: {
                HitFxMsg m;
                if (!Decode(d, n, m) || !sender(m.claim.attacker, m.claim.victim, false) || !blowOk(m.claim.attacker, m.claim.victim)) return false;
                if (first(MeshStream::HitFx, m.claim.attacker, m.claim.claimId)) claims_.Effect(m.claim);
                return true;
            }
            case MsgType::SeverClaim: {
                combat::SeverClaimMsg m;
                if (!Decode(d, n, m) || !sender(m.claim.attacker, m.claim.victim, false) || !blowOk(m.claim.attacker, m.claim.victim)) return false;
                if (m.claim.victim == me && StaleLife(m.claim.victim, m.claim.victimLife)) return true;
                if (!first(MeshStream::SeverClaim, m.claim.attacker, m.claim.claimId)) return true;
                if (m.claim.victim == me) severs_.Replay(m.claim, now);
                else severs_.Remember(m.claim, now);
                return true;
            }
            case MsgType::SeverNotice: {
                combat::SeverNoticeMsg m;
                if (!Decode(d, n, m) || !sender(m.claim.victim, 0, false)) return false;
                if (m.claim.victim != me && first(MeshStream::SeverNotice, m.claim.victim, m.claim.claimId)) severs_.Remember(m.claim, now);
                return true;
            }
            case MsgType::StuckBlade: {
                combat::StuckBladeMsg m;
                if (!Decode(d, n, m) || !sender(m.attacker, m.victim, false) || (m.stuck && !blowOk(m.attacker, m.victim))) return false;
                if (m.victim == me && first(m.stuck ? MeshStream::StuckAttach : MeshStream::StuckRelease, m.attacker, m.seq)) severs_.OnStuck(m, now);
                return true;
            }
            case MsgType::GrabState: {
                GrabStateMsg m;
                if (!Decode(d, n, m) || !sender(m.grabber, 0, false)) return false;
                if (m.grabber != me) grabs_.OnGrabState(m, now);
                return true;
            }
            case MsgType::PaintFx: {
                combat::PaintFxMsg m;
                if (!Decode(d, n, m)) return false;
                const bool strip = combat::IsStripRequest(m);
                if (!sender(m.entity, 0, strip)) return false;
                if ((m.entity != me) != strip && (strip || first(MeshStream::PaintFx, m.entity, m.seq)) && fx_.Accept(m)) fx_.Apply(m);
                return true;
            }
            case MsgType::Equip: {
                combat::EquipStateMsg m;
                if (!Decode(d, n, m) || !sender(m.entity, 0, false)) return false;
                if (m.entity != me && first(MeshStream::Equip, m.entity, m.seq)) equip_.Apply(m);
                return true;
            }
            case MsgType::BodyState: {
                if (direct) {
                    BodyStateMsg m;
                    if (!Decode(d, n, m) || m.lessee != fromSlot || !sender(Roster::EntityForSlot(fromSlot), 0, false)) return false;
                }
                props_.OnBodyState(d, n, fromSlot);   // a direct one must come from the object's granted streamer
                return true;
            }
            case MsgType::ObjectEvent: {
                // Reliable and replayed idempotently, so a second copy (the host's relay of a direct one) does nothing.
                if (direct && !sender(Roster::EntityForSlot(fromSlot), 0, false)) return false;
                return props_.OnObjectEvent(d, n, fromSlot);   // a direct one must come from the object's granted streamer
            }
            default:
                return false;
        }
    }

    void ClientReceive(const NetEvent& ev) {
        const uint8_t* d = ev.data.data(); size_t n = ev.data.size();
        MsgType t; PeekType(d, n, t);
        if (!hostBudget_.Allow(t, NowMs())) { BudgetDropped(hostBudget_, t, "client", kHostSlot); return; }   // M3: a hostile host's flood
        switch (t) {
            case MsgType::Welcome: {
                Welcome w; Decode(d, n, w);
                welcomed_ = true; mySlot_ = w.slot; world_.SetMyEntity(w.entity);
                clientNextSync_ = SyncMode(w.syncMode); SetSyncMode(clientNextSync_);   // the host's level runs it; a later one may differ (MatchInfo)
                if (!ScriptHookActive()) { refuseNoHook_ = true; break; }   // handled after the pump (ScriptHookMissing / OnRejected)
                hostHasFighter_ = !(w.flags & kWelcomeNoHostFighter);
                if (!hostHasFighter_) HSM_LOG("client: the host plays no fighter of its own (hosted server): nothing to watch, score or wait for from it");
                HSM_LOG("client: joined session %u as slot %u (entity %u), %s", w.sessionId, w.slot, w.entity, SyncName(syncMode_));
                if (w.modBuild != game::ModBuild())
                    HSM_LOG("client: host on mod build '%s', ours '%s' - update; the host will not start a match until both match",
                            w.modBuild.c_str(), game::ModBuild().c_str());
                if (!streams_.Started()) StartOwner();   // a repeated Welcome keeps the stream clock
                if (migration_.step() == MigrationStep::Following) { migration_.Welcomed(); migrateBoard_ = ScoreCarry{}; migrateObjects_.clear(); HSM_LOG("client: host migration done"); }
                meshOn_ = cfg_.mesh && steamNet_ && (w.flags & kWelcomeMesh);
                peerPingOn_ = cfg_.peerPing && steamNet_ && (w.flags & kWelcomePeerPing);
                if (!meshOn_ && !peerPingOn_ && meshPeers_.Named()) ApplyPeerMap(PeerMapMsg{});   // the host stopped offering it
                RefreshMeshSend();
                if (meshOn_ || peerPingOn_) HSM_LOG("client: mesh %s, per-player pings %s", meshOn_ ? "on" : "off", peerPingOn_ ? "on" : "off");
                meshHearing_.Invalidate();                                              // a new host-side mask table: report again
                UploadLoadout();
                break;
            }
            case MsgType::PeerMap: { PeerMapMsg m; if ((meshOn_ || peerPingOn_) && Decode(d, n, m)) ApplyPeerMap(m); break; }
            case MsgType::Voice: { VoiceMsg m; if (Decode(d, n, m)) VoiceIn(m); break; }
            case MsgType::VoteState: { VoteStateMsg m; if (Decode(d, n, m)) voteState_ = m; break; }
            case MsgType::RouteTable: { RouteTableMsg m; if (Decode(d, n, m)) ApplyRouteTable(m); break; }
            case MsgType::Pong: { Ping p; bool pong = false; if (DecodePing(d, n, p, pong) && pong) pings_.OnPong(kHostSlot, p.timeMs, NowMs()); break; }
            case MsgType::Reject: { Reject r; Decode(d, n, r); rejected_ = r.reason; bodyWork_ = true; break; }   // handled after the poll loop
            case MsgType::Travel: {
                Travel tr; Decode(d, n, tr);
                if (!TravelAllowedWithModMap(tr.level, tr.options, modMapApproved_)) {   // H2: only catalog/menu levels (or the host's checked modded map), no options
                    if (modMapHost_ && !modMapChecked_ && TravelAllowedWithModMap(tr.level, tr.options, modMapHost_->path)) {
                        pendingTravel_ = tr;   // the pak check is still running: follow once it passes
                        break;
                    }
                    HSM_LOG("client: refused travel to '%.64s' (level or options not allowed)", TravelLogText(tr.level).c_str());
                    break;
                }
                FollowTravel(tr);
                break;
            }
            case MsgType::LevelSeed: { dice::LevelSeedMsg m; if (Decode(d, n, m)) game::shareddice::SetSeed(m.seed); break; }   // the host's dice for the level it sent us to
            case MsgType::ModMap: {
                ModMapMsg m; if (!Decode(d, n, m)) break;
                modMapHost_ = m; modMapChecked_ = false; modMapApproved_.clear(); pendingTravel_.reset();
                bodyWork_ = true;
                ModMapTick();
                break;
            }
            case MsgType::HandOff: {   // rev 32: hand-off leases, written by an owner or a holder and relayed (sync/hsm_handoff.h)
                sync::HandOffMsg m; if (Decode(d, n, m)) ClientLeaseIn(m);
                break;
            }
            case MsgType::HandOffAsk: {   // another machine's fighter grabbed or stabbed mine
                sync::HandOffAskMsg m; if (Decode(d, n, m) && FightMsgsOpen()) AnswerAsk(m, OwnerSlotOf(m.by));
                break;
            }
            case MsgType::RelayInput: {   // the input of a fighter I hold, from its player (through the host)
                RelayInput m; if (Decode(d, n, m) && HeldHere(m.entity) && heldInput_[m.entity].Receive(m.frame)) ++heldRx_[m.entity];
                break;
            }
            case MsgType::MapReset: {
                MapResetMsg m; if (!Decode(d, n, m)) break;
                mapGate_.Queue(m, NowMs());
                break;
            }
            case MsgType::Spawn: {
                Spawn sp; Decode(d, n, sp);
                if (sp.entity <= kMaxWillies) clientLife_[sp.entity] = sp.life;
                if (sp.entity >= kFirstBotEntity)   // #37: a bot of the host's
                    HSM_LOG("client: bot entity %u spawn received (team %d)", sp.entity, int(sp.team));
                if (world_.OnSpawn(sp)) DestroyPuppet(sp.entity);
                if (sp.entity == world_.myEntity()) OnOwnSpawn(sp);
                break;
            }
            case MsgType::Despawn: {
                Despawn dp; Decode(d, n, dp);
                if (dp.entity != ClientOwnEntity()) {
                    standIns_.Forget(dp.entity); severs_.Forget(dp.entity); gore_.Forget(dp.entity); claims_.Forget(dp.entity); fx_.Forget(dp.entity); equip_.Forget(dp.entity);
                    grabs_.Forget(dp.entity); props_.ForgetEntity(dp.entity);
                }
                meshDedupe_.Forget(dp.entity);             // a rejoined machine numbers its claims and fx from 1 again
                if (world_.OnDespawn(dp.entity)) DestroyPuppet(dp.entity);
                if (dp.entity == world_.myEntity()) ownDeadPending_ = true;   // a respawn follows
                break;
            }
            case MsgType::MatchInfo: {
                MatchInfo m; if (!Decode(d, n, m)) break;
                clientNextSync_ = SyncMode(m.syncMode);   // rev 29: taken at the next level change
                const bool groundClear = groundSeen_ && m.clearCount != info_.clearCount && CombatPhase(MatchPhase(m.phase));   // the host cleared its ground
                groundSeen_ = true;
                info_ = std::move(m); infoAt_ = NowMs();
                if (groundClear) GroundCleared();
                if (MatchLevel()) {
                    adapter_.SetBuild(ForcedWeight(Format(info_.format), Gear(info_.gear), info_.tier), info_.forceStrength != 0 && !lobby_.Progression(), int(info_.tier));
                    adapter_.SetBare(IsBrawl(Format(info_.format), Gear(info_.gear)));
                }
                banner_.Observe(info_, world_.myEntity(), infoAt_);
                if (dropWatch_.Observe(Format(info_.format), info_.phase, info_.round)) {   // F5: after the own body's armour is on
                    dropClearPending_ = true; dropClearDue_ = NowMs() + kDropClearDelayMs; dropClearGiveUp_ = NowMs() + kDropClearGiveUpMs;
                }
                break;
            }
            case MsgType::Kill: { Kill k; Decode(d, n, k); PushFeed(k); break; }
            case MsgType::Chat: {
                ChatLine c; Decode(d, n, c);
                if (c.fromSlot == kChatSystemSlot) ui::ShowLine("SERVER", style::kBone, c.text);
                else ui::ShowLine(NameOfSlot(c.fromSlot), ChatNameInk(info_, Roster::EntityForSlot(c.fromSlot)), c.text);
                break;
            }
            case MsgType::ReportShare: { ReportShareMsg m; Decode(d, n, m); if (m.fromSlot != mySlot_) OfferLog(m); break; }
            case MsgType::BlobChunk: {
                BlobChunk c; Decode(d, n, c);
                std::vector<uint8_t> blob;
                BlobAssembler::Result res = clientAssembler_.Add(c, GetTickCount64(), blob);
                std::string why;
                if (res == BlobAssembler::Result::Complete && !CheckPassportSave(blob.data(), blob.size(), &why)) {
                    HSM_LOG("client: refused loadout %08x (%zu bytes): %s", c.blobId, blob.size(), why.c_str());
                    break;
                }
                if (res == BlobAssembler::Result::Complete && clientBlobs_.Put(c.blobId, std::move(blob)))
                    HSM_LOG("client: loadout %08x for slot %d received", c.blobId, BlobSlot(c.blobId));
                break;
            }
            case MsgType::Pose: {
                PoseMsg m; sync::PoseFrame frame;
                if (Decode(d, n, m, &frame) && m.entity != ClientOwnEntity() && ClientStreamGate(m.entity, m.epoch)) standIns_.OnPose(m, frame, ArrivalMs(ev));
                break;
            }
            case MsgType::Vitals: {
                VitalsMsg v;
                if (Decode(d, n, v) && v.entity != ClientOwnEntity() && ClientStreamGate(v.entity, v.epoch)) {
                    standIns_.OnVitals(v);
                    gore_.OnVitals(v.entity, v.severed, v.dislocated, game::SteadyNowMs());
                    severs_.OnVitals(v, game::SteadyNowMs());
                    VoiceVitals(v);
                }
                break;
            }
            case MsgType::HitClaim: case MsgType::HitFx: case MsgType::SeverClaim: case MsgType::SeverNotice: case MsgType::StuckBlade:
            case MsgType::GrabState: case MsgType::PaintFx: case MsgType::Equip: case MsgType::BodyState: case MsgType::ObjectEvent:
                ApplyEventMsg(t, d, n, kHostSlot);
                break;
            case MsgType::ObjectAnnounce: { props_.OnObjectAnnounce(d, n, kHostSlot); break; }
            case MsgType::PropLease: { props_.OnPropLease(d, n, kHostSlot); break; }
            case MsgType::PropCensus: { props_.OnPropCensus(d, n); break; }
            case MsgType::RemoteWrite: {
                sync::RemoteWriteMsg m;
                if (FightMsgsOpen() && Decode(d, n, m) && m.target == ClientOwnEntity()) ApplyRemote(m);
                break;
            }
            default: break;
        }
    }

    // Chaos contacts reach Blueprints only through component hit delegates; a client fighting in a session
    // blocks them so damage is computed by the host only.
    void InstallClientSuppression() {
        HookFunctionsMatching([](const std::string& n) { return n.find("ComponentHitSignature") != std::string::npos; },
                              [this](UObject* obj, UFunction* fn, void* params) {
                                  return HitHook(obj, fn, params);
                              });
    }

    Config cfg_;
    const bool localFighter_ = cfg_.role != Role::Server;
    game::Lobby lobby_;
    std::unique_ptr<ITransport> net_;
    SteamTransport* steamNet_ = nullptr;
    game::Adapter adapter_;
    game::Mirror mirror_;
    game::SaveGuard saveGuard_;                      // the player's GameProgress, kept out of the session
    game::Recorder recorder_{cfg_.record};           // [mod] record=1: HSMRec-*.hsmrec per fight level
    bool selfTested_ = false, selfTestOk_ = false;
    UObject* lastGameMode_ = nullptr;
    int32_t lastGameModeIndex_ = -1;
    std::string currentLevel_;
    bool progressionHub_ = false;   // currentLevel_ is the Tavern outside Free Mode (FighterLevel)
    uint32_t travelSeq_ = 0;
    std::string pendingLevel_;
    // host
    bool hostPending_ = false, refuseNoHook_ = false;
    HostMigration migration_;      // survives ResetNetwork: it spans the old session and the new one
    std::string migrateLevel_;     // the fight level open when the old host left
    ScoreCarry migrateBoard_;      // its scores by SteamID, until the new host's match starts (ResetNetwork drops it outside a migration)
    std::vector<ObjectAnnounceMsg> migrateObjects_;   // the spawned objects this machine knew when the host left (the new host adopts them)
    bool serverReadyLogged_ = false;
    uint32_t serverLobbyAt_ = 0, serverConfigAt_ = 0;   // role=server over Steam: next lobby open try, next settings push
    // role=server
    std::unique_ptr<AutoFlow> flow_;
    bool serverTravelling_ = false;                  // a map change is under way: no match starts on the old level
    std::optional<FVector> serverAnchor_;
    AnchorKind serverAnchorKind_ = AnchorKind::None;
    uint32_t serverPawnSeenAt_ = 0, serverStripAt_ = 0;
    AnchorSearch anchorSearch_;
    int serverStripped_ = 0;
    Roster roster_;
    HostDirector director_;
    BlobStore hostBlobs_;
    std::array<uint32_t, kMaxWillies> blobVersion_{};
    std::array<std::vector<uint8_t>, kMaxFighters> ownLoadout_{};   // each slot's own upload (KeepOwnLoadout)
    std::array<uint32_t, kMaxFighters> ownBlobId_{};
    std::vector<uint8_t> hostExport_;
    std::vector<uint8_t> mirrorBlob_;            // the match's shared generated loadout (Everyone identical), for joiners
    std::map<uint32_t, Peer> peers_;
    std::array<Fighter, kMaxFighters> fighters_;
    std::optional<Travel> lastTravel_;
    std::vector<Setting> fightSettings_;            // the settings of the session travel into this level (re-asserted on load)
    bool picksValid_ = false;
    FVector spawnCenter_;
    SpawnArea spawnArea_;                     // AreaOf the spawn candidates (picksValid_)
    std::string spawnersLogged_;              // the level SpawnerPoints last logged its count for
    std::vector<Point3> spawnSpots_;           // SpreadPositions per slot (picksValid_)
    uint32_t sessionId_ = NowMs();
    // match (host)
    struct Tint { ActorRef actor; int8_t team = kNoTeam; int tries = 0; uint32_t next = 0; };
    Match match_;
    KillLog kills_;
    std::map<uint8_t, int8_t> slotTeam_;
    std::map<uint8_t, ActorRef> bots_;
    UObject* hostWillie_ = nullptr;
    bool adoptStock_ = false, wavesEnabled_ = false, sidesValid_ = false, infoDirty_ = false, progression_ = false;
    bool oneFight_ = false;                          // this level holds a Progression fight: the HUD shows one fight, no waves
    int wave_ = 0;
    uint32_t nextCorpseSweep_ = 0;
    uint32_t nextScan_ = 0, nextInfo_ = 0, nextDraw_ = 0, nextIdleLog_ = 0;
    DrawCheck draw_;
    std::vector<Point3> sidePoints_;
    SpawnArea sidesArea_;                     // AreaOf(sidePoints_)
    std::vector<int8_t> sides_;
    // match (both)
    MatchInfo info_;
    uint32_t infoAt_ = 0;     // when info_ was made (host) or arrived (client), for Extrapolate
    std::deque<FeedItem> feed_;
    BannerTracker banner_;    // the centre banner the match's changes raise
    std::string buildWarnLogged_;   // BuildsAllowStart's last logged warning
    std::vector<Tint> tints_;
    bool ownDeadPending_ = false;
    // Host physics (0.9.0): the mode this level runs; the host's pick for the next one (host) or the last one heard
    // (client); the client's input uplink.
    SyncMode syncMode_ = SyncMode::Owner, nextSync_ = SyncMode::Owner, clientNextSync_ = SyncMode::Owner;
    InputCapture capture_;
    float sendAccum_ = 0;
    uint32_t inputsTx_ = 0;
    bool hooksBound_ = false;
    // Hand-off (rev 32, sync/hsm_handoff.h): the leases, what is held here, my asks; the buttons my fighter itself last got.
    sync::LeaseTable leases_;                                   // rev 32: hand-offs, every machine's view
    std::array<bool, kMaxFighters + 1> taken_{};                // a fighter held here is a live fighter here (TakeFighter ran)
    lockon::Tracker lock_;                                      // client, host physics: the camera lock's target (ClientLockOn)
    uint8_t lockTarget_ = 0;                                    // ... as last sent and logged
    std::array<InputApplier, kMaxFighters + 1> heldInput_{};    // client: the input of a fighter held here (RelayInput)
    std::array<uint32_t, kMaxFighters + 1> heldRx_{}, heldDrives_{};   // while held here: its player's frames that came, frames driven
    std::array<double, kMaxWillies + 1> reAskAt_{};             // no ask for this entity before (a give-back just happened)
    std::array<double, kMaxWillies + 1> askedAt_{};             // my last ask per victim
    std::array<uint8_t, kMaxWillies + 1> asking_{};             // victim -> my fighter coupled to it (the crossed-asks rule)
    std::array<double, kMaxFighters + 1> leaseSince_{};         // when a lease started (its log line on the way back)
    std::array<bool, kMaxFighters + 1> heldDeathSent_{};        // client: a held fighter's death was reported
    std::array<std::pair<uint8_t, double>, kMaxFighters + 1> nativeHits_{};   // client: the last fighter simulated here to hit one here
    uint64_t leasesRefused_ = 0, asksRefused_ = 0;
    bool ownLeased_ = false;                                    // my fighter is a replica of its holder's stream
    uint32_t nativeButtons_ = 0;
    // drive_lean and host_input_delay (hsm_drivelean.h, hsm_inputdelay.h)
    std::unordered_map<const InputApplier*, LeanFeed> leanFeed_;   // per driven fighter, by its applier
    uint64_t driveFed_ = 0, driveSkipped_ = 0;                     // axis events played / spared in this health window
    HostDelayGovernor hostDelay_;
    DelayedInput delayedIn_;
    bool replayingDelayed_ = false, hostDelayLogged_ = false;
    UObject* delayPawn_ = nullptr;
    bool migrateReplica_ = false;           // a migration left a host-physics level: my fighter there was a replica, the map opens again               // the input hooks are per function and stay; they act only on myWillie_ under host physics
    RespawnRetry ownRespawn_;
    static constexpr uint32_t kDropClearDelayMs = 1500, kDropClearGiveUpMs = 5000;
    uint32_t nextGroundClear_ = 0;                   // host: when the Clear ground timer fires next, 0 = not running
    uint8_t clearCount_ = 0;                         // host: the ground clears so far (MatchInfo.clearCount)
    bool groundSeen_ = false;                        // client: info_ holds a MatchInfo of this session (clearCount to compare)
    bool dropClearPending_ = false;                  // F5: client, a rematch / Duel round: ClearDroppedWeapons waits for the own body
    uint32_t dropClearDue_ = 0, dropClearGiveUp_ = 0;
    std::deque<uint8_t> respawnQueue_;   // host, owner sync: entities whose respawn waits for its tick (RespawnQueued)
    uint32_t ownDiedAt_ = 0;
    GiveUpHold giveUp_;
    std::unordered_map<uint8_t, uint32_t> gaveUp_;   // host: entity -> when it held G (free-for-all, teams, Duel)
    UObject* giveUpProbed_ = nullptr; uint32_t probeHeldAt_ = 0; int probeLines_ = 0;   // the give-up probe (ProbeGiveUpFunctions)
    bool giveUpWait_ = false;                        // G is held but there is nothing to give up yet (queued for respawn / round gap)
    MapResetGate mapGate_;                           // a queued map reset waits for the respawn and the drop clear
    uint16_t mapResetSeq_ = 0;                       // host: the count of MapResets sent
    DropClearWatch dropWatch_;                       // Duel round / rematch: the drops are cleared (DropClearFor)
    LooseTimer looseTimer_;                          // loose armour pieces here (DropsTick)
    bool looseOn_ = false;                           // the timer has its baseline for this match
    uint32_t nextLooseSweep_ = 0;
    uint64_t loosePurged_ = 0;
    std::set<uint8_t> looted_;                       // Survival bots that dropped their loot
    static constexpr uint32_t kLooseSweepMs = 5000;
    GearPool gearPool_;
    uint32_t nextGearAhead_ = 0;
    uint32_t ownBlob_ = 0;    // the loadout blob of this machine's last own Spawn (client): a change is new gear
    uint8_t chatPhase_ = 0;   // the match phase the chat last saw (MatchPhase)
    // client
    uint32_t hostPeer_ = 0, uploadId_ = 0;
    // Mesh (session/hsm_mesh.h). Client: hostSteam_ is the lobby owner (the only host peer); meshPeers_ the other clients the
    // PeerMap named; meshSend_ their transport peers; meshHearing_ which of them arrive directly. Host: meshMasks_ per
    // client, and the PeerMap goes out when the roster changed.
    uint64_t hostSteam_ = 0;
    bool meshOn_ = false, peerMapDirty_ = false;
    MeshPeers meshPeers_;
    std::vector<uint32_t> meshSend_;
    MeshHearing meshHearing_;
    MeshMasks meshMasks_;
    uint32_t meshRx_ = 0, meshDropped_ = 0, meshRelaySkipped_ = 0;   // per health window
    // Mesh v2 (client): first copy wins; a budget per mesh peer; direct copies sent and second copies dropped per window.
    MeshDedupe meshDedupe_;
    std::array<MsgBudget, kMaxFighters> meshBudget_{};
    uint32_t meshDirectTx_ = 0, meshDupes_ = 0;
    // Per-player pings (session/hsm_peer_ping.h). Client: pings_ to the host (keep-alive) and to every mesh peer, pongs_
    // caps the answers; the table goes to the host every kPingTableGapMs. Host: every client's last table.
    bool peerPingOn_ = false;
    // proximity voice
    struct VoiceState {
        voice::ConsciousGate gate;
        uint32_t vitalsAt = 0;          // the last vitals for this entity (0: none yet)
        bool dead = false;
        voice::Reach reach = voice::Reach::Proximity;   // the speaker's own setting, from its last chunk (Voice flags)
        bool noCut = false;             // ...and whether it keeps talking when knocked out
        voice::VoiceRate rate;          // host: per sender; client: per speaker as relayed
    };
    struct VoiceDrops { uint32_t cut = 0, distant = 0, deaf = 0, rate = 0, sender = 0, big = 0, off = 0; };
    game::Voice voice_;
    std::array<VoiceState, voice::kMaxSpeakers> voiceState_{};
    VoiceDrops voiceDrops_;
    game::Voice::CaptureOut voiceOut_;
    uint32_t voiceRelayed_ = 0, voiceVitalsAt_ = 0, voiceCutLogs_ = 0;
    uint32_t voiceMeterUntil_ = 0;   // a menu shows the microphone meter until then
    uint16_t voiceSeq_ = 0;
    bool voiceConfigured_ = false, voiceSteamTried_ = false;
    std::map<std::string, voice::Listen> listen_;   // this player's own mute and volume per speaker, by name, while the game runs
    // votekick: the client's view of the host's vote; the host's broadcast state
    VoteStateMsg voteState_;
    uint32_t voteSentAt_ = 0;
    bool voteDirty_ = false, voteWasActive_ = false;
    // detours: client tables and counters; host pair keys and the table epoch
    struct DetourStats {
        uint32_t sent = 0, relayed = 0, accepted = 0;
        std::array<uint32_t, 6> rejected{};   // by DetourVerdict
        std::array<uint32_t, 5> refused{};    // by RelayVerdict
    };
    DetourClient detour_;
    PairKeys pairKeys_;
    uint8_t detourEpoch_ = 0;
    bool detourDirty_ = false;
    int detourLoggedEpoch_ = -1;
    DetourStats detourStats_;
    PeerPings pings_;
    PongLimiter pongs_;
    uint32_t nextPingTable_ = 0;
    std::map<uint8_t, PingTableMsg> pingTables_;
    RouteLinks routeLinks_;              // host: every client's last ping table, for the route planner
    RoutePlanner routePlanner_;
    uint32_t nextRoutePlan_ = 0;
    bool routeLogged_ = false;

    MsgBudget hostBudget_{double(kMaxWillies)};   // the client's budget for what the host sends (M3)
    bool welcomed_ = false, snappedOwn_ = false;
    bool hostHasFighter_ = true;   // client: false when the Welcome carried kWelcomeNoHostFighter
    // A fighter was destroyed or spawned this tick (about 60 ms each): the next one waits for the next frame. A Duel
    // round reset did four in one tick (two-player logs 8 and 9: 250 ms).
    bool bodyWork_ = false;
    uint8_t mySlot_ = 0;
    ReportOffer reportOffer_;                     // another player's report this player may add a log to
    BanList bans_;                                // HSMMod.bans plus the short blocks of kicked players (host)
    bool bansLoaded_ = false, adminsParsed_ = false;
    std::vector<uint64_t> admins_;                // server_admins= (role=server)
    VoteKick vote_;
    std::string voteTargetName_;
    std::vector<std::pair<uint32_t, uint32_t>> pendingDrops_;   // rejected peer, when its connection closes
    static constexpr uint32_t kRejectLingerMs = 1000;
    uint32_t nextConsolePoll_ = 0, nextBanPrune_ = 0;
    std::optional<RejectReason> rejected_;
    std::optional<ModMapMsg> modMapHost_;      // client: the host's modded map (empty path for a stock one) and its pak
    bool modMapChecked_ = false;
    std::string modMapApproved_;               // the host's map path once this machine has the pak: the one non-catalog level a Travel may open
    std::optional<Travel> pendingTravel_;      // a Travel that waits for the pak check
    uint32_t bugHintAt_ = 0;   // when to show the /bug hint (0: not due)
    bool bugHintShown_ = false;
    std::string notice_;   // why the host turned this player away, for the multiplayer page (TakeNotice)        // client: the host's Reject, acted on after the poll loop
    PendingShare pendingShare_;                   // a crash report filed before any session: shared with the next one
    static constexpr size_t kCrashTailBytes = 256u << 10;   // how much of HSMMod.prev.log is read for a crash line
    bool crashChecked_ = false, autoAttachDue_ = false;
    std::deque<std::string> reportHeld_;          // outcome lines waiting for the chat or the multiplayer page (ReportOutcomeToPlayer)
    std::string reportStatus_; uint32_t reportStatusSeq_ = 0;   // the last outcome told, for the menu's note
    uint32_t lastReportAt_ = 0; bool reportedOnce_ = false;
    uint32_t sessionGen_ = 0;                     // counts sessions (bumped when one starts and when it ends)
    uint32_t reportSession_ = 0; bool reportIsManual_ = false;   // the session the report in flight was filed in; typed by the player
    LobbyChatGate lobbyGate_;        // incoming lobby chat, a limit per sender
    ChatRateLimiter lobbyChatOut_;   // our own lines from the lobby chat box
    ClientWorld world_;
    BlobStore clientBlobs_;
    BlobAssembler clientAssembler_;
    std::vector<uint8_t> lastUpload_;
    std::map<uint8_t, ActorRef> puppets_;
    LevelSpawnGate levelGate_;                   // owner sync: the level's start-up cleanup is over (hsm_level_gate.h)
    uint32_t nextGateProbe_ = 0;
    bool managerProbed_ = false;   // this level's manager seen by ClientGateOpen (its Level Load Delay logged)
    bool pauseLiftArmed_ = false;                    // LiftPauseFromWidgetTick: set per tick, cleared on travel
    uint32_t lastPauseButton_ = 0;                   // PauseReportTick's throttle (ms)
    uint32_t timeInputBlocked_ = 0, timeReset_ = 0;
    double nextListenerMs_ = 0;
    static constexpr uint32_t kRingOutEveryMs = 3000;   // a volume fires on every overlap; the death takes a moment
    std::unordered_map<uint8_t, uint32_t> ringOutAt_;   // BlockTimeInput, ResetTimeDilation
    bool stockFlowGuard_ = false;                    // GuardStockFlow's hooks block while set (per tick)
    uint32_t stockFlowBlocked_ = 0, unpaused_ = 0;
    RecreateLimiter recreate_;                   // owner sync: lost stand-ins, both sides
    std::set<uint8_t> lostSlots_;                // host, owner sync: client stand-ins the game destroyed
    std::atomic<UObject*> myWillie_{nullptr};
    UObject* ownBuildActor_ = nullptr;          // TickOwnBuild: the own body the forced build was last put on, and that build
    double ownBuildWeight_ = -1.0;
    std::atomic<int32_t> myWillieIndex_{-1};   // ue::ObjectIndex of myWillie_ when it was stored (MyWillieLive)
    uint32_t hitsBlocked_ = 0;
    std::unordered_map<UFunction*, int32_t> otherActorOffset_, normalImpulseOffset_;
    std::unordered_map<UClass*, HitKind> hitKind_;
    std::unordered_map<UObject*, UObject*> fighterOfFrame_;
    enum class SpawnIgnoreState : uint8_t { Outside, InTick, Cached };
    std::vector<UObject*> spawnIgnore_;     // SpawnIgnore, this Tick only
    SpawnIgnoreState spawnIgnoreState_ = SpawnIgnoreState::Outside;
    bool inFight_ = false;          // InFightLevel() cached per tick for the hit hook
    HostLossMonitor hostLoss_{0};
    PeerSilenceMonitor peerSilence_;
    HelloWatch helloWatch_;
    ReservationTable reservations_;
    double groundOffset_ = 0;
    bool groundValid_ = false, foesStopped_ = false;
    int savedFoes_ = -1;
    uint32_t probeSince_ = 0;   // InputProbeTick: when the fight level loaded, 0 once logged
    uint32_t levelLoadedAt_ = 0;
    uint32_t inputFixAt_ = 0, nextHealth_ = 0, nextHello_ = 0, hellosSent_ = 0;
    uint32_t menuInputAt_ = 0;
    bool inputFixRepeat_ = false, pauseMenuWas_ = false, menuInputRepeat_ = false;
    SpectateCycler spectate_;
    uint8_t spectating_ = 0;
    UObject* spectateActor_ = nullptr;
    int32_t spectateIndex_ = -1;
    uint32_t spectateApplyAt_ = 0, spectateMovedAt_ = 0;
    bool spectateKeyDown_ = false;
    OcclusionTracker tagSight_;                  // name tags: which heads the camera sees (hsm_occlusion.h)
    uint32_t tagTraces_ = 0, tagTraceSince_ = 0; bool tagTraceLogged_ = false;
    // agent bridge / auto-run
    std::array<Held, kAxisCount> held_{};
    bool quitRequested_ = false, automated_ = false;
    uint32_t startedAt_ = 0, quitAfterMs_ = 0, nextAuto_ = 0, lastModeTick_ = 0, lastEarlyPump_ = 0, lastPauseCheck_ = 0, noPawnSince_ = 0;
    std::string autoLevel_;
    std::vector<std::string> mirrorArgs_;           // -hsmmirror as a "mirror on ..." command; empty when not given
    bool mirrorDone_ = false;                       // the mirror came on in this level (turning it off keeps it off)
    uint32_t mirrorSince_ = 0, nextMirror_ = 0;
    // owner sync (both roles)
    game::OwnerStreams streams_;
    game::StandIns standIns_;
    game::Claims claims_;
    game::FxSync fx_;
    game::EquipSync equip_;
    game::GrabSync grabs_;
    game::ChestLids chests_;   // sync audit item 43
    game::AbyssCoop abyss_;    // sync audit items 36, 37
    game::ObjectSync props_;
    game::Severs severs_;
    game::StandInGore gore_;
    struct OwnerLife { bool aliveSeen = false; uint32_t deadSince = 0; };
    std::map<uint8_t, OwnerLife> lives_;       // host: client fighters as their vitals tell it
    bool ownDeathReported_ = false;
    LifeBook lifeBook_;                                  // host: the Spawn.life of each entity's current body
    std::array<uint8_t, kMaxWillies + 1> clientLife_{};  // client: the life the host's last Spawn named, per entity
    uint32_t staleLife_ = 0;                             // claims and reports dropped for naming an old body
    uint32_t nextKeepAlive_ = 0, streamsDropped_ = 0, claimsRejected_ = 0;
    sync::RemoteGate remoteGate_;                          // the owner's side of RemoteWrite revives
    uint64_t remoteApplied_ = 0, remoteDropped_ = 0;
    uint16_t remoteSeq_ = 0;
    uint32_t spectateOverlaysAt_ = 0;   // the last HideDeathOverlays scan
    int spectateScreensLogged_ = 0;   // "widgets on screen" lines at a spectate start (the first 10 a game run)
    uint32_t timelinesBlocked_ = 0;             // a stand-in's own timeline callbacks blocked (BlockStandInTimeline), per health window
    static constexpr size_t kFrameSamples = 4096;   // 5 s of frames at up to ~800 fps; the rest of a window is not sampled
    std::vector<double> frameMs_, tickMs_;
    std::array<uint64_t, kPumpParts> pumpLookups_{};
    std::array<double, kPumpParts> pumpMs_{}, pumpMaxMs_{};   // Timed: per part, this window
    bool sampling_ = false;
    uint64_t vmNsAtWindow_ = 0;
    uint64_t peNsAtWindow_ = 0, peCallsAtWindow_ = 0, peAllAtWindow_ = 0;
    uint64_t lookupsAtWindow_ = 0, lookupMissesAtWindow_ = 0;
};

// Never deleted. Its destructor would run at process exit, after the game has unloaded steam_api64.dll, and the
// transports' Stop() would call into the unloaded DLL: a crash on every quit after a Steam session (public #6, #7).
SessionImpl* g_session = nullptr;
UClass* g_gameModeClass = nullptr;
}  // namespace

bool StartMod(const Config& cfg) {
    if (!Init()) return false;
    game::SetStandInGripLock(cfg.standInGripLock);
    game::SetStandInScabbardFix(cfg.scabbardFix);
    game::SetAllowBuildMismatch(cfg.allowBuildMismatch);
    report::Configure(cfg.reports, cfg.reportUrl);
    update::Start(cfg.updateCheck);   // a worker thread; the menu page and UpdateNoticeTick show the result
    g_session = new SessionImpl(cfg);
    if (!g_session->Start()) return false;
    if (int port = CmdArgInt(CommandLineUtf8(), "hsmbridge", 0, 1024, 65535)) agent::Start(uint16_t(port));
    // Pump the session from the GameMode's Blueprint tick (game thread, every frame, menus included).
    // Widgets keep ticking while the game is paused: let them pump the agent bridge too, and lift a match's pause.
    HookFunctionsMatching([](const std::string& n) { return n == "Tick"; }, [](UObject*, UFunction*, void*) {
        g_session->LiftPauseFromWidgetTick();
        g_session->PauseReportTick();
        g_session->PumpAgentEarly();
        return false;
    });
    HookFunctionsMatching([](const std::string& n) { return n == "ReceiveTick"; }, [](UObject* obj, UFunction* fn, void* params) {
        if (g_session->IsTickBlocked(obj)) return true;   // a stand-in: never ticks
        static int seenClasses = 0;
        if (!g_gameModeClass && seenClasses < 12) {     // startup diagnostics: which Blueprints tick before the game mode
            static std::vector<UClass*> seen;
            UClass* c = ClassOf(obj);
            if (std::find(seen.begin(), seen.end(), c) == seen.end()) { seen.push_back(c); ++seenClasses; HSM_LOG("session: first tick of %s", ObjectName(reinterpret_cast<UObject*>(c)).c_str()); }
        }
        if (!g_gameModeClass) {
            UClass* c = ClassOf(obj);
            if (!SameNameAscii(ObjectName(reinterpret_cast<UObject*>(c)), kGameModeClass)) { g_session->PumpAgentEarly(); return false; }
            g_gameModeClass = c;
        }
        if (ClassOf(obj) != g_gameModeClass) { g_session->PumpAgentEarly(); return false; }
        float dt = 0.f;
        if (FProperty* p = FindProperty(fn, "DeltaSeconds")) std::memcpy(&dt, static_cast<uint8_t*>(params) + PropertyOffset(p), sizeof dt);
        if (!(dt > 0.f && dt < 1.f)) dt = 1.f / 60.f;
        g_session->ProfilerTick(true);
        NoteGameCarriedOn();
        const auto t0 = std::chrono::steady_clock::now();
        ue::prof::Scope scope("HSMMod session tick", ue::prof::Kind::kMod);
        g_session->Tick(obj, dt);
        const auto spent = std::chrono::steady_clock::now() - t0;
        DiscountHookTime(uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(spent).count()));   // reported as the mod tick
        g_session->NoteTickCost(std::chrono::duration<double, std::milli>(spent).count());
        return false;
    });
    // A stand-in's own timelines tick with its TimelineComponents, not with the blocked ReceiveTick. These ones run the
    // Willie's own logic, which on a stand-in is the owner's to run and stream:
    //   Timeline_0/1 (Willie_BP.cpp:21168, 21232), the grip check: with the stand-in's tonus at zero, the hand's constraint
    //     force soon beats its strength and the hand lets go: Set Up Right/Left Hand Weapon with no weapon (a drop, left
    //     floating by the stand-in's gravity-off weapon), Release Grab, or Get Damage on the hand;
    //   Timeline (21139) and Timeline_10 (21297, 21302), waking up: they clear Invulnerable, and Timeline_10 overwrites
    //     Consciousness.
    HookFunctionsMatching([](const std::string& n) {
        return n == "Timeline_0__UpdateFunc" || n == "Timeline_1__UpdateFunc" || n == "Timeline__UpdateFunc" ||
               n == "Timeline_10__UpdateFunc" || n == "Timeline_10__FinishedFunc";
    }, [](UObject* obj, UFunction*, void*) { return g_session->BlockStandInTimeline(obj); });
    HookFunctionsMatching([](const std::string& n) {
        return n.rfind("InpActEvt_SloMo_", 0) == 0 || n.rfind("InpActEvt_PhotoMode_", 0) == 0;
    }, [](UObject*, UFunction* fn, void*) {
        return g_session->BlockTimeInput(ObjectName(reinterpret_cast<UObject*>(fn)).rfind("InpActEvt_SloMo_", 0) == 0 ? "slow motion" : "photo mode");
    });
    HookFunctionsMatching([](const std::string& n) {   // Arena_Cutting_Map.cpp:54: the drop off the edge
        return n.rfind("BndEvt__Arena_Cutting_Map_TriggerVolume_0_", 0) == 0;
    }, [](UObject*, UFunction* fn, void* params) { return g_session->CuttingRingOut(fn, params); });
    if (!InstallProcessEventHook()) return false;
    HSM_LOG("session: started (%s)", RoleName(cfg.role));
    return true;
}

}  // namespace hsm
