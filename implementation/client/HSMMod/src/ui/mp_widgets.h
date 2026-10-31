// The multiplayer widgets shared by the multiplayer page (ui/menu.cpp) and the in-match Esc menu (ui/pause.cpp): the
// page's card look, the voice settings block, and the Esc menu's panel (players with mute, volume, kick and votekick,
// voice, network). What they show is decided in match/hsm_mp_scene.h; this maps it to UMG widgets. Game thread only.
#pragma once
#include "ui/umg.h"
#include "match/hsm_mp_scene.h"
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace hsm {
class Session;
namespace ui::mp {

// Everything clickable: an invisible Button around a frame around a face (the multiplayer page's look).
struct Card {
    Button button;
    Panel frame, face;
    bool selected = false, live = true;
    float glow = 0;                           // the hover fade, 0..1 (Paint)
    explicit operator bool() const { return bool(button) && bool(frame) && bool(face); }
};
struct Flat { Card card; Text label; };
Rgba Mix(Rgba a, Rgba b, float t);            // a to b by t

// Makes the page's widgets in one kit's tree.
class Maker {
public:
    Maker() = default;
    explicit Maker(Kit* kit) : kit_(kit) {}
    Kit& kit() { return *kit_; }
    Text NewText(const std::string& text, float size, Rgba color);
    Text Section(Box& column, const std::string& text, float padTop);
    Card MakeCard(const Widget& body, Margin pad);
    Flat MakeFlat(const std::string& text, float size, Margin pad = {16, 7, 16, 7});
private:
    Kit* kit_ = nullptr;
};

void Paint(Card& c);                          // per frame: the frame and face for the card's state and the mouse
bool Clicked(Card& c);
void Live(Flat& f, bool on);                  // clickable, or dimmed
Slot AddV(Box& column, const Widget& child, float padTop);
Slot AddH(Box& row, const Widget& child, float fill, float padRight);   // fill 0: the child's own width

// The voice settings: voice chat reach (the host's), microphone mode, device, threshold or push-to-talk key, volume,
// HEAR MYSELF, and a level meter. Changes go straight to the session (and HSMMod.ini).
class VoiceBlock {
public:
    bool Build(Maker& m, Box& column, float padTop);
    void Tick(Session& session);              // each frame while shown
    void Show(bool on);
    bool Binding() const { return binding_; }
    void StopBinding() { binding_ = false; }
private:
    struct RowView { Box box; Text label, value, hint; Flat prev, next, bind; };
    void PollBinding(Session& session, const MpVoice& v);
    Box root_;
    std::array<RowView, size_t(VoiceRow::Count)> rows_;
    Bar meter_;
    Icon mark_;
    Slot markSlot_{nullptr};
    float markAt_ = -1;
    Text meterText_, note_;
    bool binding_ = false;
    std::vector<int> heldAtBind_;             // keys already down when binding began (they must be let go first)
};

// The Esc menu in a multiplayer match: a full-screen panel on the stock pause menu, which GAME MENU shows again.
class EscPanel {
public:
    enum class Action : uint8_t { None, Resume, GameMenu, ReportBug, Leave };
    bool Build(Kit& kit, Canvas root);
    Action Tick(Session& session, bool reportOn);   // each frame while shown
    void Show(bool on);
    bool Shown() const { return shown_; }
    explicit operator bool() const { return bool(panel_); }
private:
    static constexpr size_t kRows = 9;
    struct PlayerView { Box row; Icon talk; Text name, detail, volume; Flat mute, down, up, kick, ban, vote, voteBan; };
    void Refresh(Session& session);
    void HandlePlayers(Session& session, uint32_t now);
    Maker m_;
    Panel panel_;
    bool shown_ = false;
    Text title_, sub_;
    std::array<Flat, 3> tabs_;
    std::array<Box, 3> pages_;
    int tab_ = 0;
    Panel voteBar_;
    Text voteText_;
    Flat voteYes_, voteNo_;
    std::array<PlayerView, kRows> rows_;
    std::vector<MpPlayer> players_;           // what the rows show: their slots and listen settings
    MpVote vote_;
    Text alone_;
    VoiceBlock voice_;
    std::array<Text, 10> netLines_;
    Flat resume_, gameMenu_, report_, help_, leave_;   // help_: HELP IMPROVE THE MOD (0.8.0-pre7)
    uint32_t nextData_ = 0, leaveArmedAt_ = 0, confirmAt_ = 0;
    int confirmRow_ = -1;
    bool confirmBan_ = false;
};

}  // namespace ui::mp
}  // namespace hsm
