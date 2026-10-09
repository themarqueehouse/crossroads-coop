#!/usr/bin/env node
// Stage-11 check: travelling takes the other player with you.
//
// Fly, Teleport, Dig and an Escape Rope all cross the map in one step. Left
// alone, one player ends up in Lilycove and the other in Petalburg, and from
// then on every gym door and every story scene refuses, because all of them
// need both players present. Getting back together is a long walk.
//
// What crosses the link is not a destination -- the four moves settle on
// theirs at four different points, some of them after the screen has already
// faded -- but "I am travelling, come with me". The partner waits until it can
// see the traveller standing somewhere that is not where it is, and warps to
// them. Deciding on arrival is what lets one mechanism cover all four moves.
//
// Driven here through coopfollowme + warp, which is the same pair of things
// Fly does, because a test rig cannot work the fly map. The hook inside each
// of the four moves is one line calling the same function.
//
// The second half matters as much as the first: an ORDINARY warp must not drag
// anybody. Every door in the game is a warp, and a partner yanked through each
// one would be far worse than a partner left behind.
//
//   node coop/harness/stage11.mjs path/to/rom.gba
import { startRig, tally, OFFSETS } from './rig.mjs';

const PORT = 8803;
const ROM = process.argv[2] || '/home/claude/crossroads/pokeemerald.gba';
const SCRIPTS_MENU_INDEX = 5;

const GYM_MAP = 0x030b;
// MAP_RUSTBORO_CITY = (3 | (0 << 8)), encoded as mapGroup | mapNum << 8.
const CITY_MAP = 0x0300;

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

const mapOf = async (rig, w) => parseInt((await rig.mailbox(w)).selfMap, 16);

async function main() {
  const rig = await startRig({ rom: ROM, port: PORT });
  const t = tally();

  try {
    const m0 = await rig.mailboxes();
    if (m0.some((x) => x.state !== 'ACTIVE'))
      throw new Error('the session never reached ACTIVE');

    const startMap = await mapOf(rig, 0);
    t.note('starting out', `both on 0x${startMap.toString(16)}`);
    t.check('both players start together',
            startMap === (await mapOf(rig, 1)) && startMap !== GYM_MAP);

    // ------------------------------------------------------- travelling
    console.log('\n--- player 1 travels ---');
    await runDebugScript(rig, 0, 12);

    let together = false;
    for (let i = 0; i < 25 && !together; i++) {
      await rig.wait(60);
      together = (await mapOf(rig, 0)) === GYM_MAP
              && (await mapOf(rig, 1)) === GYM_MAP;
    }
    await rig.wait(120);
    await freeUp(rig, 0);
    await freeUp(rig, 1);
    await rig.shot('/tmp/claude-0/stage11-followed');

    t.note('maps', `p1=0x${(await mapOf(rig, 0)).toString(16)} ` +
                   `p2=0x${(await mapOf(rig, 1)).toString(16)}`);
    t.check('the traveller arrived', (await mapOf(rig, 0)) === GYM_MAP);
    t.check('and the partner came too', (await mapOf(rig, 1)) === GYM_MAP,
            'player 2 was left behind');

    // ------------------------------------------- an ordinary warp must not
    //
    // To a THIRD map, not back to the start. Warping somewhere one of them
    // already is would let "the partner stayed put" pass for the wrong reason.
    console.log('\n--- and now an ordinary warp, which must not drag ---');
    await runDebugScript(rig, 0, 13);
    await rig.wait(420);
    await freeUp(rig, 0);

    t.note('maps', `p1=0x${(await mapOf(rig, 0)).toString(16)} ` +
                   `p2=0x${(await mapOf(rig, 1)).toString(16)}`);
    t.check('player 1 moved', (await mapOf(rig, 0)) === CITY_MAP);
    t.check('and player 2 was left where they were',
            (await mapOf(rig, 1)) === GYM_MAP,
            'an ordinary warp dragged the partner -- every door in the game is one of these');

    const m = await rig.mailboxes();
    t.check('and the session survived all of it',
            m.every((x) => x.state === 'ACTIVE'),
            m.map((x) => x.state).join('/'));

    t.check('no communication error',
            (await rig.u8(0, OFFSETS.linkErrorSiteAddr)) === 0 &&
            (await rig.u8(1, OFFSETS.linkErrorSiteAddr)) === 0);

    t.summary();
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
