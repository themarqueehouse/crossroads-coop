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
void Coop_StartBattle(u16 opponentA, u16 opponentB);

/** True from the moment a co-op battle starts until the field comes back. */
bool8 Coop_IsBattleActive(void);

#endif // GUARD_COOP_BATTLE_H
