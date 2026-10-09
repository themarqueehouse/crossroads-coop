#!/usr/bin/env node
// Stage-9 check: the game does not start for one player on their own.
//
// Every other check in this suite begins with both consoles already paired,
// because that is the interesting case. This is the one that is not: a player
// who opens the hack while their partner is still asleep. Left alone they
// would start the adventure, walk past story beats the other player is
// supposed to be standing next to, and advance a save meant to be shared.
//
// Three things to establish, and the third is the one that makes this worth
// having rather than a nuisance:
//   1. the game visibly boots and reaches the menu -- a ROM that shows nothing
//      until a partner appears is indistinguishable from a broken one
//   2. choosing to start holds, with the reason on screen
//   3. it starts BY ITSELF the moment the partner arrives, with no second
//      press needed
//   4. Player 2 never touches the menu at all -- no save file, no intro, no
//      naming screen. Their character comes out of Player 1's save, so there
//      is nothing on that menu for them to choose between, and making them
//      sit through Birch every session for a character that gets replaced on
//      connect is the thing this exists to avoid.
//
//   node coop/harness/stage9.mjs path/to/rom.gba
import { startRig, tally, OFFSETS } from './rig.mjs';

const PORT = 8799;
const ROM = process.argv[2] || '/home/claude/crossroads/pokeemerald.gba';

// The same nudge pattern startRig uses to get through the copyright screen,
// the title and Birch's speech.
const NUDGE = [null, 'A', 'A', 'A', 'Start', null, 'A', 'Start'];

const mapOf = async (rig, w) => parseInt((await rig.mailbox(w)).selfMap, 16);

// Whether the overworld is running at all. coop.c publishes selfMap from
// OverworldBasic and nowhere else, so it stays zero for as long as the game
// has not handed control to the field -- which is exactly "the adventure has
// not started".
const inGame = async (rig, w) => (await mapOf(rig, w)) !== 0;

async function nudge(rig, loops) {
  for (let i = 0; i < loops; i++) {
    await rig.wait(12);
    const key = NUDGE[i % NUDGE.length];
    if (key) await rig.tap('both', key, 6);
  }
}

async function main() {
  // Unpaired: the relay is up, but the mailbox says the other player has not
  // joined. introLoops 0 so nothing mashes past the menu before we look.
  const rig = await startRig({ rom: ROM, port: PORT, paired: false,
                               introLoops: 0, settle: 0 });
  const t = tally();

  try {
    console.log('\n--- one player, booting alone ---');
    // Through the copyright screen and the title, as far as the menu.
    await nudge(rig, 60);
    await rig.wait(120);
    await rig.shot('/tmp/claude-0/stage9-menu');

    t.check('the game boots and gets as far as the menu',
            !(await inGame(rig, 0)),
            'it started the adventure without waiting for anyone');

    console.log('\n--- and tries to start ---');
    // Press A on the highlighted item, then keep pressing: a gate that holds
    // only until the player tries again is not a gate.
    for (let i = 0; i < 14; i++) {
      await rig.tap(0, 'A', 8);
      await rig.wait(40);
    }
    await rig.wait(240);
    await rig.shot('/tmp/claude-0/stage9-waiting');

    t.check('the adventure did not start', !(await inGame(rig, 0)),
            'one player got into the world on their own');

    // --------------------------------------------------------- partner joins
    console.log('\n--- the partner arrives ---');
    await rig.pairUp();
    await rig.wait(180);
    await rig.shot('/tmp/claude-0/stage9-paired');

    // No further press on the menu: whatever happens now has to happen because
    // the partner turned up, not because the test prodded it. Past that first
    // moment the nudges are for Birch's speech, which is a long way from the
    // menu and not what is under test.
    // What is holding player 2 at the menu, if anything is.
    for (let i = 0; i < 6; i++) {
      await rig.wait(90);
      const pf = await rig.readAt(1, OFFSETS.paletteFadeAddr, 8);
      const mb = await rig.mailbox(1);
      console.log(`      p2 t+${(i + 1) * 90}: state=${mb.state} map=${mb.selfMap}` +
        `  entry=${await rig.u8(1, OFFSETS.dbgJoinEntryAddr)}` +
        `  paletteFade=[${[...pf].map((b) => b.toString(16)).join(' ')}]`);
    }

    await nudge(rig, 320);
    await rig.wait(240);
    for (let i = 0; i < 8; i++) { await rig.tap('both', 'B', 6); await rig.wait(20); }
    await rig.wait(600);
    await rig.shot('/tmp/claude-0/stage9-started');

    t.note('maps', `p1=0x${(await mapOf(rig, 0)).toString(16)} ` +
                   `p2=0x${(await mapOf(rig, 1)).toString(16)}`);
    t.check('the game started once the partner was there',
            (await inGame(rig, 0)) && (await inGame(rig, 1)),
            'still held at the menu after pairing');

    const m = await rig.mailboxes();
    t.note('session', m.map((x) => `p${x.id} ${x.state}`).join('  '));
    t.check('and the co-op session came up',
            m.every((x) => x.state === 'ACTIVE'),
            m.map((x) => x.state).join('/'));

    // Player 2 got into the world without a save of their own and without
    // being asked anything. Both consoles being in the overworld on the same
    // map is the proof: core 1 was given no save file at all.
    t.check('player 2 got in with no save and no intro',
            (await mapOf(rig, 1)) !== 0,
            'player 2 never reached the overworld');

    t.summary();
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
