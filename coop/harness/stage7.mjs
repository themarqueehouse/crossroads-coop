#!/usr/bin/env node
// Stage-7 check: a REAL gym, not a debug script shaped like one.
//
// Everything so far has been proved against Debug_EventScript_Script_7, which
// I wrote to look like a gym: coopscene, coopnextbattle_split, trainerbattle.
// A test written against my own idea of the pattern cannot tell me the pattern
// is wrong. This one walks into Rustboro Gym and talks to Roxanne, so what
// runs is data/maps/RustboroCity_Gym/scripts.inc as the generator left it,
// off the real object event, with trainerbattle_single's own bookkeeping --
// VAR_RESULT, the defeated flag, the post-battle branch -- in the loop.
//
// Two halves, in the order a player would hit them:
//   1. one player in the gym alone -> refused, and able to walk away
//   2. both in the gym -> gate, picker, and Roxanne's three Pokemon dealt
//      across herself and the filler partner (2 + 1)
//
//   node coop/harness/stage7.mjs path/to/rom.gba
import { startRig, tally, OFFSETS, pickThreeOnBoth } from './rig.mjs';

const PORT = 8795;
const ROM = process.argv[2] || '/home/claude/crossroads/pokeemerald.gba';
const SCRIPTS_MENU_INDEX = 5;

// mapGroup | (mapNum << 8) -- the encoding coop.c publishes. Rustboro Gym is
// MAP_RUSTBORO_CITY_GYM = (3 | (11 << 8)), so group 11, num 3.
const GYM_MAP = 0x030b;
const GATE_GYM_ROXANNE = 200;

// Roxanne's real team is three. Coop_BuildSplitOpponents keeps (3+1)/2 = 2 and
// hands the back half to the partner slot, so the fight should be 2 + 1 -- the
// same three Pokemon a solo player faces, not six.
const FOE_A_EXPECTED = 2;
const FOE_B_EXPECTED = 1;

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

const mapOf = async (rig, w) =>
  parseInt((await rig.mailbox(w)).selfMap, 16);

// Turn north and talk. Roxanne stands on the tile above, so Up cannot move the
// player onto her -- it only turns them to face her, which is what the A needs.
// The turn is not instant, hence the wait between the two.
async function talkNorth(rig, w) {
  await rig.hold(w, 'Up');
  await rig.wait(14);
  await rig.letGo(w, 'Up');
  await rig.wait(24);
  await rig.tap(w, 'A', 8);
  await rig.wait(60);
}

async function main() {
  const rig = await startRig({ rom: ROM, port: PORT });
  const t = tally();

  try {
    let m = await rig.mailboxes();
    if (m.some((x) => x.state !== 'ACTIVE'))
      throw new Error('the session never reached ACTIVE');

    console.log('\n--- giving both players a party ---');
    for (const w of [0, 1]) {
      await runDebugScript(rig, w, 5);
      await rig.wait(60);
      await freeUp(rig, w);
    }

    // ---------------------------------------------------------------- alone
    console.log('\n--- player 1 walks into the gym alone ---');
    await rig.clearGateLog();
    await runDebugScript(rig, 0, 9);
    await rig.wait(180);
    await freeUp(rig, 0);

    t.note('maps', `p1=0x${(await mapOf(rig, 0)).toString(16)} ` +
                   `p2=0x${(await mapOf(rig, 1)).toString(16)}`);
    t.check('player 1 is in Rustboro Gym', (await mapOf(rig, 0)) === GYM_MAP,
            `on 0x${(await mapOf(rig, 0)).toString(16)}, not 0x${GYM_MAP.toString(16)}`);
    t.check('player 2 is somewhere else', (await mapOf(rig, 1)) !== GYM_MAP);

    console.log('\n--- and challenges Roxanne by himself ---');
    await talkNorth(rig, 0);
    await rig.wait(120);
    await rig.shot('/tmp/claude-0/stage7-alone');

    t.note('gates', `p1=[${await rig.gateLog(0)}] p2=[${await rig.gateLog(1)}]`);
    t.check('the challenge was refused',
            !(await rig.gateLog(0)).includes(GATE_GYM_ROXANNE),
            'the gate opened with only one player in the gym');
    t.check('no battle started',
            !(await rig.mailbox(0)).flags.includes('BATTLE'));

    // The refusal must not trap him. A gym leader is talk-triggered, so this
    // should be a message and a release -- but if the gate were left standing
    // the player would be frozen in front of her with no partner coming.
    await freeUp(rig, 0);
    await rig.wait(60);
    t.check('player 1 can move again afterwards', !(await isBusy(rig, 0)),
            'still locked in a script');

    // --------------------------------------------------------------- together
    console.log('\n--- player 2 joins him ---');
    await rig.clearGateLog();
    await runDebugScript(rig, 1, 9);
    await rig.wait(180);
    await freeUp(rig, 1);

    t.check('both players are in the gym',
            (await mapOf(rig, 0)) === GYM_MAP && (await mapOf(rig, 1)) === GYM_MAP);
    m = await rig.mailboxes();
    t.check('and can see each other',
            m.every((x) => x.flags.includes('PEER_SAME_MAP')),
            m.map((x) => x.flags).join(' | '));
    await rig.shot('/tmp/claude-0/stage7-both-in-gym');

    console.log('\n--- challenging Roxanne together ---');
    await talkNorth(rig, 0);
    await rig.wait(240);

    t.note('gates', `p1=[${await rig.gateLog(0)}] p2=[${await rig.gateLog(1)}]`);
    t.check('the gym gate opened on both consoles',
            (await rig.gateLog(0)).includes(GATE_GYM_ROXANNE) &&
            (await rig.gateLog(1)).includes(GATE_GYM_ROXANNE),
            'the mirrored scene did not reach the partner');

    // The picker opens on both; the guest's a little later, since its copy of
    // the scene starts after the host's. pickThreeOnBoth waits for each choice
    // to land rather than pressing to a fixed rhythm.
    await rig.wait(180);
    await rig.shot('/tmp/claude-0/stage7-picker');
    const picked = await pickThreeOnBoth(rig, OFFSETS.selectedOrderAddr, OFFSETS.pickerOpenedAddr);
    t.note('chosen', `p1=${picked[0]} p2=${picked[1]}`);
    t.check('both players chose three', picked[0] === 3 && picked[1] === 3,
            `p1 chose ${picked[0]}, p2 chose ${picked[1]}`);
    await rig.wait(120);

    // Past the picker Roxanne has her say before the fight. The debug script
    // this flow was built against used trainerbattle_no_intro and had no such
    // line, so nothing here pressed through dialogue; against the real gym
    // script that left both consoles sitting on her intro text, which read as
    // a battle that never started.
    //
    // Both consoles are in the mirrored scene, so both have the text up and
    // both need the button. Poll rather than guess at the length: the entry
    // sequence -- link teardown, rebuild, player exchange, party preview --
    // runs about 480 frames after that before the opponents exist, and a fixed
    // wait shorter than the sum reports an empty battle with great confidence.
    let setUp = false;
    for (let i = 0; i < 30 && !setUp; i++) {
      await rig.tap('both', 'A', 8);
      await rig.wait(60);
      setUp = (await rig.u8(0, OFFSETS.dbgPathAddr)) === 14
           && (await rig.u8(1, OFFSETS.dbgPathAddr)) === 14;
    }
    t.check('the battle set up on both consoles', setUp,
            'still in the entry sequence after 1500 frames');
    await rig.wait(120);
    await rig.shot('/tmp/claude-0/stage7-battle');

    for (const w of [0, 1]) {
      const c = await rig.readAt(w, OFFSETS.partiesCountAddr, 4);
      t.note(`p${w + 1} party counts`,
             `you=${c[0]} foeA=${c[1]} partner=${c[2]} foeB=${c[3]}`);
    }

    const c0 = await rig.readAt(0, OFFSETS.partiesCountAddr, 4);
    t.check(`Roxanne's team was dealt ${FOE_A_EXPECTED} + ${FOE_B_EXPECTED}`,
            c0[1] === FOE_A_EXPECTED && c0[3] === FOE_B_EXPECTED,
            `got ${c0[1]} + ${c0[3]} -- her three should split, not duplicate`);
    // Each console's own party, read on its own console -- not gPartiesCount's
    // partner slot. That slot stays 0 here on both sides: in a link multi each
    // console counts only what it brought, and the partner's Pokemon arrive
    // over the link into gParties without the count being mirrored. Asserting
    // on it reported a half-empty battle that the VS screen plainly showed was
    // nothing of the kind.
    const c1 = await rig.readAt(1, OFFSETS.partiesCountAddr, 4);
    t.check('both players brought three',
            c0[0] === 3 && c1[0] === 3,
            `p1 brought ${c0[0]}, p2 brought ${c1[0]}`);

    for (const w of [0, 1]) {
      const site = await rig.u8(w, OFFSETS.linkErrorSiteAddr);
      if (site) t.note(`p${w + 1} link error`, `site ${site}`);
    }
    t.check('no communication error',
            (await rig.u8(0, OFFSETS.linkErrorSiteAddr)) === 0 &&
            (await rig.u8(1, OFFSETS.linkErrorSiteAddr)) === 0);

    t.summary();
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
