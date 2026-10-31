// Steam lobby lifecycle for the in-game Multiplayer screen. The lobby is the source of truth for who is in the
// session; the session layer mirrors membership into the transport. Game thread only.
#pragma once
#include "match/hsm_match.h"
#include "steam/steam_api.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace hsm::game {

constexpr int kMaxLobbyMembers = 8;   // players: == kMaxPlayers (session/hsm_roster.h; static_assert in session.cpp).
                                      // A role=server lobby has one member more: the server, which is not a player.
constexpr const char* kModVersion = "0.8.0-pre22";   // bump with every release tag
// This mod's build: kModVersion plus the git describe build.sh passed (session/hsm_build.h). Players compare it.
std::string ModBuild();
// allow_build_mismatch in HSMMod.ini (config.h): the host may start a match with other builds in. Set once at start.
void SetAllowBuildMismatch(bool on);
bool AllowBuildMismatch();

struct LobbyMember { steam::SteamId id = 0; std::string name; bool ready = false; int team = -1; int hostOverride = -1; bool host = false; std::string build; };

struct LobbyListing {
    steam::SteamId id = 0;
    std::string name, map, mode;
    int players = 0;      // players: a role=server lobby's server is not counted
    bool compatible = false;
    std::string reason;   // why it is not joinable, if !compatible
};

class Lobby {
public:
    enum class State { Offline, Idle, Creating, Joining, InLobby };

    // Membership callbacks for the session (fired on the game thread).
    std::function<void(steam::SteamId)> onMemberJoined, onMemberLeft;
    std::function<void()> onEntered, onLeft;
    // The host left and Steam made another member the owner. True: the session migrates to the new owner and the lobby
    // stays; false (or unset): leave the lobby, as before 0.7.0-pre5.
    std::function<bool(steam::SteamId oldOwner, steam::SteamId newOwner)> onOwnerChanged;
    std::function<void(steam::SteamId from, const std::string& text)> onChat;   // a lobby chat line (ours too); the text is unchecked
    std::function<void(const std::string& name, bool joined)> onPresence;       // someone else came or went, after we entered

    void Tick();                                   // drives Steam init + callback processing

    void Host(steam::LobbyType type, bool server = false);   // server: role=server, one member more (hsm_srv=1)
    void Join(steam::SteamId lobby);
    void Leave();
    void Invite();
    bool SendChat(const std::string& text);        // to every member (Steam lobby chat); false: not in a lobby or Steam refused
    void SetType(steam::LobbyType type);
    void SetReady(bool ready);
    void SetPublicState(const std::string& state, const std::string& map, const std::string& mode);
    void RefreshList();
    // Match settings (lobby data, host writes) and teams (own pick in member data, host moves in lobby data).
    MatchConfig config() const;
    void SetConfig(const MatchConfig& c);
    std::string MapPick() const;                    // host's chosen level (package path), empty until picked
    void SetMapPick(const std::string& path);
    bool Progression() const;                       // the host chose the Progression card (hsm_prog)
    void SetProgression(bool on);
    void SetTeamPick(int team);
    void SetOverride(steam::SteamId member, int team);
    void ClearOverrides();

    State state() const { return state_; }
    bool IsHost() const { return state_ == State::InLobby && owner_ == steam::MySteamId(); }
    bool InLobby() const { return state_ == State::InLobby; }
    bool ServerLobby() const;                       // a role=server lobby: the owner is the server, not a player
    int PlayerCount() const;                        // members, minus the server in a role=server lobby
    steam::SteamId id() const { return lobby_; }
    steam::SteamId owner() const { return owner_; }
    steam::LobbyType type() const { return type_; }
    const std::vector<LobbyMember>& members() const { return members_; }
    const std::vector<LobbyListing>& listings() const { return listings_; }
    const std::string& status() const { return status_; }

private:
    void Enter(steam::SteamId lobby);
    void RefreshMembers();
    void SetStatus(const std::string& s);
    LobbyListing Describe(steam::SteamId lobby) const;
    bool server_ = false;   // Host(type, true): this machine runs role=server
    State state_ = State::Offline;
    steam::SteamId lobby_ = 0, owner_ = 0, joining_ = 0;   // joining_: the lobby Join asked for, until its enter
    steam::LobbyType type_ = steam::LobbyType::FriendsOnly;
    std::vector<LobbyMember> members_;
    std::vector<LobbyListing> listings_;
    std::string status_ = "Steam not ready";
    bool launchChecked_ = false;
};

}  // namespace hsm::game
