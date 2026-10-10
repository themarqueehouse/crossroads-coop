'use strict';

// Is this actually a save, or is it an empty chip?
//
// The Save file button hands the player a copy of the emulator's save chip,
// and the chip only ever contains what the GAME has written to it. Press the
// button without having saved in-game and you get a perfectly well-formed
// 128 KiB file with nothing in it -- which looks exactly like a real save
// until the day you need it, load it, and are told there is no saved game.
//
// Emerald writes its save in 4 KiB sectors and stamps each one it writes with
// 0x08012025 near the end. Counting those is the only honest way to tell a
// saved game from a blank chip from outside the ROM.
//
// Counting the signatures rather than looking for non-0xFF bytes, which was
// the first attempt and is worthless: an untouched chip reads as all 0xFF on
// some emulators and all 0x00 on others, so a file of 131072 zero bytes was
// reported as entirely non-blank, which is true and tells you nothing.

export const SECTOR_BYTES = 4096;
export const SECTOR_SIGNATURE = 0x08012025;
export const SIGNATURE_OFFSET = 0xFF8;

// A full save is 14 sectors of game data plus a second copy. Anything that
// has been saved at all has the first 14.
export const SECTORS_PER_SAVE = 14;

/** How many sectors of `bytes` carry Emerald's save signature. */
export function countSaveSectors(bytes) {
  let sectors = 0;

  for (let off = 0; off + SECTOR_BYTES <= bytes.length; off += SECTOR_BYTES) {
    const at = off + SIGNATURE_OFFSET;
    const sig = (bytes[at]
              | (bytes[at + 1] << 8)
              | (bytes[at + 2] << 16)
              | (bytes[at + 3] << 24)) >>> 0;

    if (sig === SECTOR_SIGNATURE) sectors++;
  }

  return sectors;
}

/** True if `bytes` holds a game that has been saved at least once. */
export function looksLikeASave(bytes) {
  return countSaveSectors(bytes) >= SECTORS_PER_SAVE;
}
