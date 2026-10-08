#!/usr/bin/env node
// Stage-5 check: can a co-op battle be FOUGHT, and does it end cleanly?
//
// Stage 4 proved a co-op battle starts on both consoles. Starting is not the
// hard half. The configuration we ride is the Battle Tower's, and the tower has
// opinions about what happens when a battle ends -- prize money, its own
// win/loss bookkeeping, where it sends you afterwards. None of that has run
// over this transport either, and sixteen gyms is a bad place to find out.
//
// So: give both consoles something that can win, start the battle, mash
// through it, and watch for three things that matter more than the result.
//   1. the battle ends at all, rather than hanging mid-fight
//   2. both consoles come back to the overworld
//   3. the co-op session rebuilds itself afterwards -- it was suspended for the
//      battle, and if it does not come back the two players are standing in the
//      same world unable to see each other
//
//   node coop/harness/stage5.mjs path/to/rom.gba
import { startRig, tally, OFFSETS } from './rig.mjs';

const PORT = 8793;
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

// Whether a co-op battle is running.
//
// NOT gBattleTypeFlags: the game never clears it when a battle ends, so it
// still reads as a four-battler linked double battle while the player walks
// around the overworld afterwards. An earlier version of this check believed
// that and reported a battle which had in fact finished minutes ago. The
// session's own suspend flag is published every frame it is stood down for a
// battle, and only then.
const inBattle = async (rig, w) =>
  (await rig.mailbox(w)).flags.includes('BATTLE');

async function main() {
  const rig = await startRig({ rom: ROM, port: PORT });
  const t = tally();

  try {
    let m = await rig.mailboxes();
    if (m.some((x) => x.state !== 'ACTIVE'))
      throw new Error('the session never reached ACTIVE');

    console.log('\n--- giving both players something that can fight ---');
    for (const w of [0, 1]) {
      await runDebugScript(rig, w, 5);
      await rig.wait(60);
      await freeUp(rig, w);
    }

    console.log('\n--- starting the co-op battle ---');
    await rig.clearGateLog();
    await runDebugScript(rig, 0, 4);
    await rig.wait(400);

    t.check('both consoles are in the battle',
            (await inBattle(rig, 0)) && (await inBattle(rig, 1)));

    // How many Pokemon each trainer slot actually has. gPartiesCount is
    // indexed by B_TRAINER_0..3: your party, opponent A, your partner's party,
    // opponent B. Opponents reading 0 means the generation never happened,
    // which is what "two trainers at level 0" looks like from the inside.
    for (const w of [0, 1]) {
      const counts = await rig.readAt(w, OFFSETS.partiesCountAddr, 4);
      t.note(`p${w + 1} party counts`,
             `you=${counts[0]} foeA=${counts[1]} partner=${counts[2]} foeB=${counts[3]}`);
    }
    {
      const c0 = await rig.readAt(0, OFFSETS.partiesCountAddr, 4);
      t.check('the opponents have Pokemon at all', c0[1] > 0 && c0[3] > 0,
              'generation produced empty parties');
    }
    for (const w of [0, 1]) {
      t.note(`p${w + 1} generation`,
        `branch ran=${await rig.u8(w, OFFSETS.dbgMadeAddr)} ` +
        `foeA made=${await rig.u8(w, OFFSETS.dbgFoeAAddr)} ` +
        `foeB made=${await rig.u8(w, OFFSETS.dbgFoeBAddr)} ` +
        `count right after=${await rig.u8(w, OFFSETS.dbgAfterAddr)}`);
      t.note(`p${w + 1} init`,
        `reached CB2_InitBattleInternal=${await rig.u8(w, OFFSETS.dbgReachedAddr)} ` +
        `coopActive=${await rig.u8(w, OFFSETS.dbgCoopActiveAddr)} ` +
        `isDebugBattle=${await rig.u8(w, OFFSETS.dbgIsDebugAddr)} ` +
        `path=${await rig.u8(w, OFFSETS.dbgPathAddr)} ` +
        `multiuseState=${await rig.u8(w, OFFSETS.battleCommAddr)}`);
    }

    // Mash. In a double battle A walks FIGHT -> move -> target, and a level 50
    // starter against two bug catchers does not need the moves chosen well.
    // B as well as A, so a mistaken menu does not park us in a submenu for ever.
    console.log('\n--- fighting it ---');
    let endedAt = null;
    for (let round = 0; round < 60; round++) {
      await rig.tap('both', 'A', 8);
      await rig.wait(24);
      if (round % 7 === 6) { await rig.tap('both', 'B', 8); await rig.wait(24); }

      if (round % 5 === 4) {
        const live = [await inBattle(rig, 0), await inBattle(rig, 1)];
        if (!live[0] && !live[1]) { endedAt = round; break; }
      }
    }

    t.note('battle ended', endedAt === null ? 'NO -- still going after 60 rounds'
                                            : `after ~${endedAt} rounds of input`);
    t.check('the battle ended', endedAt !== null);

    await rig.shot('/tmp/claude-0/stage5-after-battle');

    // Back to the overworld, and the session rebuilt.
    console.log('\n--- after the battle ---');
    for (let i = 0; i < 10; i++) {
      await rig.wait(120);
      m = await rig.mailboxes();
      console.log(`  t+${(i + 1) * 120}: ` + m.map((x) =>
        `p${x.id} ${x.state} map=${x.selfMap} flags=${x.flags}`).join('   '));
      if (m.every((x) => x.state === 'ACTIVE')) break;
      await rig.tap('both', 'A', 8);
    }

    m = await rig.mailboxes();
    t.check('both consoles came back to the overworld',
            !(await inBattle(rig, 0)) && !(await inBattle(rig, 1)));
    t.check('the co-op session rebuilt itself',
            m.every((x) => x.state === 'ACTIVE'),
            m.map((x) => x.state).join('/'));
    t.check('they can see each other again',
            m.every((x) => x.flags.includes('PEER_SAME_MAP')),
            m.map((x) => x.flags).join(' | '));

    for (const w of [0, 1]) {
      const site = await rig.u8(w, OFFSETS.linkErrorSiteAddr);
      if (site) t.note(`p${w + 1} link error`, `site ${site}`);
    }
    t.check('no communication error anywhere',
            (await rig.u8(0, OFFSETS.linkErrorSiteAddr)) === 0 &&
            (await rig.u8(1, OFFSETS.linkErrorSiteAddr)) === 0);

    await rig.shot('/tmp/claude-0/stage5-back-in-world');
    t.summary();
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
