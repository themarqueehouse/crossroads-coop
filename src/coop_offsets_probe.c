// Struct offsets, published for the test harness.
//
// The harness reads the co-op save record straight out of emulated memory, so
// it needs the real offset of coopPlayer2 inside SaveBlock1. Computing that on
// the harness side means duplicating the struct layout in JavaScript, and the
// first time it drifted it cost an hour: sizeof(struct CoopPlayer2) is 632, not
// the 636 its fields add up to, because of alignment -- so a harness deriving
// the offset as "size of the block minus size of the record" read four bytes
// early and saw plausible-looking garbage.
//
// So the compiler answers instead. This array is unreferenced and
// --gc-sections drops it from the ROM, costing nothing; the harness reads it
// out of the object file. Regenerate with tools/coop/emit_offsets.py.
#include "global.h"
#include "constants/flags.h"

const unsigned gCoopOffsets[] = {
    sizeof(struct SaveBlock1),
    sizeof(struct CoopPlayer2),
    offsetof(struct SaveBlock1, coopPlayer2),
    offsetof(struct CoopPlayer2, playerName),
    offsetof(struct CoopPlayer2, playerGender),
    offsetof(struct CoopPlayer2, partyCount),
    offsetof(struct CoopPlayer2, claimed),
    offsetof(struct CoopPlayer2, pos),
    offsetof(struct CoopPlayer2, location),
    offsetof(struct CoopPlayer2, party),
};

// Flag and Pokedex offsets, for the harness's shared-progression check.
const unsigned gCoopWorldOffsets[] = {
    offsetof(struct SaveBlock1, flags),
    NUM_FLAG_BYTES,
    offsetof(struct SaveBlock1, dexSeen),
    offsetof(struct SaveBlock1, dexCaught),
    NUM_DEX_FLAG_BYTES,
    FLAG_BADGE01_GET,
    TEMP_FLAGS_SIZE,
    offsetof(struct SaveBlock1, bag),
    offsetof(struct SaveBlock2, encryptionKey),
    // Not 0, and not 1 either: SaveBlock1 opens with `struct Coords16 pos`, so
    // location sits past it. A harness that guessed wrote the map number over
    // the player's x coordinate and then reported, correctly, that moving the
    // partner to another map had no effect.
    offsetof(struct SaveBlock1, location),
    offsetof(struct WarpData, mapNum),
    offsetof(struct SaveBlock1, money),
};
