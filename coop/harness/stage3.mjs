#!/usr/bin/env node
// Stage-3 check: scene mirroring, through a real script.
//
// Stage 2 drives the gate state directly, which proves the handshake and
// nothing above it. This one runs an actual script -- Debug_EventScript_Script_1
// in the debug menu's Scripts list -- on ONE console, and checks the other one
// plays the same scene without being touched.
//
// That is the thing a gate alone cannot do. When you step on a trigger tile,
// only your console runs the script; your partner's game never hears about it
// and never reaches the gate, so a gate on its own is a game that stops for
// ever. The console that triggers a scene has to tell the other one where the
// script is.
//
// The scene is:
//     coopscene GATE_TEST          <- mirror, then wait for both
//     lockall
//     msgbox "You're both here."
//     coopgate GATE_TEST_SECOND    <- an ordinary mid-scene gate
//     msgbox "And still together."
//     releaseall
//
// so a console that played the whole thing passes gate 1 and then gate 2. The
// gate log is what proves it: player 2 never opened a menu.
//
//   node coop/harness/stage3.mjs path/to/rom.gba
import { startRig, tally, OFFSETS } from './rig.mjs';

const PORT = 8783;
const ROM = process.argv[2] || '/home/claude/crossroads/pokeemerald.gba';

const GATE_TEST = 1;
const GATE_TEST_SECOND = 2;

// Where the map number lives inside SaveBlock1. From the compiler, because the
// obvious guess -- SaveBlock1 starts with the location -- is wrong: it starts
// with `struct Coords16 pos`, so a guessed offset writes the map number over
// the player's x coordinate and reports, correctly, that nothing happened.
const SB1_LOCATION_MAPNUM = OFFSETS.sb1Location + OFFSETS.warpMapNum;

// R + Start opens the debug menu (DEBUG_OVERWORLD_HELD_KEYS / _TRIGGER_EVENT),
// then Scripts... is the sixth entry and Script 1 the first inside it.
const SCRIPTS_MENU_INDEX = 5;

// Close anything open first. R+Start does nothing while a message box is up,
// and the previous step always leaves one: the intro is cleared by mashing A,
// which ends with the player reading the bedroom television.
async function clearBoxes(rig, which) {
  for (let i = 0; i < 6; i++) {
    await rig.tap(which, 'B', 6);
    await rig.wait(15);
  }
}

async function runDebugScript1(rig, which) {
  await clearBoxes(rig, which);
  await rig.hold(which, 'R');
  await rig.wait(6);
  await rig.tap(which, 'Start', 8);
  await rig.wait(20);
  await rig.letGo(which, 'R');
  await rig.wait(20);
  for (let i = 0; i < SCRIPTS_MENU_INDEX; i++) {
    await rig.tap(which, 'Down', 6);
    await rig.wait(8);
  }
  await rig.tap(which, 'A', 8);   // open Scripts...
  await rig.wait(25);
  await rig.tap(which, 'A', 8);   // run Script 1
  await rig.wait(25);
}

async function main() {
  const rig = await startRig({ rom: ROM, port: PORT });
  const t = tally();

  try {
    let m = await rig.mailboxes();
    console.log('\nboth cores up: ' +
      m.map((x) => `p${x.id} ${x.state} map=${x.selfMap}`).join('   '));
    if (m.some((x) => x.state !== 'ACTIVE'))
      throw new Error('the session never reached ACTIVE; nothing below is meaningful');
    if (m[0].selfMap !== m[1].selfMap)
      throw new Error('the two players are not on the same map to begin with');

    // --- the scene mirrors, and both play it ---------------------------
    //
    // This one first, from a clean state. Player 1 opens a menu; Player 2 is
    // not touched at all.
    console.log('\n--- both on the same map; player 1 triggers the scene ---');
    t.check('player 1 can see the partner here',
            m[0].flags.includes('PEER_SAME_MAP'), `flags=${m[0].flags}`);

    await rig.clearGateLog();
    await runDebugScript1(rig, 0);

    // Both should now be at gate 1 and then through it. The msgbox after it
    // waits for a button, so tap A on both to let the scene continue into the
    // second gate.
    await rig.wait(60);
    const mid = await rig.mailboxes();
    t.note('mid-scene', mid.map((x) => `p${x.id} gate=${x.gateId}`).join('  '));

    for (let i = 0; i < 6; i++) { await rig.tap('both', 'A', 8); await rig.wait(30); }
    await rig.wait(90);

    const log0 = await rig.gateLog(0);
    const log1 = await rig.gateLog(1);
    t.note('gates p1', JSON.stringify(log0));
    t.note('gates p2', JSON.stringify(log1));

    t.check('player 1 played the scene', log0.includes(GATE_TEST));
    t.check('player 2 played the same scene, untouched', log1.includes(GATE_TEST),
            log1.length ? '' : 'player 2 never saw it');
    t.check('player 1 reached the mid-scene gate', log0.includes(GATE_TEST_SECOND));
    t.check('player 2 reached the mid-scene gate', log1.includes(GATE_TEST_SECOND));
    const done = await rig.mailboxes();
    t.note('after', done.map((x) => `p${x.id} gate=${x.gateId} flags=${x.flags}`).join('  '));
    t.check('neither is left waiting at a gate',
            done.every((x) => x.gateId === 0));
    await rig.shot('/tmp/claude-0/stage3-played');

    // --- and it must NOT run when the partner is elsewhere -------------
    //
    // Player 2's map number is changed underneath it so its position
    // broadcasts say "somewhere else". Crude, and it leaves Player 2's own
    // game confused, which is why it comes last -- but it is the only way to
    // stage "my partner is on another map" without an hour of walking.
    console.log('\n--- player 2 is elsewhere; player 1 triggers the scene ---');
    const realMapNum = await rig.u8(1, OFFSETS.saveBlock1Addr + SB1_LOCATION_MAPNUM);
    await rig.setU8(1, OFFSETS.saveBlock1Addr + SB1_LOCATION_MAPNUM, realMapNum + 1);
    await rig.wait(90);

    m = await rig.mailboxes();
    t.note('maps', m.map((x) => `p${x.id} self=${x.selfMap} peer=${x.peerMap}`).join('  '));
    t.check('player 1 now sees the partner on another map',
            !m[0].flags.includes('PEER_SAME_MAP'), `flags=${m[0].flags}`);

    await rig.clearGateLog();
    await runDebugScript1(rig, 0);
    await rig.wait(90);

    const refused0 = await rig.gateLog(0);
    const refused1 = await rig.gateLog(1);
    t.note('gates p1', JSON.stringify(refused0));
    t.note('gates p2', JSON.stringify(refused1));
    t.check('the scene did not start', refused0.length === 0 && refused1.length === 0,
            refused0.length ? 'player 1 entered it alone' : '');
    t.check('player 1 is not stuck waiting',
            (await rig.mailbox(0)).gateId === 0);
    await rig.shot('/tmp/claude-0/stage3-refused');
    const ok = t.summary();
    process.exitCode = ok ? 0 : 1;
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
