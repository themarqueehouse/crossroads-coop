#!/usr/bin/env node
// Stage-4 check: does a real link battle survive the co-op transport?
//
// This is the question the whole 2-player battle feature rests on. The battle
// machinery does not merely send a few extra commands: it CLOSES the link,
// reopens it, runs its own player exchange, and then ships every controller
// command over the block layer for the rest of the fight. None of that has ever
// run over the EWRAM mailbox.
//
// Script 4 is a mirrored scene containing a coopbattle, so one console triggers
// it and both should end up in the same battle.
//
//   node coop/harness/stage4.mjs path/to/rom.gba
import { startRig, tally, OFFSETS } from './rig.mjs';

const PORT = 8791;
const ROM = process.argv[2] || '/home/claude/crossroads/pokeemerald.gba';
const SCRIPTS_MENU_INDEX = 5;
const GATE_TEST_BATTLE = 4;

const isBusy = async (rig, w) =>
  (await rig.mailbox(w)).flags.includes('SCRIPT_BUSY');

async function freeUp(rig, w) {
  for (let i = 0; i < 12 && await isBusy(rig, w); i++) {
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
  const t = tally();

  try {
    let m = await rig.mailboxes();
    if (m.some((x) => x.state !== 'ACTIVE'))
      throw new Error('the session never reached ACTIVE');

    console.log('\n--- player 1 starts a co-op battle ---');
    await rig.clearGateLog();
    await runDebugScript(rig, 0, 4);

    // Watch the session go down, which is the handover, then watch for the
    // battle to actually appear. Sampled rather than checked once: the whole
    // point is to see WHERE it stops if it stops.
    for (let i = 0; i < 12; i++) {
      await rig.wait(90);
      m = await rig.mailboxes();
      console.log(`  t+${(i + 1) * 90}: ` + m.map((x) =>
        `p${x.id} ${x.state} gate=${x.gateId} out=${x.outPending} hb=${x.heartbeat}`).join('   '));
    }

    // Which "Communication error" fired, if any. See gCoopLinkErrorSite.
    const SITES = ['(none)', 'player-exchange magic mismatch',
                   'link players stopped matching saved', 'gLinkStatus error bit'];
    for (const w of [0, 1]) {
      const site = await rig.u8(w, OFFSETS.linkErrorSiteAddr);
      const status = await rig.u32(w, OFFSETS.linkErrorStatusAddr);
      t.note(`p${w + 1} link error`,
             `${SITES[site] ?? site} (status 0x${status.toString(16)})`);
    }
    t.check('no console threw a communication error',
            (await rig.u8(0, OFFSETS.linkErrorSiteAddr)) === 0 &&
            (await rig.u8(1, OFFSETS.linkErrorSiteAddr)) === 0);

    // The battle itself. Screenshots are good evidence and a terrible check:
    // gBattleTypeFlags says what kind of battle is running, and four battlers
    // says it is a double.
    const BATTLE_TYPE_DOUBLE = 1 << 0;
    const BATTLE_TYPE_LINK   = 1 << 1;
    const BATTLE_TYPE_TRAINER = 1 << 3;
    const BATTLE_TYPE_MULTI  = 1 << 6;
    const WANT = BATTLE_TYPE_DOUBLE | BATTLE_TYPE_LINK | BATTLE_TYPE_TRAINER | BATTLE_TYPE_MULTI;

    for (const w of [0, 1]) {
      const flags = await rig.u32(w, OFFSETS.battleTypeFlagsAddr);
      const battlers = await rig.u32(w, OFFSETS.battlersCountAddr);
      t.note(`p${w + 1} battle`, `flags=0x${flags.toString(16)} battlers=${battlers}`);
      t.check(`p${w + 1} is in a linked two-trainer double battle`,
              (flags & WANT) === WANT && battlers === 4,
              flags === 0 ? 'no battle started at all' : '');
    }

    const logs = [await rig.gateLog(0), await rig.gateLog(1)];
    t.note('gates', logs.map((l) => JSON.stringify(l)).join(' / '));
    t.check('both consoles reached the battle gate',
            logs.every((l) => l.includes(GATE_TEST_BATTLE)));

    // Both cores still running frames at all is the crudest and most important
    // check: a botched link handover hangs the ROM rather than erroring.
    const before = await rig.page.evaluate(() => window.__pair.frames());
    await rig.wait(120);
    const after = await rig.page.evaluate(() => window.__pair.frames());
    t.note('frames', `${before} -> ${after}`);
    t.check('neither console hung', after.every((f, i) => f > before[i]));

    await rig.shot('/tmp/claude-0/stage4-battle');
    t.summary();
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
