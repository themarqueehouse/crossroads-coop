// A save is paired to a game by filename and nothing else. A save named after
// anything but the ROM is not found, and the failure is silent: the game boots
// to a main menu offering NEW GAME only, which reads as a corrupt or missing
// save rather than a misnamed one. It cost a playtest.
import { test } from 'node:test';
import assert from 'node:assert';

// The rule as app.js applies it, kept here so a change to it has to be
// deliberate rather than accidental.
function uploadName(romName, saveName) {
  const isState = /\.(ss\d*|state)$/i.test(saveName);
  const wanted = romName.replace(/\.gba$/i, '') + '.sav';
  return (isState || saveName === wanted) ? saveName : wanted;
}

test('a differently named save is renamed to match the ROM', () => {
  assert.equal(uploadName('pokeemerald.gba', 'crossroads-midgame.sav'),
               'pokeemerald.sav');
});

test('a save that already matches is left alone', () => {
  assert.equal(uploadName('pokeemerald.gba', 'pokeemerald.sav'),
               'pokeemerald.sav');
});

test('the phone-duplicated name is still renamed', () => {
  assert.equal(uploadName('pokeemerald.gba', 'pokeemerald (1).sav'),
               'pokeemerald.sav');
});

test('a save state keeps its name, since those pair differently', () => {
  assert.equal(uploadName('pokeemerald.gba', 'whatever.ss1'), 'whatever.ss1');
  assert.equal(uploadName('pokeemerald.gba', 'whatever.state'), 'whatever.state');
});

test('a ROM named in capitals still produces a .sav', () => {
  assert.equal(uploadName('POKEEMERALD.GBA', 'x.sav'), 'POKEEMERALD.sav');
});
