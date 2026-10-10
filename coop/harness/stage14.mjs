#!/usr/bin/env node
// Stage-14 check: a fresh co-op game asks each player who they are and what
// they want to start with.
//
// Skipping the opening skipped the only place the game ever asks. Two players
// landed in a town nameless, genderless and with nothing to battle with. This
// is the replacement, and the things that matter about it are:
//
//   1. it happens at all, on both consoles, without anyone pressing anything
//      to summon it
//   2. each player answers for themselves -- it is NOT a mirrored scene, so
//      one player's menu must not move when the other presses
//   3. neither is handed a Pokemon -- the starter belongs to Birch's scene on
//      Route 101, and giving one here is what made him ask a second time
//   4. the gender each of them picked is recorded, and the partner is told
//   5. it does not come back once answered
//
//   node coop/harness/stage14.mjs path/to/rom.gba
import { startRig, tally, OFFSETS, answerFirstRunPrompt } from './rig.mjs';

const PORT = 8802;
const ROM = process.argv[2] || '/home/claude/crossroads/pokeemerald.gba';

const isBusy = async (rig, w) =>
  (await rig.mailbox(w)).flags.includes('SCRIPT_BUSY');

// Party count, straight out of the save block.
const partyCount = (rig, w) => rig.u8(w, OFFSETS.partiesCountAddr);

const scriptAt = (rig, w) => rig.u32(w, OFFSETS.dbgScriptPtrAddr);

const answer = (rig, w, { girl = false, letter = 0 } = {}) =>
  answerFirstRunPrompt(rig, w, girl, letter);

const MALE = 0, FEMALE = 1;
const myGender = (rig, w) =>
  rig.u8(w, OFFSETS.saveBlock2Addr + OFFSETS.sb2PlayerGender);
const peerGender = (rig, w) =>
  rig.u8(w, OFFSETS.coopPeerAddr + OFFSETS.peerGender);

const inPrompt = async (rig, w) =>
  (await rig.u8(w, OFFSETS.firstRunRunningAddr)) !== 0;

async function main() {
  // A brand-new game on both consoles, which is the only state this runs in.
  // answerFirstRun off: the rig answers these prompts for every other stage,
  // and this one is about the prompts.
  const rig = await startRig({ rom: ROM, port: PORT, answerFirstRun: false });
  const t = tally();

  try {
    const m = await rig.mailboxes();
    console.log('\nboth cores up: ' +
      m.map((x) => `p${x.id} ${x.state} map=${x.selfMap}`).join('   '));
    if (m.some((x) => x.state !== 'ACTIVE'))
      throw new Error('the session never reached ACTIVE; nothing below is meaningful');

    // --- 1: it asks, unprompted -----------------------------------------
    //
    // startRig has already mashed its way through the intro and settled, so
    // if the prompt were going to appear by itself it has had every chance.
    t.note('party at the start',
           `p1=${await partyCount(rig, 0)} p2=${await partyCount(rig, 1)}`);

    t.check('both consoles are asking something',
            (await isBusy(rig, 0)) && (await isBusy(rig, 1)),
            `p1 busy=${await isBusy(rig, 0)} p2 busy=${await isBusy(rig, 1)}`);
    await rig.shot('/tmp/claude-0/stage14-asking');

    // --- 2: each answers for themselves ---------------------------------
    //
    // Player 1 presses through the whole thing while player 2 is not touched.
    // If this were a mirrored scene, player 2's console would follow along
    // and come out the other side with a Pokemon it never chose.
    console.log('\n--- player 1 answers; player 2 is not touched ---');
    const p2Before = await scriptAt(rig, 1);
    await answer(rig, 0, { letter: 0 });

    t.note('after p1 answered',
           `p1 party=${await partyCount(rig, 0)} busy=${await isBusy(rig, 0)}  ` +
           `p2 party=${await partyCount(rig, 1)} busy=${await isBusy(rig, 1)}`);

    t.check('player 1 is back in the world', !(await isBusy(rig, 0)),
            'player 1 is still stuck in the prompts');

    // No Pokemon from this prompt, deliberately.
    //
    // It used to hand one over, and that is what made Birch ask again at his
    // bag on Route 101 -- he still had a starter to give and no idea anybody
    // had already been given one. The choice lives in that scene now, so
    // coming out of these questions with a Pokemon is the regression.
    t.check('and was not handed a Pokemon', (await partyCount(rig, 0)) === 0,
            `party of ${await partyCount(rig, 0)} before Birch has given anybody one`);

    t.check('player 2 was not dragged through it',
            (await partyCount(rig, 1)) === 0,
            'player 2 came out of a prompt it never answered');
    t.check('player 2 is still being asked', await inPrompt(rig, 1),
            'player 2\'s prompt vanished along with player 1\'s');

    // --- 3 & 4: player 2 answers, picking the same one ------------------
    console.log('\n--- player 2 answers, as a girl ---');
    await answer(rig, 1, { girl: true, letter: 1 });
    await rig.wait(120);
    await rig.shot('/tmp/claude-0/stage14-done');

    t.check('both are back in the world',
            !(await isBusy(rig, 0)) && !(await isBusy(rig, 1)),
            `p1=${await isBusy(rig, 0)} p2=${await isBusy(rig, 1)}`);

    // --- girl ------------------------------------------------------------
    //
    // Straight from the playtest: "player 2 also chose girl and it didn't
    // show for either player, both showed as boys". Two separate claims --
    // what player 2's console recorded, and what player 1 was told -- so two
    // checks.
    t.note('genders', `p1 self=${await myGender(rig, 0)} p2 self=${await myGender(rig, 1)}  ` +
                      `p1 sees peer=${await peerGender(rig, 0)}`);

    t.check('player 2 is recorded as a girl', (await myGender(rig, 1)) === FEMALE,
            `player 2's own console has gender ${await myGender(rig, 1)}`);
    t.check('player 1 is still a boy', (await myGender(rig, 0)) === MALE,
            'player 2 choosing changed player 1');
    t.check('and player 1 sees her as one', (await peerGender(rig, 0)) === FEMALE,
            `player 1 is drawing the partner as gender ${await peerGender(rig, 0)}`);

    // --- 5: and it is over ----------------------------------------------
    console.log('\n--- and it does not come back ---');
    await rig.wait(600);
    t.check('neither console is asked again',
            !(await inPrompt(rig, 0)) && !(await inPrompt(rig, 1)),
            `p1=${await inPrompt(rig, 0)} p2=${await inPrompt(rig, 1)}`);

    t.summary();
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
