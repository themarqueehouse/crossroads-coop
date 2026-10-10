#ifndef GUARD_CONSTANTS_COOP_BADGES_H
#define GUARD_CONSTANTS_COOP_BADGES_H

// Kanto's eight badges, as their own flags.
//
// They did not have any. Every Kanto gym set FLAG_BADGE01_GET through
// FLAG_BADGE08_GET -- the same eight flags Hoenn's gyms set -- so the game
// could not tell a Boulder Badge from a Stone Badge, and the Trainer Card,
// which has sixteen slots, lit the second row from the first row's flags.
// That is the "Kanto badges don't appear on the Trainer Card" report: they
// were never recorded separately to appear.
//
// These are set IN ADDITION to the originals rather than instead of them.
// Plenty of things gate on FLAG_BADGE0x_GET -- field moves above all -- and
// quietly moving Kanto off them would change what you can do with an HM in
// the middle of somebody's playthrough. This records which badges you
// actually have; it does not re-balance who can use Surf.
//
// 0xC00 upwards: clear of the hack's own flags (which stop at 0xAAD) and of
// the scattered legendaries (0xB00 and up), and well short of the end of the
// array at 3,152.
#define FLAG_KANTO_BADGE01_GET 0xC00  // Brock, Pewter
#define FLAG_KANTO_BADGE02_GET 0xC01  // Misty, Cerulean
#define FLAG_KANTO_BADGE03_GET 0xC02  // Lt. Surge, Vermilion
#define FLAG_KANTO_BADGE04_GET 0xC03  // Erika, Celadon
#define FLAG_KANTO_BADGE05_GET 0xC04  // Koga, Fuchsia
#define FLAG_KANTO_BADGE06_GET 0xC05  // Sabrina, Saffron
#define FLAG_KANTO_BADGE07_GET 0xC06  // Blaine, Cinnabar
#define FLAG_KANTO_BADGE08_GET 0xC07  // Giovanni, Viridian

#endif // GUARD_CONSTANTS_COOP_BADGES_H
