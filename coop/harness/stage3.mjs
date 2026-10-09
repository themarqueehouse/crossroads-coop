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
const CONVERSATION_SCRIPT = 16;   // debug.inc: locks the player and waits

// Whether this console is out of the player's hands: a script running, a
// message waiting to be dismissed, a menu open.
const isBusy = async (rig, which) =>
  (await rig.mailbox(which)).flags.includes('SCRIPT_BUSY');

// Hand control back to the player, and know it rather than hoping.
//
// An earlier version counted B presses and moved on. It also asked the ROM
// whether a message box was open -- and got "no" while one sat on screen,
// because the game's own flag means "text is printing", not "a box is up". So
// every section after the first ran its menu presses into a message box and
// the failures landed three sections later, nowhere near the cause.
async function freeUp(rig, which) {
  for (let i = 0; i < 12 && await isBusy(rig, which); i++) {
    await rig.tap(which, 'B', 6);
    await rig.wait(20);
  }
  if (await isBusy(rig, which))
    throw new Error(`core ${which}: would not come back to the player`);
}

// Get a console into a conversation, for staging "your partner is busy".
//
// Debug script 16, rather than walking up to something and pressing A. That
// worked while the opening left both players standing in a bedroom next to a
// television; now that they skip the opening and start outside, there is
// nothing in reach to talk to and eight hopeful A presses found nothing.
async function startConversation(rig, which) {
  await runDebugScript(rig, which, CONVERSATION_SCRIPT);
  for (let i = 0; i < 20 && !(await isBusy(rig, which)); i++)
    await rig.wait(20);
  if (!(await isBusy(rig, which)))
    throw new Error(`core ${which}: could not get into a conversation`);
}

// `beforeRun` fires with the Scripts submenu open, which is the last moment
// before the script's first command -- and the only one at which state the
// script is about to read can be staged, since the field clears some of it
// every frame the player is in control.
async function runDebugScript(rig, which, slot, beforeRun) {
  await freeUp(rig, which);
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
  for (let i = 1; i < slot; i++) {
    await rig.tap(which, 'Down', 6);
    await rig.wait(8);
  }
  if (beforeRun) await beforeRun();
  await rig.tap(which, 'A', 8);   // run Script <slot>
  await rig.wait(25);
}

const runDebugScript1 = (rig, which) => runDebugScript(rig, which, 1);

// How many of `itemId` are in a console's bag, decrypted under its own key.
//
// Scanned rather than read from slot 0: the game starts you with items, and
// which slot a new one lands in is the bag's business.
const ITEM_POTION = 28;
const BAG_SCAN_SLOTS = 60;

async function countItem(rig, which, itemId) {
  const key = await rig.u32(which, OFFSETS.saveBlock2Addr + OFFSETS.encryptionKey);
  const raw = await rig.readAt(which, OFFSETS.saveBlock1Addr + OFFSETS.bag,
                               BAG_SCAN_SLOTS * 4);
  let total = 0;
  for (let i = 0; i < BAG_SCAN_SLOTS; i++) {
    const id = raw[i * 4] | (raw[i * 4 + 1] << 8);
    if (id !== itemId) continue;
    total += ((raw[i * 4 + 2] | (raw[i * 4 + 3] << 8)) ^ (key & 0xffff)) & 0xffff;
  }
  return total;
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

    // --- a gift in a mirrored scene happens once, not twice ------------
    //
    // Script 2 adds three Potions behind goto_if_coop_guest. The bag is
    // shared, so the right answer is three between them. Each of the three
    // possible wrong answers is distinguishable:
    //
    //   3 / 3  correct
    //   3 / 0  the bag change never crossed -- the shared bag is sync-at-join
    //          only, which is how it was before this
    //   6 / 6  the guard did nothing and both consoles handed over a gift
    console.log('\n--- a gift inside a mirrored scene ---');
    await freeUp(rig, 0);
    await freeUp(rig, 1);
    const before = [await countItem(rig, 0, ITEM_POTION),
                    await countItem(rig, 1, ITEM_POTION)];
    t.note('potions before', before.join(' / '));

    await runDebugScript(rig, 0, 2);
    await rig.wait(120);
    for (let i = 0; i < 4; i++) { await rig.tap('both', 'A', 8); await rig.wait(25); }
    await rig.wait(120);

    const gained = [await countItem(rig, 0, ITEM_POTION) - before[0],
                    await countItem(rig, 1, ITEM_POTION) - before[1]];
    t.note('potions gained', gained.join(' / '));
    t.check('the trigger got the gift', gained[0] === 3,
            gained[0] === 6 ? 'both consoles handed one over' : '');
    t.check('the partner got it too, and only once', gained[1] === 3,
            gained[1] === 0 ? 'the shared bag did not cross' : '');

    // --- the mirrored scene knows who was being talked to --------------
    //
    // Most story scenes open with `lock` and `faceplayer`, and both act on
    // whatever THIS console last talked to. On the console handed the scene
    // that is some unrelated NPC it spoke to earlier, so the trigger has to
    // say who it was.
    //
    // Staged by setting the two consoles' idea of it to different values and
    // checking the guest ends up with the host's.
    //
    // It has to be staged with the debug menu already open. The field clears
    // gSpecialVar_LastTalked at the top of every frame the player is in
    // control -- which is correct, it only means anything while a script
    // started by talking to someone is running -- so a value written any
    // earlier is gone before the script's first command reads it.
    console.log('\n--- who the scene was talking to ---');
    await freeUp(rig, 0);
    await freeUp(rig, 1);
    const HOST_SPEAKER = 9;
    await rig.setU16(1, OFFSETS.lastTalkedAddr, 3);

    await rig.clearGateLog();
    await runDebugScript(rig, 0, 1, async () => {
      await rig.setU16(0, OFFSETS.lastTalkedAddr, HOST_SPEAKER);
      t.note('last talked', `${await rig.u16(0, OFFSETS.lastTalkedAddr)} / ` +
                            `${await rig.u16(1, OFFSETS.lastTalkedAddr)}`);
    });
    await rig.wait(120);

    const guestSpeaker = await rig.u16(1, OFFSETS.lastTalkedAddr);
    t.note('last talked after', `${await rig.u16(0, OFFSETS.lastTalkedAddr)} / ${guestSpeaker}`);
    t.check('the guest adopts the host\'s speaker', guestSpeaker === HOST_SPEAKER,
            guestSpeaker === 3 ? 'it kept its own' : '');

    for (let i = 0; i < 6; i++) { await rig.tap('both', 'A', 8); await rig.wait(25); }
    await rig.wait(60);

    // --- a partner who is busy, not absent -----------------------------
    //
    // Player 2 is left in a conversation with the bedroom television. The
    // scene cannot start underneath that, so Player 1 should sit at the gate
    // saying so -- and the scene should still be waiting its turn on Player 2,
    // not thrown away. Then the moment Player 2's box closes, it runs.
    //
    // This is also the only check that gets "Waiting for your partner" on
    // screen: when both players are ready the gate opens in a frame or two and
    // the message never appears. The screenshot is the record of it.
    console.log('\n--- player 2 is mid-conversation when the scene starts ---');
    await freeUp(rig, 0);
    await freeUp(rig, 1);
    await startConversation(rig, 1);
    t.check('player 2 is in a conversation', await isBusy(rig, 1));

    await rig.clearGateLog();
    await runDebugScript(rig, 0, 1);
    await rig.wait(120);

    // Screenshot first. Everything in this window burns frames against the
    // deferred scene's timeout, and a screenshot burns far more of them than
    // a memory read does -- taking it after the checks spent enough of the
    // budget to drop the scene on about one run in five, which read as the
    // ROM failing to defer it.
    await rig.shot('/tmp/claude-0/stage3-waiting');
    const waiting = await rig.mailbox(0);
    t.note('player 1', `gate=${waiting.gateId} flags=${waiting.flags}`);
    t.check('player 1 is held at the gate', waiting.gateId === GATE_TEST &&
            waiting.flags.includes('AT_GATE'));
    t.check('player 2 has not started it yet',
            (await rig.gateLog(1)).length === 0);

    // Let player 2 out of the conversation. B rather than A: A would just
    // read the television again.
    //
    // Not freeUp, which waits for control to come back: it never does, because
    // the moment the conversation ends the deferred scene starts and takes it
    // again. That is the thing being checked, so waiting for the opposite
    // would fail on success.
    for (let i = 0; i < 4; i++) { await rig.tap(1, 'B', 6); await rig.wait(20); }
    await rig.wait(120);
    const freed = (await rig.gateLog(1)).includes(GATE_TEST);
    // Say which of the two it was. A scene that never arrived and a scene
    // that arrived and could not start look identical in the gate log, and
    // the difference is the difference between a transport bug and a
    // timeout -- worth not having to reproduce it twice to find out.
    const pend = await rig.u32(1, OFFSETS.pendingSceneAddr);
    const waited = await rig.u16(1, OFFSETS.pendingSceneFramesAddr);
    t.check('the scene starts as soon as player 2 is free', freed,
            freed ? ''
                  : `the deferred scene was dropped (pending=0x${pend.toString(16)} ` +
                    `waited=${waited} frames, p2 busy=${await isBusy(rig, 1)})`);

    for (let i = 0; i < 6; i++) { await rig.tap('both', 'A', 8); await rig.wait(25); }
    await rig.wait(90);
    t.check('and both get through it',
            (await rig.gateLog(0)).includes(GATE_TEST_SECOND) &&
            (await rig.gateLog(1)).includes(GATE_TEST_SECOND));

    // --- and it must NOT run when the partner is elsewhere -------------
    //
    // Player 2's map number is changed underneath it so its position
    // broadcasts say "somewhere else". Crude, and it leaves Player 2's own
    // game confused, which is why it comes last -- but it is the only way to
    // stage "my partner is on another map" without an hour of walking.
    console.log('\n--- player 2 is elsewhere; player 1 triggers the scene ---');
    await freeUp(rig, 0);
    await freeUp(rig, 1);
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

    // --- the gym shape: refused when alone, allowed when not -----------
    //
    // Script 3 is coop_require_partner followed by a Potion. Not mirrored --
    // which is the point of it being its own thing. Mirroring a gym leader
    // would start two separate battles against two copies of the leader, and
    // whoever won first would set the badge out from under the other's fight.
    //
    // Player 2 is still displaced from the check above, so the refusal case
    // comes first and for free.
    console.log('\n--- a gym-shaped script, with the partner away ---');
    await freeUp(rig, 0);
    const beforeAway = await countItem(rig, 0, ITEM_POTION);
    await runDebugScript(rig, 0, 3);
    await rig.wait(120);
    t.check('refused while alone',
            (await countItem(rig, 0, ITEM_POTION)) === beforeAway);

    console.log('\n--- and with the partner here ---');
    await freeUp(rig, 0);
    await rig.setU8(1, OFFSETS.saveBlock1Addr + SB1_LOCATION_MAPNUM, realMapNum);
    await rig.wait(120);
    t.check('player 1 sees the partner again',
            (await rig.mailbox(0)).flags.includes('PEER_SAME_MAP'));

    const beforeTogether = [await countItem(rig, 0, ITEM_POTION),
                            await countItem(rig, 1, ITEM_POTION)];
    await runDebugScript(rig, 0, 3);
    await rig.wait(150);
    const after3 = [await countItem(rig, 0, ITEM_POTION) - beforeTogether[0],
                    await countItem(rig, 1, ITEM_POTION) - beforeTogether[1]];
    t.note('potions gained', after3.join(' / '));
    t.check('allowed with the partner here, once each side', after3[0] === 1 && after3[1] === 1);
    const ok = t.summary();
    process.exitCode = ok ? 0 : 1;
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
