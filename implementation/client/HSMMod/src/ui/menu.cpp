#include "ui/menu.h"
#include "ui/chat.h"
#include "ui/umg.h"
#include "ui/mp_widgets.h"
#include "util/hsm_names.h"
#include "gameplay/damage_pin.h"
#include "gameplay/mod_maps.h"
#include "gameplay/session.h"
#include "log.h"
#include "ue4ss_load.h"
#include "net/report_client.h"
#include "net/update_check.h"
#include "match/hsm_hud_style.h"
#include "match/hsm_lobby_scene.h"
#include "match/hsm_menu_pick.h"
#include "util/hsm_bmp.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <string>
#include <vector>

namespace hsm::ui {
using namespace hsm::ue;
namespace {

// The page is drawn in the menu palette ("Iron & Blood", shared/match/hsm_hud_style.h). Cards and their hover fade
// come from ui/mp_widgets, shared with the Esc menu.
using mp::Card;
using mp::Flat;
using mp::Paint;
constexpr float kChatHeight = 170;                // the chat log's frame, in px
constexpr Rgba kClear{1, 1, 1, 0};                  // a Button's own brush, hidden: the frame and face are ours
constexpr float kStartGlow = 0.2f;                  // START under the mouse: this far from the accent to its ink (text stays >= 4.5)

// The injected Multiplayer entry on the main menu: either a Button in the stock menu button's style (`plain`), or our
// own card (the page's look) when the stock entry cannot be copied.
struct TextButton {
    Button plain;
    Card card;
    Text label;
    Slot inner{nullptr};                     // the plain button's content slot: inset to the ribbon's flat middle
    Button& button() { return card ? card.button : plain; }
    explicit operator bool() const { return (bool(plain) || bool(card)) && bool(label); }
};
struct ModeView { Card card; Text name, blurb; };
struct MapView { Card card; Text name, tag; };
struct BrowserView { Card card; Text name, detail, players, note; };
struct ChatRow { Box row; Text name, text; };
struct MemberView { Card card; Icon swatch, avatar, ready; Text name, badge, team; };
struct HostView { Card card; Text name, blurb; };
struct TabView { Card card; Text label; Panel line; };   // MODE | RULES | MAP: the chosen one underlined in the accent

// A player's Steam avatar as an engine texture, made once and kept while the engine keeps it (an Image's brush holds
// it; one no row shows any more is collected, and made again when it is needed).
struct Avatar {
    steam::SteamId id = 0;
    UObject* texture = nullptr;
    UClass* cls = nullptr;                           // with index: the slot still holds this texture
    int32_t index = -1;
    ULONGLONG nextTry = 0;
    int tries = 0;
};
constexpr int kAvatarTries = 20;                     // once a second while Steam fetches it
struct StepperView { Box box; Text label, value, hint; Flat prev, next; };
constexpr size_t kTabCount = 3;                      // MODE, RULES, MAP
constexpr size_t kSectionCount = size_t(LobbySection::Count);

struct State {
    UObject* worldContext = nullptr;
    UClass* menuClass = nullptr;
    UObject* menu = nullptr;
    int32_t menuIndex = -1;
    ULONGLONG nextScan = 0, nextText = 0;
    bool built = false, panelOpen = false;
    Kit kit;
    mp::Maker maker;
    TextButton openButton;
    Panel panel;                             // the full-screen multiplayer page (a Border over the whole menu)
    Text title, me, badge, status, notice, updateNote, pillText, playersTitle, idleHint, hostOnly, waiting, summary;
    Panel pill, pillFrame;
    // Not in a lobby: HOST on the left, the browser on the right. In one: players and chat left, the tabs right.
    Box mainRow, idleLeft, inLeft, idleRight, inRight, settingsView;
    HostView hostFriends, hostPublic;
    Flat refresh, invite, readyUp, balance, leave, close, settingsOpen;
    std::array<Flat, kFilterChips> chips;
    Text browserCount, browserEmpty;
    BrowserFilter filter;                    // the chips' state, kept while the game runs
    std::array<TabView, kTabCount> tabs;
    std::array<Box, kTabCount> tabPages;
    size_t tab = 0;
    std::array<Text, kSectionCount> sectionTitles;
    std::array<Box, kSectionCount> sections;
    Text noMaps;
    bool settingsShown = false;
    Flat reportOpen, reportSend, reportCancel;   // REPORT A BUG, and the box it opens
    Text reportHow;                              // how to report from a fight (/bug)
    Box reportBox;
    EditBox reportText;
    Text reportNote;
    uint32_t reportSeen = 0;   // Session::ReportStatusSeq last shown in reportNote
    bool reportOpenNow = false;
    Flat helpToggle;                         // HELP IMPROVE THE MOD: ON/OFF (SETTINGS)
    mp::VoiceBlock voice;                    // VOICE CHAT: your own reach, knocked-out toggle, the microphone, the meter
    Text helpNote;
    Panel ask;                               // the one-time HELP IMPROVE THE MOD question over the whole menu (0.8.0-pre7)
    Flat askYes, askNo;
    bool askOpen = false, escDown = false;
    Box chatBox;                             // the lobby chat: CHAT, the last lines, the text box and SEND (only in a lobby)
    Box chatScroll;
    std::array<ChatRow, kChatLogLines> chatRows;
    EditBox chatText;
    Flat chatSend;
    uint32_t chatSeen = 0;                   // ChatLinesAdded() at the last draw
    bool chatFocused = false, enterDown = false;   // the box had the keyboard at the last frame; Enter was down
    std::vector<BrowserView> rows;           // kBrowserRows
    std::vector<uint64_t> rowLobbies;        // the Steam lobby each shown row joins
    std::vector<MemberView> members;         // kMaxLobbyMembers
    std::vector<steam::SteamId> memberIds;
    std::vector<Avatar> avatars;
    bool avatarFailLogged = false;
    std::vector<ModeView> modes;             // kModeOrder, then Brawl, then Progression
    std::vector<StepperView> steppers;       // one per LobbyRow, in its order
    std::vector<MapView> maps;               // the pickable maps, kMaps order, then the modded ones
    Card start;
    Text startLabel;
    LobbyScene scene;                        // the last drawn: what the clicks act on
} g;

bool StillAlive(UObject* o, int32_t index, UClass* cls) { return o && ObjectAt(index) == o && ClassOf(o) == cls; }

bool Clicked(TextButton& b) { return b.button().Clicked(); }
using mp::Clicked;                                  // a Card's click (TextButton's is above)

Slot AddToVerticalBox(Box& box, const Widget& child, float padTop) { return box.Add(child).Padding({0, padTop, 0, 0}); }

// A vertical box child that takes the space left over (the page body, a tab's page).
void FillVerticalBox(Box& box, const Widget& child, float padTop) { AddToVerticalBox(box, child, padTop).Fill(); }

// fill > 0: a share of the row; 0: the child's own width.
Slot AddToHorizontalBox(Box& box, const Widget& child, float fill, float padRight) {
    Slot s = box.Add(child);
    if (fill > 0) s.Fill(fill); else s.Auto();
    s.Padding({0, 0, padRight, 0});
    return s;
}

Text NewText(const std::string& text, float size, Rgba color) { return g.maker.NewText(text, size, color); }

// A small section label in the accent's ink (MATCH, PUBLIC LOBBIES, CHAT).
Text Section(Box& column, const std::string& text, float padTop) {
    Text t = NewText(text, 15, style::kMenuAccentInk);
    if (t) AddToVerticalBox(column, t, padTop);
    return t;
}

Card MakeCard(const Widget& body, Margin pad) { return g.maker.MakeCard(body, pad); }

Flat MakeFlat(const std::string& text, float size, Margin pad = {16, 7, 16, 7}) {
    Flat f = g.maker.MakeFlat(text, size, pad);
    if (f.label) f.label.Color(style::kMenuText);
    return f;
}

void AddFlat(Box& column, Flat& f, float padTop) { if (f.card) AddToVerticalBox(column, f.card.button, padTop).Align(HAlign::Left, VAlign::Fill); }
void AddFlatH(Box& row, Flat& f, float padRight) { if (f.card) AddToHorizontalBox(row, f.card.button, 0, padRight).Align(HAlign::Left, VAlign::Center); }

// A card across its column: a big name and a line under it (HOST FOR FRIENDS, HOST PUBLIC).
HostView MakeHost(const std::string& name, const std::string& blurb) {
    HostView v;
    Box body = g.kit.MakeColumn();
    v.name = NewText(name, 22, style::kMenuText);
    v.blurb = NewText(blurb, 14, style::kMenuDim);
    if (!body || !v.name || !v.blurb) return HostView{};
    v.blurb.Wrap(true);
    AddToVerticalBox(body, v.name, 0);
    AddToVerticalBox(body, v.blurb, 4);
    v.card = MakeCard(body, {18, 14, 18, 16});
    return v;
}

// Live or not: a dead card is dimmed and takes no clicks.
void Live(Card& c, bool on, float dim = 0.4f) {
    if (!c) return;
    c.live = on;
    c.button.Enabled(on);
    c.button.Opacity(on ? 1.f : dim);
}

// The menu's structure around the anchor, to the log once per run: the next log shows how the stock entries are built
// (user widgets, buttons, labels, brushes), which is what the Multiplayer entry copies.
void LogMenuTree(UObject* w, UObject* anchor, int depth, int& budget) {
    if (budget <= 0) return;
    --budget;
    HSM_LOG("ui: menu tree %*s%s%s", depth * 2, "", Describe(w).c_str(), w == anchor ? "   <-- anchor" : "");
    if (depth >= 4) return;
    for (UObject* child : ChildrenOf(w)) LogMenuTree(child, anchor, depth + 1, budget);
}

void LogLookOnce(const std::string& source) {
    static bool logged = false;
    if (logged) return;
    logged = true;
    HSM_LOG("ui: Multiplayer button look: %s", source.c_str());
}

// The Multiplayer entry made to look like the menu's own buttons, from what the anchor (a stock button) turns out to be:
// - its label is a real TextBlock (UI_MainMenuSNF's buttons, or a ribbon with text over it): the same button style, and
//   the label takes that TextBlock's font and colour;
// - it is art with the label baked in (this build's UI_Startup_Menu ribbons; copying the style would show the stock
//   label): the page's own card (dark face, thin frame, lit under the mouse), with the menu's own typeface when any
//   TextBlock in the menu has one. Never the plain white default button.
TextButton MakeOpenButton(const std::string& text, UObject* anchor, bool startup) {
    TextButton b;
    b.label = g.kit.MakeText();
    if (!b.label) { HSM_LOG("ui: widget spawn failed"); return TextButton{}; }
    UObject* stockLabel = startup ? FindTextBlock(anchor, 4) : nullptr;
    std::string source;
    if (!startup || stockLabel) {
        b.plain = g.kit.MakeButton();
        if (!b.plain) { HSM_LOG("ui: widget spawn failed"); return TextButton{}; }
        b.plain.StyleFrom(anchor);
        source = "the anchor's button style";
        if (stockLabel && b.label.StyleFrom(stockLabel)) source += ", font and colour of its label " + ObjectName(stockLabel);
        else source += ", the kit's font";
        // The anchor's ribbon (Quit) is sized for its four letters at that font; "MULTIPLAYER" or "LOBBY (HOST) 6/6"
        // ran past its ends. The label shrinks to fit instead (a ScaleBox), when the kit can make one.
        Shrink fit = g.kit.MakeShrink();
        if (fit) {
            fit.Content(b.label).Align(HAlign::Center, VAlign::Center);
            b.inner = b.plain.Content(fit);
        } else {
            b.inner = b.plain.Content(b.label);
        }
        b.inner.Align(HAlign::Center, VAlign::Center);
    } else {
        b.card = MakeCard(b.label, {12, 4, 12, 4});
        if (!b.card) return TextButton{};
        b.card.face.ContentAlign(HAlign::Center, VAlign::Center);
        UObject* menuText = FindTextBlock(g.menu, 12);
        source = "the page's card (the anchor's label is baked into its art)";
        if (menuText && b.label.StyleFrom(menuText)) source += ", font of the menu's TextBlock " + ObjectName(menuText);
        else source += ", the kit's font";
        b.label.Color(style::kMenuText);
    }
    LogLookOnce(source);
    b.label.Set(text);
    return b;
}

// Insert the Multiplayer button right after `anchor`, in whatever container the menu uses.
bool InjectOpenButton(UObject* anchor, bool startup) {
    UObject* freeMode = anchor;
    Call gp(freeMode, "GetParent"); gp.invoke();
    UObject* parent = gp.get<UObject*>("ReturnValue");
    if (!parent) { HSM_LOG("ui: menu anchor button has no parent panel"); return false; }
    {
        static bool treeLogged = false;
        int budget = 80;
        if (!treeLogged) { treeLogged = true; LogMenuTree(parent, anchor, 0, budget); }
    }
    g.openButton = MakeOpenButton("MULTIPLAYER", anchor, startup);
    if (!g.openButton) return false;
    std::string pcls = ObjectName(reinterpret_cast<UObject*>(ClassOf(parent)));
    if (pcls == "CanvasPanel") {
        Call add(parent, "AddChildToCanvas"); add.set<UObject*>("Content", g.openButton.button().get()).invoke();
        Slot slot(add.get<UObject*>("ReturnValue"));
        UObject** freeSlotPtr = Field<UObject*>(freeMode, "Slot");
        if (slot && freeSlotPtr && *freeSlotPtr) {
            UObject* fs = *freeSlotPtr;
            Call pos(fs, "GetPosition"); pos.invoke(); Vec2 p = pos.get<Vec2>("ReturnValue");
            Call size(fs, "GetSize"); size.invoke(); Vec2 s = size.get<Vec2>("ReturnValue");
            Call anc(fs, "GetAnchors"); anc.invoke(); Anchors a = anc.get<Anchors>("ReturnValue");
            Call al(fs, "GetAlignment"); al.invoke(); Vec2 alignment = al.get<Vec2>("ReturnValue");
            slot.Anchor(a, alignment).Size(s).Position(Vec2{p.x, p.y + s.y * 1.15});
            // Our own card has no ribbon art to set the type size: the label is a share of the entry's height.
            if (g.openButton.card && s.y > 0) g.openButton.label.Font(float(std::clamp(s.y * 0.45, 14.0, 40.0)));
            // The ribbon's curled ends and its shadow take the edges: the label keeps to the flat middle.
            if (g.openButton.inner && s.x > 0 && s.y > 0)
                g.openButton.inner.Padding({float(s.x * 0.18), float(s.y * 0.18), float(s.x * 0.18), float(s.y * 0.22)});
        }
    } else {
        // UPanelWidget::InsertChildAt is C++-only in this engine build (tools/check_reflection.py); AddChild is the
        // reflected way in, so the button goes to the end of the box rather than reshuffling the stock layout.
        Call add(parent, "AddChild"); add.set<UObject*>("Content", g.openButton.button().get()).invoke();
        if (!add.get<UObject*>("ReturnValue")) { HSM_LOG("ui: %s refused the Multiplayer button", pcls.c_str()); return false; }
    }
    HSM_LOG("ui: Multiplayer button added to %s", pcls.c_str());
    return true;
}

// The lobby chat under the players: the last lines in a framed box of fixed height (it scrolls when long lines wrap
// past it), then the text box with SEND. Enter sends too (HandleClicks).
void BuildChat(Box& left) {
    g.chatBox = g.kit.MakeColumn();
    Fixed frame = g.kit.MakeFixed();
    Panel border = g.kit.MakePanel(), panel = g.kit.MakePanel();
    g.chatScroll = g.kit.MakeScroll();
    Box lines = g.kit.MakeColumn(), input = g.kit.MakeRow();
    g.chatText = g.kit.MakeEditBox();
    g.chatSend = MakeFlat("SEND", 15, {14, 5, 14, 5});
    if (!g.chatBox || !frame || !border || !panel || !g.chatScroll || !lines || !input || !g.chatText || !g.chatSend.card) {
        HSM_LOG("ui: lobby chat widgets could not be made; chat disabled");
        g.chatBox = Box{};
        return;
    }
    Section(g.chatBox, "CHAT", 0);
    frame.Height(kChatHeight);
    border.Color(style::kMenuFrame);
    border.Padding({2, 2, 2, 2});
    border.Content(panel);
    panel.Color(style::kMenuBg);
    panel.Padding({10, 6, 10, 6});
    panel.Content(g.chatScroll);
    frame.Content(border).Align(HAlign::Fill, VAlign::Fill);
    AddToVerticalBox(g.chatBox, frame, 6);
    g.chatScroll.Add(lines);
    for (ChatRow& r : g.chatRows) {
        r.row = g.kit.MakeRow();
        r.name = NewText("", 15, style::kMenuDim);
        r.text = NewText("", 15, style::kMenuText);
        if (!r.row || !r.name || !r.text) continue;
        r.text.Wrap(true);
        AddToHorizontalBox(r.row, r.name, 0, 8).Align(HAlign::Left, VAlign::Top);
        AddToHorizontalBox(r.row, r.text, 1, 0).Align(HAlign::Fill, VAlign::Top);
        AddToVerticalBox(lines, r.row, 2);
        r.row.Show(false);
    }
    g.chatText.Hint("Say something (or /bug what went wrong)");
    AddToHorizontalBox(input, g.chatText, 1, 8).Align(HAlign::Fill, VAlign::Center);
    AddToHorizontalBox(input, g.chatSend.card.button, 0, 0).Align(HAlign::Left, VAlign::Center);
    AddToVerticalBox(g.chatBox, input, 6);
    AddToVerticalBox(left, g.chatBox, 18);
    g.chatBox.Show(false);
}

// Not in a lobby, left: the two ways to host, big, and what comes next.
void BuildIdleLeft() {
    Box& col = g.idleLeft;
    Section(col, "HOST A LOBBY", 0);
    g.hostFriends = MakeHost("HOST FOR FRIENDS", "Friends join through Steam or your invite. It is not listed for anyone else.");
    g.hostPublic = MakeHost("HOST PUBLIC", "Listed in the public lobbies for anyone with the mod.");
    for (HostView* h : {&g.hostFriends, &g.hostPublic})
        if (h->card) AddToVerticalBox(col, h->card.button, 10).Align(HAlign::Fill, VAlign::Fill);
    g.idleHint = NewText("Host a lobby or join one to choose the mode and the map.", 15, style::kMenuDim);
    if (g.idleHint) { g.idleHint.Wrap(true); AddToVerticalBox(col, g.idleHint, 18); }
}

// Not in a lobby, right: PUBLIC LOBBIES with REFRESH, the filter chips, then the rows (incompatible ones dimmed with
// the reason).
void BuildIdleRight() {
    Box& col = g.idleRight;
    Box head = g.kit.MakeRow(), chips = g.kit.MakeRow(), list = g.kit.MakeScroll();
    if (!head || !chips || !list) { HSM_LOG("ui: the lobby browser could not be made"); return; }
    AddToVerticalBox(col, head, 0);
    Text title = NewText("PUBLIC LOBBIES", 15, style::kMenuAccentInk);
    if (title) AddToHorizontalBox(head, title, 0, 16).Align(HAlign::Left, VAlign::Center);
    g.browserCount = NewText("", 14, style::kMenuDim);
    if (g.browserCount) AddToHorizontalBox(head, g.browserCount, 1, 12).Align(HAlign::Left, VAlign::Center);
    g.refresh = MakeFlat("REFRESH", 15, {14, 5, 14, 5});
    AddFlatH(head, g.refresh, 0);
    AddToVerticalBox(col, chips, 12);
    for (Flat& c : g.chips) {
        c = MakeFlat("", 14, {12, 4, 12, 4});
        AddFlatH(chips, c, 8);
    }
    FillVerticalBox(col, list, 12);
    for (size_t i = 0; i < kBrowserRows; ++i) {
        BrowserView v;
        Box row = g.kit.MakeRow(), text = g.kit.MakeColumn();
        v.name = NewText("", 18, style::kMenuText); v.detail = NewText("", 13, style::kMenuDim);
        v.players = NewText("", 15, style::kMenuDim); v.note = NewText("", 15, style::kMenuOk);
        if (row && text && v.name && v.detail && v.players && v.note) {
            AddToVerticalBox(text, v.name, 0); AddToVerticalBox(text, v.detail, 2);
            AddToHorizontalBox(row, text, 1, 14);
            AddToHorizontalBox(row, v.players, 0, 18).Align(HAlign::Left, VAlign::Center);
            AddToHorizontalBox(row, v.note, 0, 0).Align(HAlign::Left, VAlign::Center);
            v.card = MakeCard(row, {14, 8, 16, 8});
        }
        if (v.card) { list.Add(v.card.button).Padding({0, 0, 0, 6}); v.card.button.Show(false); }
        g.rows.push_back(v);                    // kept even if empty: g.rows[i] is the scene's browser[i]
    }
    g.rowLobbies.assign(g.rows.size(), 0);
    g.browserEmpty = NewText("", 15, style::kMenuDim);
    if (g.browserEmpty) { g.browserEmpty.Wrap(true); list.Add(g.browserEmpty).Padding({0, 6, 0, 0}); }
}

// In a lobby, left: the players (the host clicks one to move them to the next team), the lobby's buttons, the chat.
void BuildInLeft() {
    Box& col = g.inLeft;
    g.playersTitle = Section(col, "PLAYERS", 0);
    for (int i = 0; i < game::kMaxLobbyMembers; ++i) {
        MemberView v;
        Box row = g.kit.MakeRow();
        v.swatch = g.kit.MakeIcon(); v.avatar = g.kit.MakeIcon(); v.ready = g.kit.MakeIcon();
        v.name = NewText("", 17, style::kMenuText); v.badge = NewText("HOST", 13, style::kMenuAccentInk); v.team = NewText("", 14, style::kMenuDim);
        if (row && v.swatch && v.avatar && v.ready && v.name && v.badge && v.team) {
            v.swatch.Size({5, 32}); v.avatar.Size({32, 32}); v.ready.Size({10, 10}); v.ready.Color(style::kMenuOk);
            v.avatar.Visibility(Vis::Hidden);
            AddToHorizontalBox(row, v.swatch, 0, 10).Align(HAlign::Left, VAlign::Center);
            AddToHorizontalBox(row, v.avatar, 0, 12).Align(HAlign::Left, VAlign::Center);
            AddToHorizontalBox(row, v.name, 1, 10).Align(HAlign::Fill, VAlign::Center);
            AddToHorizontalBox(row, v.badge, 0, 12).Align(HAlign::Left, VAlign::Center);
            AddToHorizontalBox(row, v.team, 0, 12).Align(HAlign::Left, VAlign::Center);
            AddToHorizontalBox(row, v.ready, 0, 0).Align(HAlign::Left, VAlign::Center);
            v.card = MakeCard(row, {8, 5, 12, 5});
        }
        if (v.card) { AddToVerticalBox(col, v.card.button, 6).Align(HAlign::Fill, VAlign::Fill); v.card.button.Show(false); }
        g.members.push_back(v);                 // kept even if empty: g.members[i] is the scene's roster[i]
    }
    g.memberIds.assign(g.members.size(), 0);
    Box actions1 = g.kit.MakeRow(), actions2 = g.kit.MakeRow();
    g.invite = MakeFlat("INVITE FRIENDS", 15, {14, 6, 14, 6});
    g.readyUp = MakeFlat("Mark me ready", 15, {14, 6, 14, 6});
    g.balance = MakeFlat("AUTO-BALANCE", 15, {14, 6, 14, 6});
    g.leave = MakeFlat("LEAVE LOBBY", 15, {14, 6, 14, 6});
    if (actions1 && actions2) {
        AddToVerticalBox(col, actions1, 14);
        AddToVerticalBox(col, actions2, 8);
        AddFlatH(actions1, g.invite, 8); AddFlatH(actions1, g.readyUp, 0);
        AddFlatH(actions2, g.balance, 8); AddFlatH(actions2, g.leave, 0);
    }
    BuildChat(col);
}

// The one-time question (0.8.0-pre7): the first MULTIPLAYER click shows it over the menu before the page opens. YES turns
// help_improve on, NO THANKS (or Esc) leaves it off; either answer is saved and it is never asked again. Off by default.
void BuildAsk(Canvas& root) {
    g.ask = g.kit.MakePanel();
    Fixed width = g.kit.MakeFixed();
    Panel frame = g.kit.MakePanel(), face = g.kit.MakePanel();
    Box column = g.kit.MakeColumn(), buttons = g.kit.MakeRow();
    Text title = NewText("HELP IMPROVE THE MOD?", 30, style::kMenuText);
    Text body = NewText("When someone in your lobby files a bug report, or your game crashes, your HSMMod log is sent to the "
                        "maintainers so the bug can be found and fixed.", 17, style::kMenuText);
    Text privacy = NewText("Steam IDs, IP addresses and your Windows user name are removed first. Player names stay. "
                           "Nothing is sent unless you say yes. You can change this any time in SETTINGS on the "
                           "multiplayer page or in the Esc menu during a match.", 14, style::kMenuDim);
    g.askYes = MakeFlat("YES, SEND MY LOG", 18);
    g.askNo = MakeFlat("NO THANKS", 18);
    if (!g.ask || !width || !frame || !face || !column || !buttons || !title || !body || !privacy || !g.askYes.card || !g.askNo.card) {
        HSM_LOG("ui: the help-improve question could not be made; the page opens without it");
        g.ask = Panel{};
        return;
    }
    root.Add(g.ask).Anchor({{0, 0}, {1, 1}}, {0, 0}).Offsets({0, 0, 0, 0}).Z(110);
    g.ask.Color({0, 0, 0, 0.80f});
    g.ask.ContentAlign(HAlign::Center, VAlign::Center);
    g.ask.Content(width);
    width.Width(760);
    width.Content(frame);
    frame.Color(style::kMenuAccent);
    frame.Padding({2, 2, 2, 2});
    frame.Content(face);
    face.Color(style::kMenuPanel);
    face.Padding({36, 28, 36, 28});
    face.Content(column);
    title.Font(30, 1, {0, 0, 0, 0.85f});
    AddToVerticalBox(column, title, 0);
    body.Wrap(true); AddToVerticalBox(column, body, 14);
    privacy.Wrap(true); AddToVerticalBox(column, privacy, 10);
    AddToVerticalBox(column, buttons, 22);
    AddToHorizontalBox(buttons, g.askYes.card.button, 0, 14);
    AddToHorizontalBox(buttons, g.askNo.card.button, 0, 0);
    g.ask.Show(false);
}

// MODE tab: the cards two to a row, in their groups (VERSUS, CO-OP).
void BuildModeTab(Box& page) {
    Grid versus = g.kit.MakeGrid(), coop = g.kit.MakeGrid();
    if (!versus || !coop) return;
    Section(page, "VERSUS", 0);
    versus.SlotPadding({4, 4, 4, 4});
    AddToVerticalBox(page, versus, 6);
    Section(page, "CO-OP", 16);
    coop.SlotPadding({4, 4, 4, 4});
    AddToVerticalBox(page, coop, 6);
    int32_t at[2] = {0, 0};
    for (size_t i = 0; i < kModeCardCount; ++i) {
        ModeView v;
        Box body = g.kit.MakeColumn();
        v.name = NewText("", 19, style::kMenuText); v.blurb = NewText("", 13, style::kMenuDim);
        if (body && v.name && v.blurb) {
            v.blurb.Wrap(true);
            AddToVerticalBox(body, v.name, 0); AddToVerticalBox(body, v.blurb, 4);
            v.card = MakeCard(body, {14, 10, 14, 12});
        }
        const int grp = ModeGroupAt(i) == ModeGroup::Coop ? 1 : 0;
        Grid& grid = grp ? coop : versus;
        if (v.card) grid.Add(v.card.button, at[grp] / 2, at[grp] % 2).Align(HAlign::Fill, VAlign::Fill);
        ++at[grp];
        g.modes.push_back(v);                   // kept even if empty: g.modes[i] is the scene's modes[i]
    }
}

// RULES tab: a stepper per setting, "label and hint | < | value | >", in its section (MATCH, TEAMS, GEAR, WORLD).
void BuildRulesTab(Box& page) {
    for (size_t sct = 0; sct < kSectionCount; ++sct) {
        g.sectionTitles[sct] = Section(page, SectionName(LobbySection(sct)), sct ? 18 : 0);
        g.sections[sct] = g.kit.MakeColumn();
        if (g.sections[sct]) AddToVerticalBox(page, g.sections[sct], 2);
    }
    for (int i = 0; i <= int(LobbyRow::Sync); ++i) {
        StepperView v;
        Box& home = g.sections[size_t(SectionOf(LobbyRow(i)))];
        v.box = g.kit.MakeRow();
        Box text = g.kit.MakeColumn();
        Fixed valueBox = g.kit.MakeFixed();
        v.label = NewText("", 17, style::kMenuText); v.value = NewText("", 17, style::kMenuText); v.hint = NewText("", 13, style::kMenuDim);
        v.prev = MakeFlat("<", 17, {12, 2, 12, 2}); v.next = MakeFlat(">", 17, {12, 2, 12, 2});
        if (!home || !v.box || !text || !valueBox || !v.label || !v.value || !v.hint || !v.prev.card || !v.next.card) { g.steppers.push_back(StepperView{}); continue; }
        v.value.Align(Justify::Center);
        v.hint.Wrap(true);
        valueBox.Width(230);
        valueBox.Content(v.value).Align(HAlign::Fill, VAlign::Center);
        AddToVerticalBox(text, v.label, 0);
        AddToVerticalBox(text, v.hint, 2);
        AddToHorizontalBox(v.box, text, 1, 16).Align(HAlign::Fill, VAlign::Center);
        AddToHorizontalBox(v.box, v.prev.card.button, 0, 6).Align(HAlign::Left, VAlign::Center);
        AddToHorizontalBox(v.box, valueBox, 0, 6).Align(HAlign::Fill, VAlign::Center);
        AddToHorizontalBox(v.box, v.next.card.button, 0, 0).Align(HAlign::Left, VAlign::Center);
        AddToVerticalBox(home, v.box, 10);
        g.steppers.push_back(v);
    }
}

// MAP tab: the map cards two to a row (the refused ones dimmed with the reason), the modded maps after them.
void BuildMapTab(Box& page) {
    Grid grid = g.kit.MakeGrid();
    if (!grid) return;
    grid.SlotPadding({4, 4, 4, 4});
    AddToVerticalBox(page, grid, 0);
    int32_t at = 0;
    auto add = [&](const std::string& name) {
        MapView v;
        Box body = g.kit.MakeColumn();
        v.name = NewText(name, 18, style::kMenuText); v.tag = NewText("", 13, style::kMenuDim);
        if (body && v.name && v.tag) {
            AddToVerticalBox(body, v.name, 0); AddToVerticalBox(body, v.tag, 2);
            v.card = MakeCard(body, {12, 8, 12, 9});
        }
        if (v.card) grid.Add(v.card.button, at / 2, at % 2).Align(HAlign::Fill, VAlign::Fill);
        ++at;
        g.maps.push_back(v);                    // kept even if empty: g.maps[j] is s.maps[j]
    };
    for (size_t i = 0; i < MapCount(); ++i) if (MapAt(i).pick) add(MapAt(i).display);
    for (size_t i = 0; i < kMaxModMaps; ++i) add("");   // hidden until a modded map fills it
    g.noMaps = NewText("Progression plays in the Tavern: there is no map to pick.", 15, style::kMenuDim);
    if (g.noMaps) { g.noMaps.Wrap(true); AddToVerticalBox(page, g.noMaps, 6); g.noMaps.Show(false); }
}

// In a lobby, right: the MODE | RULES | MAP tabs, the chosen tab's page, and the summary with START at the foot.
void BuildInRight() {
    Box& col = g.inRight;
    Box tabs = g.kit.MakeRow(), foot = g.kit.MakeRow(), footText = g.kit.MakeColumn();
    Fixed under = g.kit.MakeFixed();
    Panel underLine = g.kit.MakePanel();
    if (!tabs || !foot || !footText || !under || !underLine) { HSM_LOG("ui: the lobby tabs could not be made"); return; }
    AddToVerticalBox(col, tabs, 0);
    static constexpr const char* kTabNames[kTabCount] = {"MODE", "RULES", "MAP"};
    for (size_t i = 0; i < kTabCount; ++i) {
        TabView& t = g.tabs[i];
        Box body = g.kit.MakeColumn();
        Fixed lineBox = g.kit.MakeFixed();
        t.label = NewText(kTabNames[i], 18, style::kMenuDim);
        t.line = g.kit.MakePanel();
        if (!body || !lineBox || !t.label || !t.line) continue;
        AddToVerticalBox(body, t.label, 0);
        lineBox.Height(3);
        lineBox.Content(t.line).Align(HAlign::Fill, VAlign::Fill);
        AddToVerticalBox(body, lineBox, 6).Align(HAlign::Fill, VAlign::Fill);
        t.card = MakeCard(body, {14, 6, 14, 0});
        if (t.card) AddToHorizontalBox(tabs, t.card.button, 0, 6);
    }
    under.Height(1);
    under.Content(underLine).Align(HAlign::Fill, VAlign::Fill);
    underLine.Color(style::kMenuFrame);
    AddToVerticalBox(col, under, 0);
    for (size_t i = 0; i < kTabCount; ++i) {
        Box scroll = g.kit.MakeScroll();
        g.tabPages[i] = g.kit.MakeColumn();
        if (!scroll || !g.tabPages[i]) continue;
        scroll.Add(g.tabPages[i]).Padding({0, 0, 12, 0});
        FillVerticalBox(col, scroll, 14);
        if (i == 0) BuildModeTab(g.tabPages[i]);
        if (i == 1) BuildRulesTab(g.tabPages[i]);
        if (i == 2) BuildMapTab(g.tabPages[i]);
        g.tabPages[i] = scroll;                 // what the tab shows and collapses is the page's scroll box
    }
    AddToVerticalBox(col, foot, 14);
    AddToHorizontalBox(foot, footText, 1, 18).Align(HAlign::Fill, VAlign::Center);
    g.summary = NewText("", 15, style::kMenuText);
    g.hostOnly = NewText("", 14, style::kMenuDim);
    g.waiting = NewText("", 15, style::kMenuDim);
    for (Text* t : {&g.summary, &g.hostOnly, &g.waiting}) if (*t) { t->Wrap(true); AddToVerticalBox(footText, *t, 2); }
    g.startLabel = NewText("START MATCH", 24, style::kMenuText);
    g.start = MakeCard(g.startLabel, {38, 12, 38, 12});
    if (g.start) {
        g.startLabel.Font(24, 1, {0, 0, 0, 0.85f});
        g.startLabel.Align(Justify::Center);
        AddToHorizontalBox(foot, g.start.button, 0, 0).Align(HAlign::Right, VAlign::Center);
    }
}

// SETTINGS (the footer button): help improve the mod and voice chat, in place of the lobby while it is open.
void BuildSettingsView(Box& col) {
    Section(col, "HELP IMPROVE THE MOD", 0);
    // The setting behind help_improve in HSMMod.ini; what it sends is session.cpp's business.
    g.helpToggle = MakeFlat("HELP IMPROVE THE MOD: OFF", 17);
    AddFlat(col, g.helpToggle, 8);
    g.helpNote = NewText("When on, your HSMMod log goes to the maintainers when someone in your lobby files a bug report or your game "
                         "crashes. Steam IDs, IPs and your Windows user name are removed; player names stay. Off unless you turn it on.",
                         14, style::kMenuDim);
    if (g.helpNote) { g.helpNote.Wrap(true); AddToVerticalBox(col, g.helpNote, 8); }
    // VOICE CHAT: the same block as the in-match Esc menu (ui/mp_widgets.h); shown with or without a lobby.
    g.voice.Build(g.maker, col, 28);
}

// A column's dark panel around a box (a scroll box when `scroll`), as one share of the body row.
Box AddColumn(Box& row, float fill, float padRight, bool scroll) {
    Panel p = g.kit.MakePanel();
    Box outer = scroll ? g.kit.MakeScroll() : g.kit.MakeColumn();
    Box inner = g.kit.MakeColumn();
    if (!p || !outer || !inner) return Box{};
    p.Color(style::kMenuPanel);
    p.Padding({24, 20, 24, 20});
    p.Content(outer);
    AddToHorizontalBox(row, p, fill, padRight).Align(HAlign::Fill, VAlign::Fill);
    if (scroll) { outer.Add(inner).Padding({0, 0, 12, 0}); return inner; }
    return outer;
}

// The multiplayer page: a Border over the whole menu (opaque, so the stock buttons underneath can neither be seen nor
// clicked) with the header, the body (two panels, or SETTINGS) and the footer (BACK, SETTINGS, REPORT A BUG).
bool BuildPanel() {
    Canvas root = g.kit.Root();
    if (!root) { HSM_LOG("ui: menu root is not a CanvasPanel; panel disabled"); return false; }
    g.panel = g.kit.MakePanel();
    Box page = g.kit.MakeColumn();
    Box header = g.kit.MakeRow();
    Box body = g.kit.MakeColumn();
    g.mainRow = g.kit.MakeRow();
    Box settingsRow = g.kit.MakeRow();
    Box footer = g.kit.MakeRow();
    Fixed rule = g.kit.MakeFixed();
    Panel ruleLine = g.kit.MakePanel();
    g.pill = g.kit.MakePanel();
    g.pillFrame = g.kit.MakePanel();
    if (!g.panel || !page || !header || !body || !g.mainRow || !settingsRow || !footer || !rule || !ruleLine || !g.pill || !g.pillFrame) {
        HSM_LOG("ui: multiplayer page widgets could not be made; panel disabled");
        g.panel = Panel{};
        return false;
    }
    root.Add(g.panel).Anchor({{0, 0}, {1, 1}}, {0, 0}).Offsets({0, 0, 0, 0}).Z(100);
    g.panel.Color(style::kMenuBg);
    g.panel.Padding({64, 40, 64, 30});
    g.panel.Content(page);

    // Header: the title, our Steam name, the state pill (outlined) and the version badge; under it the accent rule and
    // the lobby's last message.
    AddToVerticalBox(page, header, 0);
    g.title = NewText("MULTIPLAYER", 40, style::kMenuText);
    g.me = NewText("", 17, style::kMenuDim);
    g.pillText = NewText("", 15, style::kMenuDim);
    g.badge = NewText("", 14, style::kMenuDim);
    if (g.title) { g.title.Font(40, 1, {0, 0, 0, 0.85f}); AddToHorizontalBox(header, g.title, 0, 28).Align(HAlign::Left, VAlign::Center); }
    if (g.me) AddToHorizontalBox(header, g.me, 1, 20).Align(HAlign::Left, VAlign::Center);
    if (g.pillText) { g.pill.Padding({14, 4, 14, 4}); g.pill.Color(style::kMenuBg); g.pill.Content(g.pillText); }
    g.pillFrame.Padding({1, 1, 1, 1});
    g.pillFrame.Content(g.pill);
    AddToHorizontalBox(header, g.pillFrame, 0, 18).Align(HAlign::Left, VAlign::Center);
    if (g.badge) AddToHorizontalBox(header, g.badge, 0, 0).Align(HAlign::Left, VAlign::Center);
    rule.Height(3);
    rule.Content(ruleLine);
    ruleLine.Color(style::kMenuAccent);
    AddToVerticalBox(page, rule, 12);
    g.status = NewText("", 15, style::kMenuDim);
    if (g.status) AddToVerticalBox(page, g.status, 8);
    g.notice = NewText("", 20, style::kMenuAccentInk);   // Session::TakeNotice: kicked, banned, refused; until the page closes
    if (g.notice) { g.notice.Wrap(true); g.notice.Show(false); AddToVerticalBox(page, g.notice, 8); }
    g.updateNote = NewText("", 15, style::kMenuOk);   // net/update_check.h: only when a newer release exists
    if (g.updateNote) { g.updateNote.Show(false); AddToVerticalBox(page, g.updateNote, 6); }

    FillVerticalBox(page, body, 16);
    FillVerticalBox(body, g.mainRow, 0);
    FillVerticalBox(body, settingsRow, 0);
    Box left = AddColumn(g.mainRow, 0.95f, 20, true);
    Box right = AddColumn(g.mainRow, 1.45f, 0, false);
    g.idleLeft = g.kit.MakeColumn(); g.inLeft = g.kit.MakeColumn();
    g.idleRight = g.kit.MakeColumn(); g.inRight = g.kit.MakeColumn();
    if (!left || !right || !g.idleLeft || !g.inLeft || !g.idleRight || !g.inRight) {
        HSM_LOG("ui: multiplayer page columns could not be made; panel disabled");
        g.panel = Panel{};
        return false;
    }
    AddToVerticalBox(left, g.idleLeft, 0);
    AddToVerticalBox(left, g.inLeft, 0);
    FillVerticalBox(right, g.idleRight, 0);
    FillVerticalBox(right, g.inRight, 0);
    BuildIdleLeft();
    BuildIdleRight();
    BuildInLeft();
    BuildInRight();
    g.settingsView = settingsRow;
    if (Box settings = AddColumn(settingsRow, 1, 0, true)) BuildSettingsView(settings);
    g.settingsView.Show(false);
    BuildAsk(root);

    // REPORT A BUG opens a text box over the footer: SEND and CANCEL; the text becomes a public GitHub issue
    // (net/report_client).
    g.reportSend = MakeFlat("SEND", 15, {14, 5, 14, 5});
    g.reportCancel = MakeFlat("CANCEL", 15, {14, 5, 14, 5});
    g.reportText = g.kit.MakeEditBox();
    g.reportBox = g.kit.MakeRow();
    g.reportNote = NewText("", 14, style::kMenuDim);
    if (g.reportBox && g.reportText) {
        g.reportText.Hint("What went wrong? One line is enough");
        AddToHorizontalBox(g.reportBox, g.reportText, 1, 8).Align(HAlign::Fill, VAlign::Center);
        AddFlatH(g.reportBox, g.reportSend, 8); AddFlatH(g.reportBox, g.reportCancel, 0);
        AddToVerticalBox(page, g.reportBox, 12);
        g.reportBox.Show(false);
    }
    if (g.reportNote) { g.reportNote.Wrap(true); AddToVerticalBox(page, g.reportNote, 6); }

    AddToVerticalBox(page, footer, 14);
    g.close = MakeFlat("BACK", 16);
    g.settingsOpen = MakeFlat("SETTINGS", 16);
    g.reportOpen = MakeFlat("REPORT A BUG", 16);
    AddFlatH(footer, g.close, 10); AddFlatH(footer, g.settingsOpen, 10); AddFlatH(footer, g.reportOpen, 18);
    g.reportHow = NewText("In a fight: press Enter and type /bug and what happened. Your HSMMod log goes with it.", 13, style::kMenuDim);
    if (g.reportHow) { g.reportHow.Wrap(true); AddToHorizontalBox(footer, g.reportHow, 1, 0).Align(HAlign::Fill, VAlign::Center); }
    g.panel.Show(false);
    return true;
}

void Reset() { g = State{}; SetLobbyTyping(false); }

// UI_Startup_Menu (this build's main menu): image ribbons, the Multiplayer entry goes by Button_3 (Quit; Button_4 is
// Free Mode, decomp UI/UI_Startup_Menu.cpp LatentResume_L44). UI_MainMenuSNF (older builds): text buttons, anchored on
// ButtonFreeMode with TextFreeMode's font. How the entry is styled is MakeOpenButton's; the page's text takes a new
// TextBlock's font.
void Build() {
    bool startup = SameNameAscii(ObjectName(reinterpret_cast<UObject*>(g.menuClass)), "UI_Startup_Menu_C");
    const char* anchorName = startup ? "Button_3" : "ButtonFreeMode";
    UObject** anchorPtr = Field<UObject*>(g.menu, anchorName);
    g.built = true;
    if (!anchorPtr || !*anchorPtr) { HSM_LOG("ui: %s not found on the main menu", anchorName); return; }
    UObject* anchor = *anchorPtr;
    g.kit = Kit(g.menu, Vis::Visible);
    g.maker = mp::Maker(&g.kit);
    if (!startup) if (UObject** tf = Field<UObject*>(g.menu, "TextFreeMode"); tf && *tf) g.kit.FontFrom(*tf, "UI_MainMenuSNF.TextFreeMode");
    if (!g.kit.HasFont()) if (Text probe = g.kit.MakeText()) g.kit.FontFrom(probe.get(), "a new TextBlock");   // sizes by font, not by scale
    if (!InjectOpenButton(anchor, startup)) return;
    BuildPanel();
}

LobbyPhase PhaseOf(game::Lobby::State s) {
    switch (s) {
        case game::Lobby::State::Idle: return LobbyPhase::Idle;
        case game::Lobby::State::Creating: return LobbyPhase::Creating;
        case game::Lobby::State::Joining: return LobbyPhase::Joining;
        case game::Lobby::State::InLobby: return LobbyPhase::InLobby;
        case game::Lobby::State::Offline: break;
    }
    return LobbyPhase::Offline;
}

LobbyLocal Local(game::Lobby& lobby) {
    LobbyLocal l;
    l.phase = PhaseOf(lobby.state());
    l.host = lobby.IsHost();
    l.publicLobby = lobby.type() == steam::LobbyType::Public;
    l.myName = steam::PersonaName(steam::MySteamId());
    l.status = lobby.status();
    l.version = game::kModVersion;
    l.myBuild = game::ModBuild();
    l.allowBuildMismatch = game::AllowBuildMismatch();
    l.damage = game::VanillaDamageRange();
    l.maxPlayers = game::kMaxLobbyMembers;
    if (lobby.InLobby()) {
        l.config = lobby.config();
        l.mapPick = lobby.MapPick();
        for (const ModMapEntry& e : game::modmaps::Maps()) {
            const auto& paks = game::modmaps::Paks();
            l.modMaps.push_back({e.path, e.display, e.pak >= 0 && size_t(e.pak) < paks.size() ? paks[size_t(e.pak)].name : std::string()});
        }
        l.modPickPending = l.host && game::modmaps::PickPending(l.mapPick);
        l.progression = lobby.Progression();
        const bool server = lobby.ServerLobby();
        for (const game::LobbyMember& m : lobby.members()) {
            if (server && m.host) continue;   // role=server: the server is not a player
            LobbyPlayer p;
            p.name = m.name; p.host = m.host; p.ready = m.ready; p.me = m.id == steam::MySteamId();
            p.team = m.team; p.hostOverride = m.hostOverride; p.build = m.build;
            l.players.push_back(p);
        }
    }
    for (const game::LobbyListing& f : lobby.listings()) {
        LobbyFound o;
        o.name = f.name; o.map = f.map; o.mode = f.mode; o.players = f.players; o.compatible = f.compatible; o.reason = f.reason;
        l.found.push_back(o);
    }
    return l;
}


// The tabs: the chosen one in the accent's ink and underlined, the others dim (brighter under the mouse).
void PaintTabs() {
    for (size_t i = 0; i < kTabCount; ++i) {
        TabView& t = g.tabs[i];
        if (!t.card) continue;
        const bool on = i == g.tab;
        const bool hover = t.card.button.Hovered();
        t.card.frame.Color(kClear);
        t.card.face.Color(kClear);
        t.label.Color(on ? style::kMenuAccentInk : hover ? style::kMenuText : style::kMenuDim);
        t.line.Color(on ? style::kMenuAccent : kClear);
        if (g.tabPages[i]) g.tabPages[i].Show(on);
    }
}

void Draw(const LobbyScene& s) {
    g.pillText.Set(s.pill.text);
    g.pillText.Color(s.pill.color);
    g.pillFrame.Color(style::Faded(s.pill.color, 0.7f));
    g.me.Set(s.me);
    g.badge.Set(s.badge);
    g.status.Set(s.status);
    g.status.Show(!s.status.empty());

    g.mainRow.Show(!g.settingsShown);
    g.settingsView.Show(g.settingsShown);
    g.settingsOpen.card.selected = g.settingsShown;
    g.idleLeft.Show(!s.in);
    g.idleRight.Show(!s.in);
    g.inLeft.Show(s.in);
    g.inRight.Show(s.in);

    for (HostView* h : {&g.hostFriends, &g.hostPublic}) Live(h->card, s.idle);
    g.idleHint.Show(!s.in);
    Live(g.refresh.card, s.idle);
    for (size_t i = 0; i < g.chips.size(); ++i) {
        Flat& c = g.chips[i];
        if (!c.card) continue;
        const bool show = i < s.filters.size();
        c.card.button.Show(show);
        if (!show) continue;
        c.label.Set(s.filters[i].label);
        c.label.Color(s.filters[i].on ? style::kMenuAccentInk : style::kMenuText);
        c.card.selected = s.filters[i].on;
    }
    g.browserCount.Set(s.browserCount);
    g.browserEmpty.Set(s.browserEmpty);
    g.browserEmpty.Show(s.idle && s.browser.empty() && !s.browserEmpty.empty());
    for (size_t i = 0; i < g.rows.size(); ++i) {
        BrowserView& v = g.rows[i];
        bool show = i < s.browser.size();
        v.card.button.Show(show);
        if (!show) continue;
        const BrowserRow& r = s.browser[i];
        v.name.Set(r.name);
        v.detail.Set(r.detail);
        v.detail.Show(!r.detail.empty());
        v.players.Set(r.players);
        v.note.Set(r.joinable ? "JOIN" : r.note.text);
        v.note.Color(r.joinable ? style::kMenuOk : style::kMenuWarn);
        Live(v.card, r.joinable, 0.55f);        // incompatible and full lobbies stay readable, dimmed, with the reason
    }

    g.playersTitle.Set(s.playersTitle.empty() ? "PLAYERS" : s.playersTitle);
    for (size_t i = 0; i < g.members.size(); ++i) {
        MemberView& v = g.members[i];
        bool show = i < s.roster.size();
        v.card.button.Show(show);
        if (!show) continue;
        const RosterRow& r = s.roster[i];
        v.name.Set(r.name);
        v.badge.Show(r.host);
        v.team.Set(r.team + (r.moved ? " (moved)" : ""));
        v.team.Show(!r.team.empty());
        v.swatch.Color(r.swatch);
        v.ready.Visibility(r.ready ? Vis::HitTestInvisible : Vis::Hidden);
        v.card.selected = r.me;                 // our own row, framed
        v.card.live = r.movable;                // the host clicks a player to move them to the next team
    }
    g.invite.card.button.Show(s.in);
    g.readyUp.card.button.Show(s.ready);
    g.readyUp.label.Set(s.readyLabel);
    g.readyUp.label.Color(s.readyOn ? style::kMenuOk : style::kMenuText);   // the dot's green
    g.readyUp.card.selected = s.readyOn;
    g.balance.card.button.Show(s.balance);
    g.leave.card.button.Show(s.in);

    for (size_t i = 0; i < g.modes.size() && i < s.modes.size(); ++i) {
        ModeView& v = g.modes[i];
        const ModeCard& m = s.modes[i];
        v.name.Set(m.name);
        v.name.Color(m.selected ? style::kMenuAccentInk : style::kMenuText);
        v.blurb.Set(m.description);
        v.card.selected = m.selected;
        v.card.live = m.enabled;
        v.card.button.Enabled(m.enabled);
    }
    std::array<bool, kSectionCount> used{};
    for (size_t i = 0; i < g.steppers.size(); ++i) {
        StepperView& v = g.steppers[i];
        if (!v.box) continue;
        const SettingRow* row = nullptr;
        for (const SettingRow& r : s.settings) if (size_t(r.id) == i) row = &r;
        v.box.Show(row != nullptr);
        if (!row) continue;
        used[size_t(SectionOf(row->id))] = true;
        v.label.Set(row->label);
        v.label.Color(row->enabled ? style::kMenuText : style::kMenuDim);
        v.value.Set(row->value);
        v.hint.Set(row->hint);
        v.hint.Show(!row->hint.empty());
        for (Flat* f : {&v.prev, &v.next}) Live(f->card, row->enabled, 0.3f);
    }
    for (size_t sct = 0; sct < kSectionCount; ++sct) {   // a section the mode has no rows in is not shown
        if (g.sectionTitles[sct]) g.sectionTitles[sct].Show(used[sct]);
        if (g.sections[sct]) g.sections[sct].Show(used[sct]);
    }
    for (size_t i = 0; i < g.maps.size(); ++i) {
        MapView& v = g.maps[i];
        if (!v.card) continue;
        v.card.button.Show(i < s.maps.size());          // none while Progression is the choice
        if (i >= s.maps.size()) continue;
        const MapCard& m = s.maps[i];
        if (m.modded) v.name.Set(m.name);
        v.name.Color(m.selected ? style::kMenuAccentInk : style::kMenuText);
        v.tag.Set(m.allowed ? m.tag : m.reason);
        v.tag.Color(m.allowed ? style::kMenuDim : style::kMenuWarn);
        v.card.selected = m.selected;
        v.card.live = m.enabled;
        v.card.button.Enabled(m.enabled);
        v.card.button.Opacity(m.allowed ? 1.f : 0.45f);
    }
    g.noMaps.Show(s.in && s.maps.empty());
    g.summary.Set(s.summary);
    g.summary.Show(!s.summary.empty());
    g.hostOnly.Set(s.hostOnly);
    g.hostOnly.Show(!s.hostOnly.empty());
    g.start.button.Show(s.start);
    g.waiting.Set(s.waiting);
    g.waiting.Color(s.buildMismatch ? style::kMenuWarn : style::kMenuDim);
    g.waiting.Show(!s.waiting.empty());
    g.scene = s;
}

// The engine reads image files, not pixels: Steam's RGBA goes into a BMP in memory for ImportBufferAsTexture2D.
UObject* ImportAvatar(int image) {
    uint32_t w = 0, h = 0;
    std::vector<uint8_t> rgba;
    if (!steam::AvatarRGBA(image, w, h, rgba)) return nullptr;
    std::vector<uint8_t> file = RgbaToBmp(rgba.data(), rgba.size(), w, h);
    UObject* cls = FindObject("KismetRenderingLibrary", "Class");
    UObject* lib = cls ? DefaultObject(reinterpret_cast<UClass*>(cls)) : nullptr;
    if (file.empty() || !lib) return nullptr;
    FByteArray buffer{file.data(), int32_t(file.size()), int32_t(file.size())};   // read by the engine, ours
    Call c(lib, "ImportBufferAsTexture2D");
    c.set<UObject*>("WorldContextObject", g.menu).set("Buffer", buffer);
    return c.invoke() ? c.get<UObject*>("ReturnValue") : nullptr;
}

// The player's avatar texture, or null while there is none (yet): Steam answers -1 while it fetches an avatar, so a
// miss is asked again a second later, kAvatarTries times.
UObject* AvatarFor(steam::SteamId id, ULONGLONG now) {
    Avatar* a = nullptr;
    for (Avatar& e : g.avatars) if (e.id == id) a = &e;
    if (!a) { g.avatars.push_back(Avatar{id}); a = &g.avatars.back(); }
    if (a->texture && ObjectAt(a->index) == a->texture && ClassOf(a->texture) == a->cls) return a->texture;
    if (a->texture) { a->texture = nullptr; a->tries = 0; }   // collected: make it again
    if (now < a->nextTry || a->tries >= kAvatarTries) return nullptr;
    ++a->tries;
    a->nextTry = now + 1000;
    int image = steam::MediumAvatar(id);
    if (image == 0) { a->tries = kAvatarTries; return nullptr; }   // no avatar set
    if (image < 0) return nullptr;
    a->texture = ImportAvatar(image);
    if (!a->texture) {
        a->tries = kAvatarTries;
        if (!g.avatarFailLogged) { g.avatarFailLogged = true; HSM_LOG("ui: an avatar could not be made into a texture"); }
        return nullptr;
    }
    a->index = ObjectIndex(a->texture);
    a->cls = ClassOf(a->texture);
    return a->texture;
}

// The chat box (in a lobby only): the history's last lines, each sender's name in the colour the line came with.
void DrawChat(bool in) {
    if (!g.chatBox) return;
    g.chatBox.Show(in);
    if (!in) return;
    const std::vector<ChatEntry> history = ChatHistory();
    const size_t first = history.size() > g.chatRows.size() ? history.size() - g.chatRows.size() : 0;
    for (size_t i = 0; i < g.chatRows.size(); ++i) {
        ChatRow& r = g.chatRows[i];
        if (!r.row) continue;
        const bool show = first + i < history.size();
        r.row.Show(show);
        if (!show) continue;
        const ChatEntry& e = history[first + i];
        r.name.Set(e.name);
        r.name.Color(e.ink);
        r.text.Set(e.text);
    }
    if (ChatLinesAdded() != g.chatSeen) { g.chatSeen = ChatLinesAdded(); g.chatScroll.ScrollToEnd(); }
}

void UpdatePanel(Session& session) {
    game::Lobby& lobby = session.lobby();
    LobbyLocal local = Local(lobby);
    local.filter = g.filter;
    const LobbyScene scene = BuildLobbyScene(local);
    // Each shown row joins the listing it came from (the filters and the sort reorder them).
    const auto& list = lobby.listings();
    for (size_t i = 0; i < g.rowLobbies.size(); ++i) {
        const bool row = lobby.state() == game::Lobby::State::Idle && i < scene.browser.size() && scene.browser[i].source < list.size();
        g.rowLobbies[i] = row ? list[scene.browser[i].source].id : 0;
    }
    const auto& mem = lobby.members();
    for (size_t i = 0; i < g.memberIds.size(); ++i) g.memberIds[i] = lobby.InLobby() && i < mem.size() ? mem[i].id : 0;
    Draw(scene);
    DrawChat(lobby.InLobby());
    std::string notice = update::Notice();
    if (const std::string proxy = ue4ss::Notice(); !proxy.empty()) notice += (notice.empty() ? "" : "\n") + proxy;
    g.updateNote.Set(notice);
    g.updateNote.Show(!notice.empty());
    ULONGLONG now = GetTickCount64();
    for (size_t i = 0; i < g.members.size(); ++i) {
        UObject* texture = g.memberIds[i] ? AvatarFor(g.memberIds[i], now) : nullptr;
        if (texture) g.members[i].avatar.Texture(texture);
        g.members[i].avatar.Visibility(texture ? Vis::HitTestInvisible : Vis::Hidden);
    }
}

void UpdateOpenLabel(Session& session) {
    game::Lobby& lobby = session.lobby();
    std::string label = "MULTIPLAYER";
    if (lobby.InLobby()) label = std::string(lobby.IsHost() ? "LOBBY (HOST) " : "LOBBY ") + std::to_string(lobby.PlayerCount()) + "/" + std::to_string(game::kMaxLobbyMembers);
    g.openButton.label.Set(label);
}

// The lobby chat box: SEND, or Enter while the box has the keyboard (or lost it to that Enter this very frame, since the
// box commits on Enter), sends what is typed through the session (commands included). While the box has the keyboard
// the game's hotkeys stay off (ui::Typing).
void HandleChat(Session& session, bool in) {
    const bool enter = (GetAsyncKeyState(VK_RETURN) & 0x8000) != 0;
    const bool enterEdge = enter && !g.enterDown;
    g.enterDown = enter;
    if (!g.chatBox || !in) { g.chatFocused = false; SetLobbyTyping(false); return; }
    const bool focused = g.chatText.Focused();
    const bool send = Clicked(g.chatSend.card) || (enterEdge && (focused || g.chatFocused));
    g.chatFocused = focused;
    SetLobbyTyping(focused);
    if (!send) return;
    const std::string text = g.chatText.Value();
    if (text.empty()) return;
    g.chatText.Clear();
    g.chatText.Focus();                         // the next line can be typed straight away
    session.LobbyChatLine(text);
    g.nextText = 0;
}

// START: the accent face, a little toward its ink under the mouse.
void PaintStart() {
    if (!g.start) return;
    const bool hover = g.start.button.Hovered();
    g.start.glow += ((hover ? 1.f : 0.f) - g.start.glow) * 0.18f;
    g.start.frame.Color(mp::Mix(style::kMenuAccent, style::kMenuAccentInk, g.start.glow));
    g.start.face.Color(mp::Mix(style::kMenuAccent, style::kMenuAccentInk, g.start.glow * kStartGlow));
}

void ClosePage() {
    g.panelOpen = false; g.panel.Show(false); g.notice.Show(false); g.voice.StopBinding();
}

// Every frame while the page is open: each button is polled (a click is a press and a release seen in turn), the
// cards are painted for the mouse, and a click acts on what the last drawn scene offered.
void HandleClicks(Session& session) {
    game::Lobby& lobby = session.lobby();
    const bool openClicked = Clicked(g.openButton);
    Paint(g.openButton.card);
    auto openPage = [&] {
        g.panelOpen = true; g.panel.Show(true); g.nextText = 0; game::modmaps::Rescan();
        if (lobby.state() == game::Lobby::State::Idle) lobby.RefreshList();   // the browser is filled on arrival
    };
    if (g.askOpen) {                            // the one-time question: nothing else on the menu until it is answered
        const bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0, escEdge = esc && !g.escDown;
        g.escDown = esc;
        const bool yes = Clicked(g.askYes.card), no = Clicked(g.askNo.card) || escEdge;
        Paint(g.askYes.card); Paint(g.askNo.card);
        if (!yes && !no) return;
        g.askOpen = false; g.ask.Show(false);
        HSM_LOG("ui: help-improve question answered: %s", yes ? "yes" : "no");
        session.SetHelpImprove(yes);
        openPage();
        return;
    }
    if (openClicked && !g.panelOpen && !session.HelpImproveAsked() && g.ask) {
        g.askOpen = true; g.ask.Show(true);
        g.escDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
        HSM_LOG("ui: first MULTIPLAYER click: asking about help improve the mod (off until answered)");
        return;
    }
    if (openClicked) { if (g.panelOpen) ClosePage(); else openPage(); }
    if (!g.panelOpen) { SetLobbyTyping(false); return; }
    const LobbyScene& s = g.scene;
    bool acted = false;
    auto act = [&](bool clicked, bool allowed) { bool go = clicked && allowed; acted = acted || go; return go; };

    // The footer.
    if (Clicked(g.close.card)) { ClosePage(); return; }
    if (act(Clicked(g.settingsOpen.card), true)) { g.settingsShown = !g.settingsShown; if (!g.settingsShown) g.voice.StopBinding(); }
    g.reportOpen.card.button.Show(report::Enabled());
    g.reportHow.Show(report::Enabled());
    if (act(Clicked(g.reportOpen.card), report::Enabled())) {
        g.reportOpenNow = !g.reportOpenNow;
        g.reportBox.Show(g.reportOpenNow);
        g.reportNote.Set("");
        if (g.reportOpenNow) g.reportText.Focus();
    }
    g.reportOpen.card.selected = g.reportOpenNow;
    if (g.reportOpenNow && act(Clicked(g.reportCancel.card), true)) { g.reportOpenNow = false; g.reportBox.Show(false); g.reportText.Clear(); }
    if (g.reportOpenNow && act(Clicked(g.reportSend.card), true)) {
        std::string text = g.reportText.Value();
        if (text.empty()) {
            g.reportNote.Set("Type what went wrong first.");
        } else {
            g.reportNote.Set(session.SubmitReport(text));
            g.reportText.Clear();
            g.reportOpenNow = false; g.reportBox.Show(false);
        }
    }
    if (const uint32_t seq = session.ReportStatusSeq(); seq != g.reportSeen) {
        g.reportSeen = seq;
        g.reportNote.Set(s.in ? std::string() : session.ReportStatus());
    }

    // SETTINGS: HELP IMPROVE THE MOD (on: the accent frame and a green label, as Mark me ready) and voice.
    const bool help = session.HelpImprove();
    g.helpToggle.label.Set(help ? "HELP IMPROVE THE MOD: ON" : "HELP IMPROVE THE MOD: OFF");
    g.helpToggle.label.Color(help ? style::kMenuOk : style::kMenuText);
    g.helpToggle.card.selected = help;
    if (act(Clicked(g.helpToggle.card), g.settingsShown)) session.SetHelpImprove(!help);
    if (g.settingsShown) g.voice.Tick(session);
    HandleChat(session, s.in && !g.settingsShown);

    // Not in a lobby: hosting, the browser and its filters.
    if (act(Clicked(g.hostFriends.card), s.idle)) lobby.Host(steam::LobbyType::FriendsOnly);
    if (act(Clicked(g.hostPublic.card), s.idle)) lobby.Host(steam::LobbyType::Public);
    if (act(Clicked(g.refresh.card), s.idle)) lobby.RefreshList();
    for (size_t i = 0; i < g.chips.size(); ++i)
        if (act(Clicked(g.chips[i].card), s.idle && i < s.filters.size())) g.filter = ToggleFilter(g.filter, i);
    for (size_t i = 0; i < g.rows.size(); ++i)
        if (act(Clicked(g.rows[i].card), i < s.browser.size() && s.browser[i].joinable && g.rowLobbies[i])) lobby.Join(g.rowLobbies[i]);

    // In a lobby: the players' buttons, the tabs, and what each tab offers.
    if (act(Clicked(g.invite.card), s.in)) lobby.Invite();
    if (act(Clicked(g.readyUp.card), s.ready)) lobby.SetReady(!s.readyOn);
    if (act(Clicked(g.balance.card), s.balance)) lobby.ClearOverrides();
    if (act(Clicked(g.leave.card), s.in)) lobby.Leave();
    for (size_t i = 0; i < kTabCount; ++i) if (act(Clicked(g.tabs[i].card), s.in)) g.tab = i;

    MatchConfig c = lobby.config();
    int teams = HasTeams(c.format) ? TeamCount(c.format) : 0;
    for (size_t i = 0; i < g.members.size(); ++i) {
        if (!act(Clicked(g.members[i].card), i < s.roster.size() && s.roster[i].movable && g.memberIds[i] && teams)) continue;
        for (const game::LobbyMember& m : lobby.members())
            if (m.id == g.memberIds[i]) lobby.SetOverride(m.id, NextTeamPick(int8_t(m.hostOverride >= 0 ? m.hostOverride : m.team), teams));
    }
    for (size_t i = 0; i < g.modes.size(); ++i) {
        if (!act(Clicked(g.modes[i].card), g.tab == 0 && i < s.modes.size() && s.modes[i].enabled)) continue;
        if (s.modes[i].progression) { lobby.SetProgression(true); continue; }
        if (lobby.Progression()) lobby.SetProgression(false);
        MatchConfig next = SelectMode(c, s.modes[i].format, s.modes[i].brawl);
        if (next.format != c.format || next.gear != c.gear) { lobby.SetConfig(next); lobby.ClearOverrides(); c = next; }
    }
    for (size_t i = 0; i < g.steppers.size(); ++i) {
        StepperView& v = g.steppers[i];
        if (!v.box) continue;
        int dir = Clicked(v.prev.card) ? -1 : 0;
        if (Clicked(v.next.card)) dir = 1;
        const SettingRow* row = nullptr;
        for (const SettingRow& r : s.settings) if (size_t(r.id) == i) row = &r;
        if (!act(dir != 0, g.tab == 1 && row && row->enabled)) continue;
        if (row->id == LobbyRow::MyTeam) {
            int mine = kNoTeam;
            for (const game::LobbyMember& m : lobby.members()) if (m.id == steam::MySteamId()) mine = m.team;
            lobby.SetTeamPick(StepTeamPick(int8_t(mine), teams, dir));
        } else {
            c = StepSetting(c, row->id, dir, game::VanillaDamageRange());
            lobby.SetConfig(c);
        }
    }
    for (size_t i = 0; i < g.maps.size(); ++i)
        if (act(Clicked(g.maps[i].card), g.tab == 2 && i < s.maps.size() && s.maps[i].enabled))
            lobby.SetMapPick(s.maps[i].modded ? s.maps[i].path : std::string(MapAt(s.maps[i].index).path));
    if (act(Clicked(g.start), s.start)) {
        if (s.modes.size() == kModeCardCount && s.modes.back().selected) session.StartProgression();
        else {
            const MapCard* custom = nullptr;                 // a modded map is the pick: it is open by its package path
            for (const MapCard& mc : s.maps) if (mc.modded && mc.selected) custom = &mc;
            size_t m = EffectiveMap(c.format, lobby.MapPick());
            if (custom) session.StartOnMap(custom->path);
            else if (m < MapCount()) session.StartOnMap(MapAt(m).path);
        }
    }

    // Paint after the polls (a card's Hovered() is the one its Clicked() just saw).
    for (Flat* f : {&g.refresh, &g.invite, &g.readyUp, &g.balance, &g.leave, &g.close, &g.settingsOpen, &g.reportOpen,
                    &g.reportSend, &g.reportCancel, &g.chatSend, &g.helpToggle}) Paint(f->card);
    for (Flat& f : g.chips) Paint(f.card);
    for (HostView* h : {&g.hostFriends, &g.hostPublic}) Paint(h->card);
    for (BrowserView& v : g.rows) Paint(v.card);
    for (MemberView& v : g.members) Paint(v.card);
    for (ModeView& v : g.modes) Paint(v.card);
    for (StepperView& v : g.steppers) { Paint(v.prev.card); Paint(v.next.card); }
    for (MapView& v : g.maps) Paint(v.card);
    PaintTabs();
    PaintStart();
    if (acted) g.nextText = 0;                  // redraw now, not at the next quarter second
}
}  // namespace

void Tick(Session& session, UObject* worldContext) {
    if (!session.steamMode()) return;
    game::modmaps::Tick();                       // the modded-map scan (worker results, asset registry); cheap when idle
    if (worldContext != g.worldContext) { Reset(); g.worldContext = worldContext; }
    ULONGLONG now = GetTickCount64();
    const bool alive = StillAlive(g.menu, g.menuIndex, g.menuClass);
    if (!alive && g.menu) { UObject* wc = g.worldContext; Reset(); g.worldContext = wc; }
    if (!alive || !InViewport(g.menu)) {
        // The settings screens remove the menu and their Back button makes a new one (UI_GameSettings.cpp:123, 345),
        // while the removed one lives on until garbage collection: follow whichever menu is on screen (PickMenu).
        if (now < g.nextScan) return;
        g.nextScan = now + 500;
        std::vector<MenuCandidate> live;
        for (const char* name : {"UI_Startup_Menu_C", "UI_MainMenuSNF_C"})
            for (UObject* o : FindInstances(name)) live.push_back({o, InViewport(o)});
        UObject* pick = static_cast<UObject*>(const_cast<void*>(PickMenu(g.menu, false, live)));
        if (!pick) return;
        if (pick != g.menu) {
            UObject* wc = g.worldContext; Reset(); g.worldContext = wc;
            g.menu = pick; g.menuIndex = ObjectIndex(pick); g.menuClass = ClassOf(pick);
            HSM_LOG("ui: main menu %s is on screen; the Multiplayer button follows it", ObjectName(pick).c_str());
        }
    }
    if (!g.built) Build();
    if (!g.openButton) return;
    // Turned away by a host: the page opens on its own with the reason in large type, so it is not missed.
    if (std::string why = session.TakeNotice(); !why.empty() && g.notice) {
        g.notice.Set(why); g.notice.Show(true);
        g.panelOpen = true; g.panel.Show(true); g.nextText = 0;
    }
    HandleClicks(session);
    if (now >= g.nextText) { g.nextText = now + 250; UpdateOpenLabel(session); if (g.panelOpen) UpdatePanel(session); }
}

// Fight levels: no main menu belongs on screen. A client that joined with the multiplayer page open kept the page over
// the match (test 2026-10-27), so close our page and take any main menu still in the viewport off it.
bool PanelOpen() { return g.panelOpen && g.menu && StillAlive(g.menu, g.menuIndex, g.menuClass) && InViewport(g.menu); }

void LeaveMenus() {
    static ULONGLONG next = 0;
    const ULONGLONG now = GetTickCount64();
    if (now < next) return;
    next = now + 2000;
    SetLobbyTyping(false);
    if (g.panelOpen && StillAlive(g.menu, g.menuIndex, g.menuClass)) { g.panelOpen = false; g.panel.Show(false); }
    for (const char* name : {"UI_Startup_Menu_C", "UI_MainMenuSNF_C"})
        for (UObject* o : FindInstances(name))
            if (InViewport(o)) {
                Call(o, "RemoveFromParent").invoke();
                HSM_LOG("ui: %s was still on screen in a fight level; removed", ObjectName(o).c_str());
            }
}

}  // namespace hsm::ui
