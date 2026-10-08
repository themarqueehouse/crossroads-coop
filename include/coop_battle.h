#ifndef GUARD_COOP_BATTLE_H
#define GUARD_COOP_BATTLE_H

#include "global.h"

// Co-op battles: both players against the same trainers, at once. See
// src/coop_battle.c for the shape and why it is the Battle Tower's link multi
// configuration rather than anything that sounds more direct.

/**
 * Begin a co-op battle against two trainers.
 *
 * Must be called on BOTH consoles in the same moment -- put it behind a sync
 * gate, or one console tears its link down while the other is still walking
 * about and neither gets a battle.
 */
void Coop_StartBattle(u16 opponentA, u16 opponentB, bool8 splitTeam);

/**
 * True when the two opposing trainer slots share ONE trainer's team.
 *
 * A gym leader has no partner, and pairing them with a real gym trainer costs
 * the gym a fight. So the second slot is a filler who brings nothing of their
 * own and the leader's team is dealt across the two of them: the opposition is
 * exactly the Pokemon the leader always had, in a double battle because there
 * are two trainers standing there.
 */
bool8 Coop_BattleSplitsTeam(void);

/** Generate opponent A's whole team and deal the back half to opponent B. */
void Coop_BuildSplitOpponents(void);

/**
 * Ask for the NEXT trainerbattle to be fought by both players.
 *
 * Set just before an ordinary trainerbattle command. Marking the battle rather
 * than replacing the command is what keeps the intro text, the defeat text, the
 * trainer flag, the jump to the badge script and the whiteout all working --
 * they belong to trainerbattle, and trainerbattle still runs.
 */
void Coop_MarkNextBattle(u16 partnerTrainer, bool8 split);

/**
 * Called by BattleSetup_StartTrainerBattle. TRUE means co-op has taken the
 * battle over and the caller should not start one of its own.
 */
bool8 Coop_TakeOverTrainerBattle(void);

/** True from the moment a co-op battle starts until the field comes back. */
bool8 Coop_IsBattleActive(void);

#endif // GUARD_COOP_BATTLE_H
