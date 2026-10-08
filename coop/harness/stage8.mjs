#!/usr/bin/env node
// Stage-8 check: trainers who SPOT you, rather than waiting to be spoken to.
//
// Every co-op battle proved so far has been talk-triggered: the player walks
// up, presses A, and the script runs. That is how gym leaders work and it is
// how all the debug scripts work -- and it is the minority case. Counting the
// object events whose scripts the generator rewrote, 95 are TRAINER_TYPE_NORMAL
// with a line of sight and only 34 are talked to. If the gates do not work
// when a trainer starts the script himself, most of the battles in the game
// are broken and none of the tests would have said so.
//
// Two things to find out, in increasing order of how much they would hurt:
//
//   1. Does a sight-triggered script mirror at all? The trainer only spots the
//      player on ONE console. The other player's game has no idea anything
//      happened, so the whole encounter has to travel over coopscene.
//
//   2. What happens when the partner is NOT there? A talk-triggered refusal is
//      harmless: the message closes and the player walks away. A sight
//      trigger has no such exit -- the player is still standing in the line of
//      sight, so the trainer spots them again, and refuses again, forever. If
//      that happens the player is soft-locked somewhere they cannot leave.
//
//   node coop/harness/stage8.mjs path/to/rom.gba
import { startRig, tally, OFFSETS } from './rig.mjs';

const PORT = 8797;
const ROM = process.argv[2] || '/home/claude/crossroads/pokeemerald.gba';
const SCRIPTS_MENU_INDEX = 5;

const GYM_MAP = 0x030b;
// Josh, the youngster at (5,13) facing down with a sight range of 2. Script 10
// lands the player at (5,14), one tile inside that.
const GATE_JOSH = 300 + 80;

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

    console.log('\n--- giving both players a party ---');
    for (const w of [0, 1]) {
      await runDebugScript(rig, w, 5);
      await rig.wait(60);
      await freeUp(rig, w);
    }

    // -------------------------------------------------- spotted, and alone
    console.log('\n--- player 1 walks into Josh\'s line of sight, alone ---');
    await rig.clearGateLog();
    await runDebugScript(rig, 0, 10);
    await rig.wait(300);
    await rig.shot('/tmp/claude-0/stage8-spotted-alone');

    t.check('player 1 is in the gym', (await mapOf(rig, 0)) === GYM_MAP);
    t.check('player 2 is elsewhere', (await mapOf(rig, 1)) !== GYM_MAP);
    t.note('gates', `p1=[${await rig.gateLog(0)}] p2=[${await rig.gateLog(1)}]`);
    t.check('the fight was refused',
            !(await rig.gateLog(0)).includes(GATE_JOSH),
            'it started with only one player present');

    // The soft-lock check: does he actually MOVE?
    //
    // Not "is a script running". A gym full of trainers means walking out
    // crosses other lines of sight, so a refusal message is on screen a good
    // deal of the time and catching one proves nothing -- the first version of
    // this check asked that and reported a soft-lock that was not there.
    //
    // Player 1's position is read from player 2's mailbox, which is where it
    // is broadcast. If it changes while we press B and a direction, the player
    // has control and the refusal is an interruption rather than a trap.
    console.log('\n--- can he walk away again? ---');
    const whereIsP1 = async () => (await rig.mailbox(1)).peerPos;
    const startedAt = await whereIsP1();
    const seen = new Set([startedAt]);
    for (let i = 0; i < 24; i++) {
      await rig.tap(0, 'B', 6);
      await rig.wait(18);
      await rig.tap(0, i % 2 ? 'Down' : 'Left', 12);
      await rig.wait(22);
      seen.add(await whereIsP1());
    }
    await rig.wait(90);
    await rig.shot('/tmp/claude-0/stage8-walked-away');
    t.note('player 1 moved', `${startedAt} -> ${await whereIsP1()} ` +
                             `(${seen.size} distinct tiles)`);
    t.check('player 1 is not trapped in the refusal', seen.size > 1,
            `never left ${startedAt} across twenty-four attempts to walk`);

    // --------------------------------------------------- spotted, together
    console.log('\n--- player 2 joins, and player 1 gets spotted again ---');
    await runDebugScript(rig, 1, 9);
    await rig.wait(240);
    await freeUp(rig, 1);
    t.check('both players are in the gym',
            (await mapOf(rig, 0)) === GYM_MAP && (await mapOf(rig, 1)) === GYM_MAP);

    await rig.clearGateLog();
    // Back into the line of sight. The warp is the reliable way in: walking
    // there depends on where the previous step left him.
    await runDebugScript(rig, 0, 10);
    await rig.wait(420);
    await rig.shot('/tmp/claude-0/stage8-spotted-together');

    t.note('gates', `p1=[${await rig.gateLog(0)}] p2=[${await rig.gateLog(1)}]`);
    t.check('the sight-triggered script mirrored to the partner',
            (await rig.gateLog(0)).includes(GATE_JOSH) &&
            (await rig.gateLog(1)).includes(GATE_JOSH),
            'the partner never saw the encounter the trainer started');

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
