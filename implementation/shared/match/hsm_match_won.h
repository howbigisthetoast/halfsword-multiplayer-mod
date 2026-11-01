// What the game mode's "Match Won" flag should be, from the referee's match state. Game-independent and unit-tested; the
// client's gameplay layer applies the answer (session.cpp TickMatchWon).
//
// The game's own give-up (the kneel on G, the dropped weapon, the voice lines) does nothing while Match Won is set. The
// first Survival match runs the game's win logic, which sets it after the first round, so its breaks are quiet; a
// rematch never runs that logic and every G hold in a break started the give-up. The Abyss never sets it at all. In a bot
// format the referee's phase is that logic: Match Won is set in the break between waves and clear in a fought wave.
// Other formats and the phases outside a fight are left alone.
#pragma once
#include "match/hsm_hud_text.h"

namespace hsm {

enum class MatchWonFlag : uint8_t { Leave, Clear, Set };

inline MatchWonFlag MatchWonFor(const MatchInfo& m) {
    if (!IsBotFormat(Format(m.format))) return MatchWonFlag::Leave;
    const bool fought = m.phase == uint8_t(MatchPhase::Live) || m.phase == uint8_t(MatchPhase::SuddenDeath);
    if (!fought) return MatchWonFlag::Leave;
    return InWaveBreak(m) ? MatchWonFlag::Set : MatchWonFlag::Clear;
}

}  // namespace hsm
