// The game mode's Match Won and Player DED flags per format, phase and fighter state (StockFlagsFor). The bug: after a
// Survival rematch (and from the first match in the Abyss) Match Won stayed clear in the break between waves and Player DED
// stayed set from the first match's loss, so holding G to skip the break started the game's give-up.
#include "match/hsm_match_won.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++g_fail; } } while (0)
using namespace hsm;

static MatchInfo Info(Format f, MatchPhase p = MatchPhase::Live, uint32_t gapLeftMs = 0) {
    MatchInfo m; m.format = uint8_t(f); m.phase = uint8_t(p); m.roundGapLeftMs = gapLeftMs;
    return m;
}

static void TestBotFormats() {
    for (Format f : {Format::PlayersVsBots, Format::Zombies}) {
        CHECK(MatchWonFor(Info(f)) == MatchWonFlag::Clear);                                       // a wave being fought
        CHECK(MatchWonFor(Info(f, MatchPhase::SuddenDeath)) == MatchWonFlag::Clear);
        CHECK(MatchWonFor(Info(f, MatchPhase::Live, 30000)) == MatchWonFlag::Set);                // the break between waves
        CHECK(MatchWonFor(Info(f, MatchPhase::SuddenDeath, 5000)) == MatchWonFlag::Set);
    }
}

static void TestEveryOtherFormatIsClearInARound() {
    for (Format f : {Format::FreeForAll, Format::TwoTeams, Format::ThreeTeams, Format::Duel}) {
        CHECK(MatchWonFor(Info(f)) == MatchWonFlag::Clear);                                       // never set outside a bot format's break
        CHECK(MatchWonFor(Info(f, MatchPhase::SuddenDeath)) == MatchWonFlag::Clear);
    }
    CHECK(MatchWonFor(Info(Format::Duel, MatchPhase::Live, 3000)) == MatchWonFlag::Leave);        // a Duel's round gap is not a round
}

static void TestOutsideAFight() {
    for (Format f : {Format::PlayersVsBots, Format::Zombies, Format::FreeForAll, Format::Duel})
        for (MatchPhase p : {MatchPhase::Idle, MatchPhase::Countdown, MatchPhase::Results}) {
            CHECK(MatchWonFor(Info(f, p)) == MatchWonFlag::Leave);
            CHECK(MatchWonFor(Info(f, p, 30000)) == MatchWonFlag::Leave);                         // a stale gap time counts for nothing
        }
}

static void TestPlayerDedFollowsTheFighter() {
    const MatchInfo wave = Info(Format::PlayersVsBots);
    CHECK(StockFlagsFor(wave, true).clearPlayerDed);
    CHECK(!StockFlagsFor(wave, false).clearPlayerDed);                                            // dead or waiting: the game's flag stands
    CHECK(StockFlagsFor(Info(Format::Duel, MatchPhase::Live, 3000), true).clearPlayerDed);        // alive in a gap, the flag is still stale
    CHECK(StockFlagsFor(Info(Format::Zombies, MatchPhase::Live, 30000), true).matchWon == MatchWonFlag::Set);
}

int main() {
    TestBotFormats(); TestEveryOtherFormatIsClearInARound(); TestOutsideAFight(); TestPlayerDedFollowsTheFighter();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::puts("hsm_match_won ok");
    return 0;
}
