#include "ui/mp_widgets.h"
#include "gameplay/session.h"
#include "match/hsm_hud_style.h"
#include "log.h"
#include <windows.h>
#include <algorithm>

namespace hsm::ui::mp {
namespace {
constexpr float kGlowStep = 0.18f;            // the hover fade: a share of the way per frame (about 0.1 s at 60 fps)
constexpr Rgba kClear{1, 1, 1, 0};
constexpr Rgba kNone{0, 0, 0, 0};
constexpr float kMeterWidth = 300;
constexpr uint32_t kConfirmMs = 4000;

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
}  // namespace

// ---------------------------------------------------------------- the card look

Text Maker::NewText(const std::string& text, float size, Rgba color) {
    Text t = kit_ ? kit_->MakeText() : Text{};
    if (!t) return t;
    t.Set(text);
    t.Font(size);
    t.Color(color);
    return t;
}

Text Maker::Section(Box& column, const std::string& text, float padTop) {
    Text t = NewText(text, 18, style::kBloodInk);
    if (t) AddV(column, t, padTop);
    return t;
}

Card Maker::MakeCard(const Widget& body, Margin pad) {
    if (!kit_) return Card{};
    Card c{kit_->MakeButton(), kit_->MakePanel(), kit_->MakePanel()};
    if (!c || !body) { HSM_LOG("ui: card widgets could not be made"); return Card{}; }
    c.button.Background(kClear);
    c.button.Content(c.frame).Padding({0, 0, 0, 0}).Align(HAlign::Fill, VAlign::Fill);
    c.frame.Padding({2, 2, 2, 2});
    c.frame.Content(c.face);
    c.face.Padding(pad);
    c.face.Content(body);
    return c;
}

Flat Maker::MakeFlat(const std::string& text, float size, Margin pad) {
    Flat f;
    f.label = NewText(text, size, style::kBone);
    f.card = MakeCard(f.label, pad);
    return f;
}

Rgba Mix(Rgba a, Rgba b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t}; }

void Paint(Card& c) {
    if (!c) return;
    const bool hover = c.live && c.button.Hovered();
    c.glow += ((hover ? 1.f : 0.f) - c.glow) * kGlowStep;
    if (c.glow < 0.01f) c.glow = 0; else if (c.glow > 0.99f) c.glow = 1;
    c.frame.Color(c.selected ? style::kMenuAccent : Mix(style::kMenuFrame, style::kMenuFrameHover, c.glow));
    c.face.Color(Mix(style::kMenuCard, style::kMenuCardHover, c.glow));
}

bool Clicked(Card& c) { return c && c.button.Clicked(); }

void Live(Flat& f, bool on) {
    if (!f.card) return;
    f.card.live = on;
    f.card.button.Enabled(on);
    f.card.button.Opacity(on ? 1.f : 0.3f);
}

Slot AddV(Box& column, const Widget& child, float padTop) { return column.Add(child).Padding({0, padTop, 0, 0}); }

Slot AddH(Box& row, const Widget& child, float fill, float padRight) {
    Slot s = row.Add(child);
    if (fill > 0) s.Fill(fill); else s.Auto();
    s.Padding({0, 0, padRight, 0});
    return s;
}

// ---------------------------------------------------------------- the voice block

bool VoiceBlock::Build(Maker& m, Box& column, float padTop) {
    Kit& kit = m.kit();
    root_ = kit.MakeColumn();
    if (!root_) { HSM_LOG("ui: voice block widgets could not be made"); return false; }
    AddV(column, root_, padTop);
    m.Section(root_, "VOICE CHAT", 0);
    for (RowView& v : rows_) {
        v.box = kit.MakeColumn();
        Box row = kit.MakeRow();
        Fixed valueBox = kit.MakeFixed();
        v.label = m.NewText("", 17, style::kBone); v.value = m.NewText("", 17, style::kBone); v.hint = m.NewText("", 13, style::kIron);
        v.prev = m.MakeFlat("<", 17, {14, 2, 14, 2}); v.next = m.MakeFlat(">", 17, {14, 2, 14, 2});
        v.bind = m.MakeFlat("CHANGE", 15, {12, 3, 12, 3});
        if (!v.box || !row || !valueBox || !v.label || !v.value || !v.hint || !v.prev.card || !v.next.card || !v.bind.card) { v = RowView{}; continue; }
        v.value.Align(Justify::Center);
        v.hint.Wrap(true);
        valueBox.Width(230);
        valueBox.Content(v.value).Align(HAlign::Fill, VAlign::Center);
        AddH(row, v.label, 1, 12).Align(HAlign::Fill, VAlign::Center);
        AddH(row, v.prev.card.button, 0, 6);
        AddH(row, valueBox, 0, 6).Align(HAlign::Fill, VAlign::Center);
        AddH(row, v.next.card.button, 0, 6);
        AddH(row, v.bind.card.button, 0, 0).Align(HAlign::Left, VAlign::Center);
        AddV(v.box, row, 0);
        AddV(v.box, v.hint, 2);
        AddV(root_, v.box, 8);
    }
    Box meterRow = kit.MakeRow();
    Fixed meterBox = kit.MakeFixed();
    Box stack = kit.MakeStack();
    meter_ = kit.MakeBar();
    mark_ = kit.MakeIcon();
    meterText_ = m.NewText("", 14, style::kIron);
    if (meterRow && meterBox && stack && meter_ && mark_ && meterText_) {
        meterBox.Width(kMeterWidth);
        meterBox.Height(14);
        meterBox.Content(stack).Align(HAlign::Fill, VAlign::Fill);
        stack.Add(meter_).Align(HAlign::Fill, VAlign::Fill);
        markSlot_ = stack.Add(mark_);
        markSlot_.Align(HAlign::Left, VAlign::Fill);
        mark_.Size({3, 14});
        mark_.Color(style::kBone);
        AddH(meterRow, meterBox, 0, 12).Align(HAlign::Left, VAlign::Center);
        AddH(meterRow, meterText_, 1, 0).Align(HAlign::Fill, VAlign::Center);
        AddV(root_, meterRow, 12);
    } else {
        meter_ = Bar{};
    }
    note_ = m.NewText("", 14, style::kBloodInk);
    if (note_) { note_.Wrap(true); AddV(root_, note_, 8); }
    return true;
}

void VoiceBlock::Show(bool on) {
    if (root_) root_.Show(on);
    if (!on) binding_ = false;
}

// Push to talk: the next key pressed (a letter, a digit, F1-F12 or a side mouse button) becomes the key; Esc cancels.
void VoiceBlock::PollBinding(Session& session, const MpVoice& v) {
    if (v.mic != voice::Mic::PushToTalk || KeyDown(VK_ESCAPE)) { binding_ = false; return; }
    for (int vk : voice::VoiceKeyCandidates()) {
        const bool down = KeyDown(vk);
        const auto held = std::find(heldAtBind_.begin(), heldAtBind_.end(), vk);
        if (held != heldAtBind_.end()) { if (!down) heldAtBind_.erase(held); continue; }
        if (!down) continue;
        MpVoice n = v;
        n.key = vk;
        session.SetVoicePrefs(n);
        HSM_LOG("voice: push-to-talk key set to %s from the menu", voice::VoiceKeyIni(vk).c_str());
        binding_ = false;
        return;
    }
}

void VoiceBlock::Tick(Session& session) {
    if (!root_) return;
    MpVoice v = session.VoiceView();
    if (binding_) { PollBinding(session, v); if (!binding_) v = session.VoiceView(); }
    const VoiceScene sc = BuildVoiceScene(v, binding_);
    for (size_t i = 0; i < rows_.size(); ++i) {
        RowView& r = rows_[i];
        if (!r.box) continue;
        Paint(r.prev.card); Paint(r.next.card); Paint(r.bind.card);
        int dir = Clicked(r.prev.card) ? -1 : 0;
        if (Clicked(r.next.card)) dir = 1;
        const bool bindClicked = Clicked(r.bind.card);
        const VoiceRowView* row = nullptr;
        for (const VoiceRowView& x : sc.rows) if (size_t(x.id) == i) row = &x;
        r.box.Show(row != nullptr);
        if (!row) continue;
        const bool key = row->id == VoiceRow::Key;
        r.label.Set(row->label);
        r.label.Color(row->enabled ? style::kBone : style::kIron);
        r.value.Set(row->value);
        r.value.Color(key && binding_ ? style::kBloodInk : style::kBone);
        r.hint.Set(row->hint);
        r.hint.Show(!row->hint.empty());
        r.prev.card.button.Show(!key);
        r.next.card.button.Show(!key);
        r.bind.card.button.Show(key);
        r.bind.label.Set(binding_ ? "CANCEL" : "CHANGE");
        r.bind.card.selected = binding_;
        Live(r.prev, row->enabled);
        Live(r.next, row->enabled);
        if (key && bindClicked) {
            binding_ = !binding_;
            heldAtBind_.clear();
            if (binding_) for (int vk : voice::VoiceKeyCandidates()) if (KeyDown(vk)) heldAtBind_.push_back(vk);
        }
        if (!dir || !row->enabled || key) continue;
        const MpVoice n = StepVoice(v, row->id, dir);
        switch (row->id) {
            case VoiceRow::Reach: session.SetVoiceReach(n.reach); break;
            case VoiceRow::Monitor: session.SetVoiceMonitor(n.monitor); break;
            default: session.SetVoicePrefs(n); break;
        }
    }
    if (meter_) {
        meter_.Percent(sc.meter);
        meter_.FillColor(sc.meterHot ? style::kOk : style::kIron);
        mark_.Visibility(sc.showThreshold ? Vis::HitTestInvisible : Vis::Hidden);
        const float at = std::clamp(sc.threshold, 0.f, 1.f) * (kMeterWidth - 3);
        if (markSlot_ && std::abs(at - markAt_) > 0.5f) { markAt_ = at; markSlot_.Padding({at, 0, 0, 0}); }
    }
    if (meterText_) meterText_.Set(sc.meterText);
    if (note_) { note_.Set(sc.note); note_.Show(!sc.note.empty()); }
}

// ---------------------------------------------------------------- the Esc menu

bool EscPanel::Build(Kit& kit, Canvas root) {
    m_ = Maker(&kit);
    panel_ = kit.MakePanel();
    Box page = kit.MakeColumn(), header = kit.MakeRow(), tabs = kit.MakeRow(), body = kit.MakeScroll(), footer = kit.MakeRow();
    Fixed rule = kit.MakeFixed();
    Panel ruleLine = kit.MakePanel();
    for (Box& p : pages_) p = kit.MakeColumn();
    bool pagesOk = true;
    for (Box& p : pages_) pagesOk = pagesOk && bool(p);
    if (!root || !panel_ || !page || !header || !tabs || !body || !footer || !rule || !ruleLine || !pagesOk) {
        HSM_LOG("ui: Esc menu widgets could not be made");
        panel_ = Panel{};
        return false;
    }
    root.Add(panel_).Anchor({{0, 0}, {1, 1}}, {0, 0}).Offsets({0, 0, 0, 0}).Z(1000);
    panel_.Color({style::kSmoke.r, style::kSmoke.g, style::kSmoke.b, 0.97f});
    panel_.Padding({72, 44, 72, 36});
    panel_.Content(page);

    AddV(page, header, 0);
    title_ = m_.NewText("MULTIPLAYER", 40, style::kBone);
    sub_ = m_.NewText("", 17, style::kIron);
    if (title_) { title_.Font(40, 1, {0, 0, 0, 0.85f}); AddH(header, title_, 0, 28).Align(HAlign::Left, VAlign::Center); }
    if (sub_) AddH(header, sub_, 1, 0).Align(HAlign::Left, VAlign::Center);
    rule.Height(3);
    rule.Content(ruleLine);
    ruleLine.Color(style::kBlood);
    AddV(page, rule, 12);

    AddV(page, tabs, 14);
    const char* const tabNames[3] = {"PLAYERS", "VOICE", "NETWORK"};
    for (size_t i = 0; i < tabs_.size(); ++i) {
        tabs_[i] = m_.MakeFlat(tabNames[i], 18);
        if (tabs_[i].card) AddH(tabs, tabs_[i].card.button, 0, 8);
    }
    AddV(page, body, 14).Fill();
    for (Box& p : pages_) body.Add(p);

    // PLAYERS: the vote running now, then a row per player.
    Box& players = pages_[0];
    voteBar_ = kit.MakePanel();
    Box vrow = kit.MakeRow();
    voteText_ = m_.NewText("", 18, style::kBone);
    voteYes_ = m_.MakeFlat("YES", 17);
    voteNo_ = m_.MakeFlat("NO", 17);
    if (voteBar_ && vrow && voteText_ && voteYes_.card && voteNo_.card) {
        voteText_.Wrap(true);
        voteBar_.Color({style::kBlood.r, style::kBlood.g, style::kBlood.b, 0.35f});
        voteBar_.Padding({14, 8, 14, 8});
        voteBar_.Content(vrow);
        AddH(vrow, voteText_, 1, 12).Align(HAlign::Fill, VAlign::Center);
        AddH(vrow, voteYes_.card.button, 0, 8).Align(HAlign::Left, VAlign::Center);
        AddH(vrow, voteNo_.card.button, 0, 0).Align(HAlign::Left, VAlign::Center);
        AddV(players, voteBar_, 0);
        voteBar_.Show(false);
    } else {
        voteBar_ = Panel{};
    }
    for (PlayerView& r : rows_) {
        r.row = kit.MakeRow();
        Box names = kit.MakeColumn();
        Fixed volBox = kit.MakeFixed();
        r.talk = kit.MakeIcon();
        r.name = m_.NewText("", 18, style::kBone); r.detail = m_.NewText("", 14, style::kIron); r.volume = m_.NewText("", 15, style::kBone);
        const Margin small{12, 4, 12, 4};
        r.mute = m_.MakeFlat("MUTE", 15, small); r.down = m_.MakeFlat("-", 15, small); r.up = m_.MakeFlat("+", 15, small);
        r.kick = m_.MakeFlat("KICK", 15, small); r.ban = m_.MakeFlat("BAN", 15, small); r.vote = m_.MakeFlat("VOTE KICK", 15, small);
        r.voteBan = m_.MakeFlat("VOTE BAN", 15, small);
        if (!r.row || !names || !volBox || !r.talk || !r.name || !r.detail || !r.volume || !r.mute.card || !r.down.card || !r.up.card ||
            !r.kick.card || !r.ban.card || !r.vote.card || !r.voteBan.card) {
            r = PlayerView{};
            continue;
        }
        r.talk.Size({10, 10});
        r.volume.Align(Justify::Center);
        volBox.Width(64);
        volBox.Content(r.volume).Align(HAlign::Fill, VAlign::Center);
        AddV(names, r.name, 0);
        AddV(names, r.detail, 1);
        AddH(r.row, r.talk, 0, 10).Align(HAlign::Left, VAlign::Center);
        AddH(r.row, names, 1, 12).Align(HAlign::Fill, VAlign::Center);
        for (Flat* f : {&r.mute, &r.down}) AddH(r.row, f->card.button, 0, 6).Align(HAlign::Left, VAlign::Center);
        AddH(r.row, volBox, 0, 6).Align(HAlign::Fill, VAlign::Center);
        AddH(r.row, r.up.card.button, 0, 18).Align(HAlign::Left, VAlign::Center);
        for (Flat* f : {&r.kick, &r.ban, &r.vote, &r.voteBan}) AddH(r.row, f->card.button, 0, 6).Align(HAlign::Left, VAlign::Center);
        AddV(players, r.row, 8);
        r.row.Show(false);
    }
    alone_ = m_.NewText("Nobody else is here yet.", 16, style::kIron);
    if (alone_) AddV(players, alone_, 12);

    // VOICE: the same block as the multiplayer page.
    voice_.Build(m_, pages_[1], 0);

    // NETWORK: read-only lines.
    for (Text& t : netLines_) {
        t = m_.NewText("", 17, style::kBone);
        if (t) { AddV(pages_[2], t, 6); t.Show(false); }
    }

    AddV(page, footer, 16);
    resume_ = m_.MakeFlat("RESUME", 18);
    gameMenu_ = m_.MakeFlat("GAME MENU", 18);
    report_ = m_.MakeFlat("REPORT A BUG", 18);
    help_ = m_.MakeFlat("HELP IMPROVE THE MOD: OFF", 18);
    leave_ = m_.MakeFlat("LEAVE SESSION", 18);
    for (Flat* f : {&resume_, &gameMenu_, &report_, &help_, &leave_}) if (f->card) AddH(footer, f->card.button, 0, 10).Align(HAlign::Left, VAlign::Center);
    panel_.Show(false);
    return true;
}

void EscPanel::Show(bool on) {
    if (!panel_) return;
    shown_ = on;
    panel_.Show(on);
    if (!on) voice_.StopBinding();
    nextData_ = 0;
    leaveArmedAt_ = 0;
    confirmRow_ = -1;
}

// The players, the vote and the network lines, four times a second (names and pings change slowly).
void EscPanel::Refresh(Session& session) {
    players_ = session.Players();
    vote_ = session.Vote();
    const std::vector<PlayerRowView> rows = BuildPlayerRows(players_, vote_);
    const uint32_t now = GetTickCount();
    if (confirmRow_ >= 0 && now - confirmAt_ >= kConfirmMs) confirmRow_ = -1;
    for (size_t i = 0; i < rows_.size(); ++i) {
        PlayerView& r = rows_[i];
        if (!r.row) continue;
        r.row.Show(i < rows.size());
        if (i >= rows.size()) continue;
        const PlayerRowView& v = rows[i];
        r.name.Set(v.name);
        r.name.Color(v.voteTarget ? style::kBloodInk : style::kBone);
        r.detail.Set(v.detail);
        r.detail.Show(!v.detail.empty());
        r.talk.Color(v.talking ? style::kOk : kNone);
        r.mute.card.button.Show(v.showMute);
        r.mute.label.Set(v.muteLabel);
        r.mute.card.selected = v.muteLabel == "UNMUTE";
        r.down.card.button.Show(v.showVolume);
        r.up.card.button.Show(v.showVolume);
        r.volume.Show(v.showVolume);
        r.volume.Set(v.volume);
        r.kick.card.button.Show(v.showKick);
        r.ban.card.button.Show(v.showBan);
        r.vote.card.button.Show(v.showVote);
        r.voteBan.card.button.Show(v.showVoteBan);
        const bool confirm = confirmRow_ == int(i);
        r.kick.label.Set(confirm && !confirmBan_ ? "CONFIRM KICK" : "KICK");
        r.ban.label.Set(confirm && confirmBan_ ? "CONFIRM BAN" : "BAN");
        r.kick.card.selected = confirm && !confirmBan_;
        r.ban.card.selected = confirm && confirmBan_;
    }
    if (alone_) alone_.Show(players_.size() <= 1);
    if (voteBar_) {
        const VoteBar b = BuildVoteBar(vote_);
        voteBar_.Show(b.show);
        voteText_.Set(b.text);
        voteYes_.card.button.Show(b.showButtons);
        voteNo_.card.button.Show(b.showButtons);
        voteYes_.card.selected = b.yesOn;
        voteNo_.card.selected = b.noOn;
    }
    const std::vector<std::string> lines = session.NetworkLines();
    for (size_t i = 0; i < netLines_.size(); ++i) {
        if (!netLines_[i]) continue;
        netLines_[i].Show(i < lines.size());
        if (i < lines.size()) netLines_[i].Set(lines[i]);
    }
    if (sub_) sub_.Set(lines.empty() ? std::string() : lines[0] + ", " + std::to_string(players_.size()) + (players_.size() == 1 ? " player" : " players"));
}

void EscPanel::HandlePlayers(Session& session, uint32_t now) {
    for (size_t i = 0; i < rows_.size(); ++i) {
        PlayerView& r = rows_[i];
        if (!r.row) continue;
        for (Flat* f : {&r.mute, &r.down, &r.up, &r.kick, &r.ban, &r.vote, &r.voteBan}) Paint(f->card);
        const bool mute = Clicked(r.mute.card), down = Clicked(r.down.card), up = Clicked(r.up.card);
        const bool kick = Clicked(r.kick.card), ban = Clicked(r.ban.card), vote = Clicked(r.vote.card), voteBan = Clicked(r.voteBan.card);
        if (i >= players_.size()) continue;
        MpPlayer& p = players_[i];
        if (mute) { p.muted = !p.muted; session.SetPlayerListen(p.slot, p.muted, p.volumePct); nextData_ = 0; }
        if (down || up) { p.volumePct = voice::StepVolumePct(p.volumePct, up ? 1 : -1); session.SetPlayerListen(p.slot, p.muted, p.volumePct); nextData_ = 0; }
        if ((kick || ban) && p.canKick) {
            if (confirmRow_ == int(i) && confirmBan_ == ban && now - confirmAt_ < kConfirmMs) { session.KickPlayer(p.slot, ban); confirmRow_ = -1; }
            else { confirmRow_ = int(i); confirmBan_ = ban; confirmAt_ = now; }
            nextData_ = 0;
        }
        if (vote && p.canVote && !vote_.active) { session.VoteKickPlayer(p.slot); nextData_ = 0; }
        if (voteBan && p.canVoteBan && !vote_.active) { session.VoteBanPlayer(p.slot); nextData_ = 0; }
    }
    if (voteBar_) {
        Paint(voteYes_.card); Paint(voteNo_.card);
        const bool yes = Clicked(voteYes_.card), no = Clicked(voteNo_.card);
        if ((yes || no) && vote_.active && vote_.mine >= -1) { session.CastVote(yes); nextData_ = 0; }
    }
}

EscPanel::Action EscPanel::Tick(Session& session, bool reportOn) {
    if (!panel_ || !shown_) return Action::None;
    const uint32_t now = GetTickCount();
    for (size_t i = 0; i < tabs_.size(); ++i) {
        if (!tabs_[i].card) continue;
        Paint(tabs_[i].card);
        if (Clicked(tabs_[i].card)) { tab_ = int(i); nextData_ = 0; }
        tabs_[i].card.selected = tab_ == int(i);
        tabs_[i].label.Color(tab_ == int(i) ? style::kBloodInk : style::kBone);
        pages_[i].Show(tab_ == int(i));
    }
    if (tab_ != 1) voice_.StopBinding();
    HandlePlayers(session, now);
    if (!nextData_ || int32_t(now - nextData_) >= 0) { nextData_ = now + 250; Refresh(session); }
    if (tab_ == 1) voice_.Tick(session);

    for (Flat* f : {&resume_, &gameMenu_, &report_, &help_, &leave_}) Paint(f->card);
    if (report_.card) report_.card.button.Show(reportOn);
    if (help_.card) {                           // the multiplayer page's switch: on gets the blood frame and a green label
        const bool help = session.HelpImprove();
        help_.card.button.Show(reportOn);
        help_.label.Set(help ? "HELP IMPROVE THE MOD: ON" : "HELP IMPROVE THE MOD: OFF");
        help_.label.Color(help ? style::kOk : style::kBone);
        help_.card.selected = help;
        if (reportOn && Clicked(help_.card)) { session.SetHelpImprove(!help); nextData_ = 0; }
    }
    const bool resume = Clicked(resume_.card), game = Clicked(gameMenu_.card), report = Clicked(report_.card), leave = Clicked(leave_.card);
    if (leaveArmedAt_ && now - leaveArmedAt_ >= kConfirmMs) leaveArmedAt_ = 0;
    if (leave_.card) {
        leave_.label.Set(leaveArmedAt_ ? "CLICK AGAIN TO LEAVE" : "LEAVE SESSION");
        leave_.card.selected = leaveArmedAt_ != 0;
    }
    if (resume) return Action::Resume;
    if (game) return Action::GameMenu;
    if (report && reportOn) return Action::ReportBug;
    if (leave) {
        if (leaveArmedAt_) { leaveArmedAt_ = 0; return Action::Leave; }
        leaveArmedAt_ = now ? now : 1;
    }
    return Action::None;
}

}  // namespace hsm::ui::mp
