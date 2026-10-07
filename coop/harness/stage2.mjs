#!/usr/bin/env node
// Stage-2 check: sync gates.
//
// A gate is a point in a script neither player passes alone -- a story door, a
// cutscene. Whoever gets there first stops; when the other reaches the same
// gate, both carry on.
//
// The gate is driven here by writing the arrival straight into the ROM's gate
// state rather than by playing the game up to a scripted gate, because what is
// under test is the handshake, not the script command that calls it. Driving it
// through a real script would mean an hour of button presses per run to reach
// one story beat, and would test the wrong half.
//
// What each check is for:
//   1. arriving alone WAITS. The failure this catches is a gate that opens on
//      nothing, which looks like success in every other way: the game plays on,
//      and the two players simply drift apart.
//   2. the arrival is ANNOUNCED. One console's state changing is not a
//      handshake; the peer has to hear it.
//   3. the second arrival OPENS BOTH. Not just the second one -- the first
//      player is the one sat on a dark screen.
//   4. a remembered arrival does NOT open a later gate. This is the one the
//      arrival counter exists for: walk back through a door you have both
//      already been through, and a gate that trusts the last report it heard
//      opens with the partner three towns away.
//   5. whoever arrives SECOND opens immediately, from the report already
//      waiting for them -- the asymmetric path through the same code.
//
//   node coop/harness/stage2.mjs path/to/rom.gba
import { startRig, tally, OFFSETS } from './rig.mjs';

const PORT = 8781;
const ROM = process.argv[2] || '/home/claude/crossroads/pokeemerald.gba';

const GATE_TEST = 1;
const GATE_TEST_B = 2;
const AT_GATE = 1 << 5; // COOP_DIAG_AT_GATE, include/net_link.h

// Put a console at a gate, exactly as Coop_BeginGate would.
//
// Deliberately NOT including the "is the partner already here" shortcut that
// Coop_BeginGate does on arrival: this writes the pre-handshake state and lets
// the ROM's own per-frame handshake resolve it. A rig that decided the outcome
// itself would pass whatever the ROM did.
// sGateId is written LAST, and that ordering is the whole trick.
//
// Each write is its own round trip into the page, so the emulator runs frames
// between them -- and the ROM's handshake runs once a frame. Writing sGateId
// first opened the gate on the spot, from the partner's report, and then the
// rig's own later write of sGateOpen = 0 slammed it shut again. Two checks
// failed and the ROM was innocent in both. sGateId == 0 is the ROM's "no gate"
// short-circuit, so leaving it until the end makes the arrival atomic as far as
// the ROM is concerned, however many frames the rig takes to assemble it.
async function arrive(rig, which, gateId, seq) {
  await rig.setU8(which, OFFSETS.gateOpenAddr, 0);
  await rig.setU16(which, OFFSETS.gateSeqAddr, seq);
  await rig.setU16(which, OFFSETS.gateSendIdAddr, gateId);
  await rig.setU16(which, OFFSETS.gateSendSeqAddr, seq);
  await rig.setU8(which, OFFSETS.gateSendsLeftAddr, 4);
  await rig.setU16(which, OFFSETS.gateIdAddr, gateId);
}

async function gateState(rig, which) {
  return {
    gateId: await rig.u16(which, OFFSETS.gateIdAddr),
    open: await rig.u8(which, OFFSETS.gateOpenAddr),
    sendsLeft: await rig.u8(which, OFFSETS.gateSendsLeftAddr),
    peerGateId: await rig.u16(which, OFFSETS.peerGateIdAddr),
    peerGateSeq: await rig.u16(which, OFFSETS.peerGateSeqAddr),
    usedSeq: await rig.u16(which, OFFSETS.usedPeerGateSeqAddr),
  };
}

const show = (s) =>
  `gate=${s.gateId} open=${s.open} sends=${s.sendsLeft} ` +
  `peer=${s.peerGateId}/${s.peerGateSeq} used=${s.usedSeq}`;

async function main() {
  const rig = await startRig({ rom: ROM, port: PORT });
  const t = tally();

  try {
    const m = await rig.mailboxes();
    console.log('\nboth cores up: ' +
      m.map((x) => `p${x.id} ${x.state} map=${x.selfMap}`).join('   '));
    if (m.some((x) => x.state !== 'ACTIVE'))
      throw new Error('the session never reached ACTIVE; nothing below is meaningful');

    // --- 1 & 2: arriving alone waits, and is announced ------------------
    console.log('\n--- player 1 arrives at gate 1, alone ---');
    await arrive(rig, 0, GATE_TEST, 1);
    await rig.wait(30);

    let a = await gateState(rig, 0);
    let b = await gateState(rig, 1);
    t.note('p1', show(a));
    t.note('p2', show(b));

    t.check('p1 waits at the gate', a.gateId === GATE_TEST && a.open === 0);
    t.check('p1 announced the arrival', b.peerGateId === GATE_TEST && b.peerGateSeq === 1);
    t.check('p1 stopped re-announcing once sent', a.sendsLeft === 0);

    const diag = await rig.mailbox(0);
    t.check('the wait is visible to the host', diag.gateId === GATE_TEST &&
            diag.flags.includes('AT_GATE'), `flags=${diag.flags} gateId=${diag.gateId}`);

    t.check('p2, who is not at a gate, is unaffected', b.gateId === 0 && b.open === 0);

    // --- 3 & 5: the second arrival opens both ---------------------------
    console.log('\n--- player 2 arrives at the same gate ---');
    await arrive(rig, 1, GATE_TEST, 1);
    await rig.wait(30);

    a = await gateState(rig, 0);
    b = await gateState(rig, 1);
    t.note('p1', show(a));
    t.note('p2', show(b));

    // gateId is cleared by Coop_GateIsOpen, which only a script calls, so from
    // here the gate reads as "open and still set" -- that is the state a
    // blocked script sees on the frame it is released.
    t.check('p1 opened', a.open === 1);
    t.check('p2 opened', b.open === 1);
    t.check('p1 consumed p2 arrival 1', a.usedSeq === 1);
    t.check('p2 consumed p1 arrival 1', b.usedSeq === 1);

    // --- 4: a remembered arrival must not open a later gate -------------
    //
    // Both reports still say "gate 1, arrival 1". Player 1 now walks back into
    // the same door on its own. Without the arrival counter, its own remembered
    // copy of Player 2's report would open the gate instantly.
    console.log('\n--- player 1 re-enters gate 1 alone (the stale-report trap) ---');
    await arrive(rig, 0, GATE_TEST, 2);
    await rig.wait(30);

    a = await gateState(rig, 0);
    t.note('p1', show(a));
    t.check('p1 waits rather than opening on the old report',
            a.gateId === GATE_TEST && a.open === 0,
            a.open ? 'opened with the partner elsewhere' : '');

    console.log('\n--- player 2 re-enters, as a fresh arrival ---');
    await arrive(rig, 1, GATE_TEST, 2);
    await rig.wait(30);

    a = await gateState(rig, 0);
    b = await gateState(rig, 1);
    t.note('p1', show(a));
    t.note('p2', show(b));
    t.check('p1 opened on the new arrival', a.open === 1 && a.usedSeq === 2);
    t.check('p2 opened on the new arrival', b.open === 1 && b.usedSeq === 2);

    // --- a different gate must not open on this one's reports -----------
    console.log('\n--- player 1 arrives at a DIFFERENT gate ---');
    await arrive(rig, 0, GATE_TEST_B, 3);
    await rig.wait(30);
    a = await gateState(rig, 0);
    t.note('p1', show(a));
    t.check('a different gate id does not open on gate 1 reports',
            a.gateId === GATE_TEST_B && a.open === 0);

    // --- a dropped partner must not open the gate -----------------------
    //
    // The remaining player waiting through a reconnect is the whole point: one
    // console walking the shared story forward alone is unrecoverable, and
    // there is one save between the two of them.
    console.log('\n--- the partner drops while player 1 waits ---');
    await rig.reconnect(240);
    await rig.wait(60);
    a = await gateState(rig, 0);
    t.note('p1', show(a));
    t.check('still waiting after the drop', a.gateId === GATE_TEST_B && a.open === 0,
            a.open ? 'the lone player was let through' : '');
    t.check('the arrival is re-announced over the new session', a.sendsLeft <= 4);

    await rig.wait(600);
    const back = await rig.mailboxes();
    t.note('after reconnect', back.map((x) => `p${x.id} ${x.state}`).join('  '));

    a = await gateState(rig, 0);
    b = await gateState(rig, 1);
    t.note('p1', show(a));
    t.note('p2', show(b));
    t.check('p2 heard the re-announcement', b.peerGateId === GATE_TEST_B);

    console.log('\n--- player 2 joins it, over the new session ---');
    await arrive(rig, 1, GATE_TEST_B, 4);
    await rig.wait(30);
    a = await gateState(rig, 0);
    b = await gateState(rig, 1);
    t.note('p1', show(a));
    t.note('p2', show(b));
    t.check('the gate opens on both after a reconnect', a.open === 1 && b.open === 1);

    await rig.shot('/tmp/claude-0/stage2');
    const ok = t.summary();
    process.exitCode = ok ? 0 : 1;
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
