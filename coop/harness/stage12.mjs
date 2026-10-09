#!/usr/bin/env node
// Stage-12 check: a story scene is paced by one player, not two.
//
// Both consoles run the same script. Left to themselves each printed at its
// own text speed and advanced on its own button press, and the two drifted
// apart a message box at a time -- which in the first real playtest meant one
// player finished talking to Oak, walked away with him, and left their partner
// standing in front of a second copy of Oak that was still mid-sentence.
//
// Whoever walks into the scene paces it now. What that has to mean:
//   1. the driver's presses move BOTH consoles on
//   2. the follower's own presses move NOTHING -- otherwise the faster player
//      still gets ahead, which is the whole bug
//   3. they come out of it together
//
// Measured on the pacing counters themselves: how many times the driver has
// pressed on, and how many of those the follower has used. The script pointer
// looks like the obvious thing to compare and cannot do this job -- msgbox
// routes every box through one shared routine, so all four boxes park the
// pointer at the same ROM address and two consoles three boxes apart compare
// equal. That check passed on a value that never changed once.
//
// The scene is debug script 17: four message boxes and nothing else. Script 1
// has sync gates either side of its boxes, so the two were hauled back level
// whatever happened in between and the drift could not show.
//
//   node coop/harness/stage12.mjs path/to/rom.gba
import { startRig, tally, OFFSETS } from './rig.mjs';

const PORT = 8800;
const ROM = process.argv[2] || '/home/claude/crossroads/pokeemerald.gba';

const SCRIPTS_MENU_INDEX = 5;
const LOCKSTEP_SCRIPT = 17;

const isBusy = async (rig, w) =>
  (await rig.mailbox(w)).flags.includes('SCRIPT_BUSY');

// Presses the driver has made, and presses the follower has acted on. Both
// run for the whole session, so the test works in deltas from a baseline.
const sent = (rig, w) => rig.u16(w, OFFSETS.sceneAdvanceSentAddr);
const used = (rig, w) => rig.u16(w, OFFSETS.sceneAdvanceUsedAddr);

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
  const t = tally();

  try {
    const m = await rig.mailboxes();
    console.log('\nboth cores up: ' +
      m.map((x) => `p${x.id} ${x.state} map=${x.selfMap}`).join('   '));
    if (m.some((x) => x.state !== 'ACTIVE'))
      throw new Error('the session never reached ACTIVE; nothing below is meaningful');
    if (m[0].selfMap !== m[1].selfMap)
      throw new Error('the two players are not on the same map to begin with');

    console.log('\n--- player 1 walks into the scene ---');
    await runDebugScript(rig, 0, LOCKSTEP_SCRIPT);
    await rig.wait(150);

    t.check('both consoles are in the scene',
            (await isBusy(rig, 0)) && (await isBusy(rig, 1)),
            `p1=${await isBusy(rig, 0)} p2=${await isBusy(rig, 1)}`);

    // ------------------------------------------- the follower presses alone
    //
    // First, because it is the check the old behaviour fails outright: the
    // player who is not driving mashing A is exactly how they used to get
    // ahead. Nothing should move.
    console.log('\n--- player 2 mashes A; nothing should move ---');
    const base = { sent: await sent(rig, 0), used: await used(rig, 1) };
    for (let i = 0; i < 6; i++) { await rig.tap(1, 'A', 8); await rig.wait(25); }
    await rig.wait(60);

    // Six presses against a four-box scene: left to itself the follower would
    // be out the other side of it by now. Whether it is still in the scene is
    // the honest measure, and it does not depend on the mechanism being
    // tested -- reading the advance counter here would be reading a number
    // that only moves when the fix is working, which passes either way.
    const afterMash = { sent: await sent(rig, 0), used: await used(rig, 1) };
    t.note('p2 mashing', `driver pressed ${afterMash.sent - base.sent}  ` +
                         `follower advanced ${afterMash.used - base.used}`);
    t.check('the follower is still in the scene after mashing through it',
            await isBusy(rig, 1),
            'player 2 pressed its own way out while the driver had not moved');
    t.check('and the driver has not been moved on by its partner',
            afterMash.sent === base.sent && await isBusy(rig, 0),
            `driver advanced ${afterMash.sent - base.sent} without pressing`);

    // ------------------------------------------------ the driver presses on
    console.log('\n--- player 1 presses through, one box at a time ---');
    const steps = [];
    let inStep = true;
    for (let box = 0; box < 3; box++) {
      await rig.tap(0, 'A', 8);
      await rig.wait(75);
      const pressed = (await sent(rig, 0)) - base.sent;
      const followed = (await used(rig, 1)) - base.used;
      steps.push(`press${box + 1}: driver ${pressed} follower ${followed}`);
      if (pressed !== followed) inStep = false;
    }
    t.note('after each press', steps.join('  |  '));

    t.check('the follower moved on exactly when the driver did', inStep,
            steps.join('; '));

    // ------------------------------------------------------ and out together
    console.log('\n--- and out of it, on the same press ---');
    // Both still inside with one box to go. Without this the check below
    // passes on a scene the follower left three boxes ago.
    const beforeLast = [await isBusy(rig, 0), await isBusy(rig, 1)];
    t.note('one box to go', `p1 busy=${beforeLast[0]} p2 busy=${beforeLast[1]}`);
    t.check('both are still in the scene with one box left',
            beforeLast[0] && beforeLast[1],
            `p1=${beforeLast[0]} p2=${beforeLast[1]}`);

    await rig.tap(0, 'A', 8);
    await rig.wait(120);

    const stillIn = [await isBusy(rig, 0), await isBusy(rig, 1)];
    t.note('at the end', `p1 busy=${stillIn[0]} p2 busy=${stillIn[1]}`);
    t.check('and the driver\'s last press took them both out',
            !stillIn[0] && !stillIn[1],
            `p1=${stillIn[0]} p2=${stillIn[1]}`);

    t.summary();
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
