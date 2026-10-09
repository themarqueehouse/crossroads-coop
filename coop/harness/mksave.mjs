#!/usr/bin/env node
// Build a mid-game save file to start a playtest from.
//
// Starting every playtest in a bedroom with no Pokemon means the things most
// worth testing -- surfing, flying, Pokemon Centers, shops, caves -- are an
// hour of play away each time. This runs the game far enough to have a
// character, hands it eight badges, every HM and a team, saves, and writes the
// save out as a .sav the emulator can load.
//
// Only Player 1 needs it. Player 2's character lives inside Player 1's save.
//
//   node coop/harness/mksave.mjs [out.sav]
import { writeFileSync } from 'node:fs';
import { startRig } from './rig.mjs';

const PORT = 8805;
const ROM = '/home/claude/crossroads/pokeemerald.gba';
const OUT = process.argv[2] || '/home/claude/crossroads/coop/crossroads-midgame.sav';
const SCRIPTS_MENU_INDEX = 5;

const isBusy = async (rig, w) =>
  (await rig.mailbox(w)).flags.includes('SCRIPT_BUSY');

async function freeUp(rig, w) {
  for (let i = 0; i < 16 && await isBusy(rig, w); i++) {
    await rig.tap(w, 'B', 6);
    await rig.wait(20);
  }
}

async function runDebugScript(rig, w, slot) {
  await freeUp(rig, w);
  await rig.hold(w, 'R');
  await rig.wait(6);
  await rig.tap(w, 'Start', 8);
  await rig.wait(20);
  await rig.letGo(w, 'R');
  await rig.wait(20);
  for (let i = 0; i < SCRIPTS_MENU_INDEX; i++) { await rig.tap(w, 'Down', 6); await rig.wait(8); }
  await rig.tap(w, 'A', 8);
  await rig.wait(25);
  for (let i = 1; i < slot; i++) { await rig.tap(w, 'Down', 6); await rig.wait(8); }
  await rig.tap(w, 'A', 8);
  await rig.wait(25);
}

async function main() {
  const rig = await startRig({ rom: ROM, port: PORT });

  try {
    const m = await rig.mailboxes();
    if (m.some((x) => x.state !== 'ACTIVE'))
      throw new Error('the session never reached ACTIVE; nothing to save');

    console.log('\n--- kitting the player out ---');
    await runDebugScript(rig, 0, 14);

    // Wait for the script to actually finish rather than guessing at how long
    // it takes. Thirteen items and six Pokemon is nineteen message sequences,
    // each wanting a press; a fixed wait ran out somewhere around HM07 and
    // the save script that followed never ran at all.
    let done = false;
    for (let i = 0; i < 400 && !done; i++) {
      await rig.tap(0, 'B', 6);
      await rig.wait(14);
      if (i % 10 === 9) done = !(await isBusy(rig, 0));
    }
    await rig.wait(120);
    await freeUp(rig, 0);
    await rig.shot('/tmp/claude-0/mksave-kitted');
    if (!done) throw new Error('the setup script never finished');

    console.log('\n--- saving ---');
    // Script 15 empties the Player 2 slot and writes in the same breath, so
    // the partner console the rig runs cannot leave its character in the file
    // somebody else is going to start from.
    await runDebugScript(rig, 0, 15);
    // The save itself takes a moment and must not be interrupted.
    await rig.wait(600);
    await rig.shot('/tmp/claude-0/mksave-saved');

    // Flush the emulator's filesystem, then take the save bytes.
    const save = await rig.page.evaluate(async () => {
      await window.__pair.sync();
      const s = window.__pair.save();
      return s ? Array.from(s) : null;
    });

    if (!save) throw new Error('the core had no save data to give');

    writeFileSync(OUT, Buffer.from(save));
    console.log(`\nwrote ${OUT} (${save.length} bytes)`);

    // A GBA save is 128 KiB of flash in 4 KiB sectors, and Emerald stamps each
    // one it writes with 0x08012025 near the end. Counting those is the check
    // that means something: an untouched chip reads as all 0xFF or all 0x00
    // depending on the emulator, and the first version of this looked only for
    // 0xFF -- so a save file of 131072 zero bytes was reported as entirely
    // non-blank, which is true and worthless.
    let sectors = 0;
    for (let off = 0; off + 4096 <= save.length; off += 4096) {
      const sig = save[off + 0xFF8] | (save[off + 0xFF9] << 8)
                | (save[off + 0xFFA] << 16) | (save[off + 0xFFB] << 24);
      if (sig >>> 0 === 0x08012025) sectors++;
    }
    console.log(`${sectors} sectors carry Emerald's save signature`);
    if (sectors < 14)
      throw new Error(`only ${sectors} valid sectors -- the save did not happen`);
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
