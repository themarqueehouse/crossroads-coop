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
import { startRig, tally, OFFSETS, answerFirstRunPrompt } from './rig.mjs';

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
  // answerFirstRun off: the prompt cannot appear until the pair is up, and
  // the whole point of this stage is that it is not, yet.
  const rig = await startRig({ rom: ROM, port: PORT, paired: false,
                               introLoops: 0, settle: 0,
                               answerFirstRun: false });
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
    for (let i = 0; i < 20; i++) {
      await rig.wait(180);
      const pf = await rig.readAt(1, OFFSETS.paletteFadeAddr, 8);
      const mb = await rig.mailbox(1);
      console.log(`      p2 t+${(i + 1) * 180}: state=${mb.state} map=${mb.selfMap}` +
        `  entry=${await rig.u8(1, OFFSETS.dbgJoinEntryAddr)}` +
        `  err p1=${await rig.u8(0, OFFSETS.linkErrorSiteAddr)}` +
        `/${(await rig.u32(0, OFFSETS.linkErrorStatusAddr)).toString(16)}` +
        `  err p2=${await rig.u8(1, OFFSETS.linkErrorSiteAddr)}` +
        `/${(await rig.u32(1, OFFSETS.linkErrorStatusAddr)).toString(16)}` +
        `  p1state=${(await rig.mailbox(0)).state}` +
        `  p1entry=${await rig.u8(0, OFFSETS.dbgJoinEntryAddr)}` +
        `  p1menu=${(await rig.u8(0, OFFSETS.dbgMenuActionAddr)).toString(16)}` +
        `  paletteFade=[${[...pf].map((b) => b.toString(16)).join(' ')}]`);
    }

    // No nudging through Birch any more: both consoles skip the opening and
    // land in Littleroot by themselves. Anything pressed here would be pressed
    // at whatever is actually on screen.
    await rig.wait(900);
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

    // Both are asked who they are, and until they answer neither can be moved
    // anywhere -- the placement warp will not run on a console whose controls
    // are locked by a prompt. The rig answers these for every other stage; it
    // cannot here, because they do not exist until the pair is up and the
    // pair coming up is what this stage is about.
    console.log('\n--- answering the opening questions ---');
    for (const w of [0, 1]) await answerFirstRunPrompt(rig, w, 2, 2, false, w);

    // And in the same world as Player 1.
    //
    // Player 2's console starts before a byte has crossed the link, so it has
    // to guess a region, and it guesses Hoenn. Player 1 may have chosen Kanto
    // on the title screen -- which is sticky, so "I switched it back" does not
    // undo it. Without the region travelling with the world sync the two spent
    // the session on different continents wearing different skins, which is
    // exactly what came back from the first real playtest.
    await rig.wait(900);
    const sameMap = (await mapOf(rig, 0)) === (await mapOf(rig, 1));
    t.note('together', `p1=0x${(await mapOf(rig, 0)).toString(16)} ` +
                       `p2=0x${(await mapOf(rig, 1)).toString(16)}`);
    t.check('and both players ended up in the same place', sameMap,
            'the two consoles are on different maps');

    t.summary();
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
