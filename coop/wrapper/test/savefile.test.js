'use strict';

// The Save file button hands over a copy of the emulator's save chip, and the
// chip holds what the GAME last wrote to it rather than what is on screen.
// Press it without having saved in-game and the file is a well-formed 128 KiB
// of nothing -- which looks like a real save right up until the day you need
// it. These are the checks that tell the two apart.

import test from 'node:test';
import assert from 'node:assert/strict';

import {
  countSaveSectors,
  looksLikeASave,
  SECTOR_BYTES,
  SIGNATURE_OFFSET,
  SECTORS_PER_SAVE,
} from '../src/savefile.js';

const CHIP_BYTES = 128 * 1024;

function blankChip(fill) {
  return new Uint8Array(CHIP_BYTES).fill(fill);
}

/** Stamp `n` sectors with Emerald's signature, as a real save does. */
function chipWithSectors(n, fill = 0xFF) {
  const chip = blankChip(fill);
  for (let i = 0; i < n; i++) {
    const at = i * SECTOR_BYTES + SIGNATURE_OFFSET;
    chip[at] = 0x25;
    chip[at + 1] = 0x20;
    chip[at + 2] = 0x01;
    chip[at + 3] = 0x08;
  }
  return chip;
}

test('an erased chip reads as no save, whichever way it is erased', () => {
  // 0xFF on some emulators, 0x00 on others. An earlier version of this check
  // only looked for 0xFF, so a chip of 131072 zero bytes was reported as
  // entirely non-blank -- true, and worthless.
  for (const fill of [0x00, 0xFF]) {
    assert.equal(countSaveSectors(blankChip(fill)), 0, `fill 0x${fill.toString(16)}`);
    assert.equal(looksLikeASave(blankChip(fill)), false);
  }
});

test('a saved game is recognised', () => {
  const chip = chipWithSectors(SECTORS_PER_SAVE);
  assert.equal(countSaveSectors(chip), SECTORS_PER_SAVE);
  assert.equal(looksLikeASave(chip), true);
});

test('a half-written chip is not a save', () => {
  // Emerald writes fourteen sectors. Fewer means the write was interrupted,
  // and handing that over as a backup is worse than refusing.
  const chip = chipWithSectors(SECTORS_PER_SAVE - 1);
  assert.equal(looksLikeASave(chip), false);
});

test('a near-miss signature does not count', () => {
  const chip = blankChip(0xFF);
  const at = SIGNATURE_OFFSET;
  chip[at] = 0x25; chip[at + 1] = 0x20; chip[at + 2] = 0x01; chip[at + 3] = 0x09;
  assert.equal(countSaveSectors(chip), 0);
});

test('the signature is only looked for where Emerald puts it', () => {
  // The same four bytes anywhere else in the sector are data, not a stamp.
  const chip = blankChip(0x00);
  chip[0] = 0x25; chip[1] = 0x20; chip[2] = 0x01; chip[3] = 0x08;
  assert.equal(countSaveSectors(chip), 0);
});

test('a short buffer does not read past its end', () => {
  assert.equal(countSaveSectors(new Uint8Array(0)), 0);
  assert.equal(countSaveSectors(new Uint8Array(SECTOR_BYTES - 1)), 0);
});

test('the sixteen bytes of RTC on the end change nothing', () => {
  // A real .sav off this emulator is 131088 bytes: the chip plus an RTC tail
  // that is not a whole sector.
  const chip = chipWithSectors(SECTORS_PER_SAVE);
  const withRtc = new Uint8Array(chip.length + 16);
  withRtc.set(chip);
  assert.equal(countSaveSectors(withRtc), SECTORS_PER_SAVE);
  assert.equal(looksLikeASave(withRtc), true);
});
