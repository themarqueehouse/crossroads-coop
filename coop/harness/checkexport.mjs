#!/usr/bin/env node
// Does the Save file button's "you have not saved yet" check actually fire?
//
// The wrapper refuses to hand over a save chip the game has never written to,
// because that file is a well-formed 128 KiB of nothing and is
// indistinguishable from a real save until the day you load it. The unit
// tests for that check work on chips this file builds by hand, which proves
// the arithmetic and nothing about the emulator.
//
// This is the other half: a real chip out of a real core that has booted,
// reached the overworld and not saved, and the same chip after saving. The
// check has to say no to the first and yes to the second, or it is either
// useless or in the way.
//
//   node coop/harness/checkexport.mjs
import { startRig } from './rig.mjs';
import { countSaveSectors, looksLikeASave } from '../wrapper/src/savefile.js';

const PORT = 8806;
const ROM = '/home/claude/crossroads/pokeemerald.gba';
const SCRIPTS_MENU_INDEX = 5;
const SAVE_SCRIPT = 15;          // debug.inc: clears Player 2, then saves

const isBusy = async (rig, w) =>
  (await rig.mailbox(w)).flags.includes('SCRIPT_BUSY');

async function runDebugScript(rig, w, slot) {
  await rig.hold(w, 'R'); await rig.wait(6);
  await rig.tap(w, 'Start', 8); await rig.wait(20);
  await rig.letGo(w, 'R'); await rig.wait(20);
  for (let i = 0; i < SCRIPTS_MENU_INDEX; i++) { await rig.tap(w, 'Down', 6); await rig.wait(8); }
  await rig.tap(w, 'A', 8); await rig.wait(25);
  for (let i = 1; i < slot; i++) { await rig.tap(w, 'Down', 6); await rig.wait(8); }
  await rig.tap(w, 'A', 8); await rig.wait(25);
}

const chip = async (rig) => {
  const bytes = await rig.page.evaluate(async () => {
    await window.__pair.sync();
    const s = window.__pair.save();
    return s ? Array.from(s) : null;
  });
  return bytes ? new Uint8Array(bytes) : null;
};

async function main() {
  const rig = await startRig({ rom: ROM, port: PORT });
  let failures = 0;
  const check = (what, ok, detail) => {
    console.log(`${ok ? 'PASS' : 'FAIL'}: ${what}${ok || !detail ? '' : ` -- ${detail}`}`);
    if (!ok) failures++;
  };

  try {
    // --- never saved ----------------------------------------------------
    const before = await chip(rig);
    check('the core gives us a chip at all', before !== null && before.length > 0,
          'getSave returned nothing');

    if (before) {
      console.log(`      ${before.length} bytes, ${countSaveSectors(before)} sectors`);
      check('a game that has never saved is not mistaken for one that has',
            !looksLikeASave(before),
            `${countSaveSectors(before)} sectors on a chip nobody has written to`);
    }

    // --- and after saving -----------------------------------------------
    console.log('\n--- saving in-game ---');
    await runDebugScript(rig, 0, SAVE_SCRIPT);
    await rig.wait(600);
    for (let i = 0; i < 20 && await isBusy(rig, 0); i++) {
      await rig.tap(0, 'B', 6);
      await rig.wait(20);
    }

    const after = await chip(rig);
    if (after) {
      console.log(`      ${after.length} bytes, ${countSaveSectors(after)} sectors`);
      check('and a saved game is recognised', looksLikeASave(after),
            `only ${countSaveSectors(after)} sectors after saving`);
    } else {
      check('and a saved game is recognised', false, 'no chip after saving');
    }

    console.log(failures === 0 ? '\nboth ways round' : `\n${failures} failed`);
    if (failures) process.exitCode = 1;
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
