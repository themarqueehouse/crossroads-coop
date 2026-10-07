#ifndef GUARD_CONFIG_COOP_H
#define GUARD_CONFIG_COOP_H

// Co-op settings.

// Money is never spent and always sufficient.
//
// A deliberate choice about the game, not a workaround. Money was the one
// shared thing that could cause an argument between two players out of one
// wallet, and grinding for it is the least interesting part of the game. Shops
// still work normally; you simply always have enough.
//
// It also happens to remove money from the shared-state problem entirely:
// nothing to sync, and no need to handle the per-console encryption key for it.
// That is a convenience, not the reason.
//
// Set to FALSE to play with ordinary money. Nothing else needs changing: the
// amount is still stored and still displayed, it is simply never decremented.
#define COOP_UNLIMITED_MONEY TRUE

#endif // GUARD_CONFIG_COOP_H
