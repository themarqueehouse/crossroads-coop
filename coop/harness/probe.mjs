#!/usr/bin/env node
// Scratch probe: photograph every step of the co-op battle entry.
//
// Not a check. The breadcrumbs say CB2_InitBattleInternal never runs, and the
// screen says a four-battler double battle is in progress. Both cannot be
// true, so one of them is measuring something other than what I think. This
// looks, rather than infers.
import { startRig, OFFSETS } from './rig.mjs';

const PORT = 8795;
const ROM = process.argv[2] || '/home/claude/crossroads/pokeemerald.gba';
const SCRIPTS_MENU_INDEX = 5;

const isBusy = async (rig, w) =>
  (await rig.mailbox(w)).flags.includes('SCRIPT_BUSY');

async function freeUp(rig, w) {
  for (let i = 0; i < 12 && await isBusy(rig, w); i++) {
    await rig.tap(w, 'B', 6);
    await rig.wait(20);
  }
}

async function state(rig, tag) {
  const out = [];
  for (const w of [0, 1]) {
    out.push(`p${w + 1} path=${await rig.u8(w, OFFSETS.dbgPathAddr)} ` +
             `started=${await rig.u8(w, OFFSETS.dbgReachedAddr)} ` +
             `flags=${(await rig.mailbox(w)).flags}`);
  }
  console.log(`  [${tag}] ` + out.join('   |   '));
}

async function main() {
  const rig = await startRig({ rom: ROM, port: PORT });
  try {
    // Give both consoles a Pokemon first, exactly as stage5 does -- that is
    // the only difference between this run (which reaches the battle) and the
    // one that appeared to stall.
    for (const w of [0, 1]) {
      await freeUp(rig, w);
      await rig.hold(w, 'R'); await rig.wait(6);
      await rig.tap(w, 'Start', 8); await rig.wait(20);
      await rig.letGo(w, 'R'); await rig.wait(20);
      for (let i = 0; i < SCRIPTS_MENU_INDEX; i++) { await rig.tap(w, 'Down', 6); await rig.wait(8); }
      await rig.tap(w, 'A', 8); await rig.wait(25);
      for (let i = 1; i < 5; i++) { await rig.tap(w, 'Down', 6); await rig.wait(8); }
      await rig.tap(w, 'A', 8); await rig.wait(40);
      await freeUp(rig, w);
    }

    await freeUp(rig, 0);
    await rig.shot('/tmp/claude-0/pb-0-idle');
    await state(rig, 'idle');

    // Open the debug menu and photograph each navigation step, because
    // "Script 4" is reached by counting keypresses and a miscount lands on
    // something else entirely -- the debug menu has its own AI test battle.
    await rig.hold(0, 'R');
    await rig.wait(6);
    await rig.tap(0, 'Start', 8);
    await rig.wait(20);
    await rig.letGo(0, 'R');
    await rig.wait(20);
    await rig.shot('/tmp/claude-0/pb-1-menu-open');

    for (let i = 0; i < SCRIPTS_MENU_INDEX; i++) { await rig.tap(0, 'Down', 6); await rig.wait(8); }
    await rig.shot('/tmp/claude-0/pb-2-on-scripts');

    await rig.tap(0, 'A', 8);
    await rig.wait(25);
    await rig.shot('/tmp/claude-0/pb-3-scripts-submenu');

    for (let i = 1; i < 4; i++) { await rig.tap(0, 'Down', 6); await rig.wait(8); }
    await rig.shot('/tmp/claude-0/pb-4-on-script4');

    await rig.tap(0, 'A', 8);
    await rig.wait(30);
    await rig.shot('/tmp/claude-0/pb-5-just-run');
    await state(rig, 'just ran');

    // Then watch the entry unfold, without pressing anything.
    for (let i = 0; i < 10; i++) {
      await rig.wait(120);
      await rig.shot(`/tmp/claude-0/pb-6-settle${i}`);
      await state(rig, `+${(i + 1) * 120}f`);
    }
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
