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
// It also happens to sidestep the shared-state problem: with nothing ever
// spent there is nothing to disagree about.
//
// Set to FALSE to play with ordinary money. Nothing else needs changing -- and
// that is now true rather than nearly true. Money is synced either way: it
// rides in the join snapshot and in a live delta, both decrypted, since the
// two consoles have different encryption keys and the stored form means
// nothing across the link. Before that it was simply unshared, which did not
// show while this was TRUE because the figure never moved.
#define COOP_UNLIMITED_MONEY TRUE

#endif // GUARD_CONFIG_COOP_H
