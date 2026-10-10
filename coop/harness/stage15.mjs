#!/usr/bin/env node
// Stage-15 check: the two players can battle each other.
//
// "How do you battle each other?" was a question with no answer: there was
// nothing in the world to press A at that did anything, and pressing A at the
// partner was in fact a crash waiting to happen -- they are a spawned object
// with no map template, and the interaction path looked a script up through
// that template.
//
// Four things to establish, and the last two are the ones worth the run:
//   1. the offer is mirrored -- one player presses A, BOTH consoles ask
//   2. it takes two yeses. One player's agreement must not drag the other
//      into a battle they declined
//   3. a decline RELEASES the player who agreed, rather than leaving them sat
//      at a gate for ever. This is what the give-up gate is for, and a gate
//      that waits for ever looks identical for the first thirty seconds
//   4. two yeses start a real two-player link battle -- and when it is over,
//      both parties come back exactly as they went in. A fight between
//      friends costs nothing: nobody faints for real, nobody loses money
//
//   node coop/harness/stage15.mjs path/to/rom.gba
import { startRig, tally, OFFSETS, runDebugScriptOn } from './rig.mjs';

const PORT = 8803;
const ROM = process.argv[2] || '/home/claude/crossroads/pokeemerald.gba';

// coop_gates.h. Hard-coded rather than read from the ROM because the point of
// the constant is that both consoles agree on it without negotiating.
const GATE_COOP_PVP_READY = 98;

// coop.c: READY_GATE_TIMEOUT_FRAMES, one minute.
const READY_GATE_TIMEOUT = 3600;

const BATTLE_TYPE_DOUBLE = 1 << 0;
const BATTLE_TYPE_LINK   = 1 << 1;
const BATTLE_TYPE_TRAINER = 1 << 3;
const BATTLE_TYPE_MULTI  = 1 << 6;

const isBusy = async (rig, w) =>
  (await rig.mailbox(w)).flags.includes('SCRIPT_BUSY');

// Whether a battle is running. Not gBattleTypeFlags -- the game never clears
// it, so it still reads as a link battle while the player walks around
// afterwards, which an earlier stage believed and reported a battle that had
// finished minutes before.
const inBattle = async (rig, w) =>
  (await rig.mailbox(w)).flags.includes('BATTLE');

const pvp = async (rig, w) => rig.u8(w, OFFSETS.pvpActiveAddr);
const gateId = async (rig, w) => rig.u16(w, OFFSETS.gateIdAddr);

async function freeUp(rig, w) {
  for (let i = 0; i < 12 && await isBusy(rig, w); i++) {
    await rig.tap(w, 'B', 6);
    await rig.wait(20);
  }
}

// The party, as numbers that would change if a battle had kept anything.
//
// Level and max HP pin which Pokemon it is; hp is what a battle takes away.
// Read straight out of gParties[B_TRAINER_0] with the offsets the compiler
// published, because struct Pokemon's layout is not something to restate here.
async function partySnapshot(rig, w, trainer = 0) {
  const { partiesAddr, sizeofPokemon, monHp, monMaxHp, monLevel, partySize } = OFFSETS;
  const mons = [];
  for (let i = 0; i < partySize; i++) {
    const base = partiesAddr + (trainer * partySize + i) * sizeofPokemon;
    const level = await rig.u8(w, base + monLevel);
    const hp = await rig.u16(w, base + monHp);
    const maxHp = await rig.u16(w, base + monMaxHp);
    if (maxHp === 0) break;           // an empty slot ends the party
    mons.push({ level, hp, maxHp });
  }
  return mons;
}

const describe = (mons) =>
  mons.length === 0 ? '(empty)'
                    : mons.map((m) => `L${m.level} ${m.hp}/${m.maxHp}`).join(' ');

const sameParty = (a, b) =>
  a.length === b.length &&
  a.every((m, i) => m.level === b[i].level && m.hp === b[i].hp && m.maxHp === b[i].maxHp);

// Answer the yes/no the offer puts up. The cursor opens on YES.
//
// Checked rather than counted. A blind press is how the first run of this
// went: the offer's text had a paragraph break in it, the single A press
// turned the page instead of answering, and the console sat on an unanswered
// Yes/No for the rest of the run while every check downstream reported
// something else as broken.
async function answerOffer(rig, w, yes, want) {
  for (let i = 0; i < 8; i++) {
    if (!yes) { await rig.tap(w, 'Down', 6); await rig.wait(16); }
    await rig.tap(w, 'A', 8);
    await rig.wait(60);
    if (await want()) return true;
  }
  return false;
}

// What saying yes looks like from outside: the console is sat on the gate,
// waiting for the other answer.
const atReadyGate = (rig, w) => async () =>
  (await gateId(rig, w)) === GATE_COOP_PVP_READY;

// And saying no: the script is over and the player has their controls back.
const backInTheWorld = (rig, w) => async () => !(await isBusy(rig, w));

// Put the offer up. Script 20 is `goto CoopEventScript_BattlePartner`, which
// is what pressing A at the partner runs -- reached through the menu because a
// rig cannot reliably walk two sprites onto adjacent tiles, and the tile
// arithmetic is not what this is checking.
const offer = (rig, w) => runDebugScriptOn(rig, w, 20, { dismiss: false });

async function main() {
  const rig = await startRig({ rom: ROM, port: PORT });
  const t = tally();

  try {
    const m = await rig.mailboxes();
    if (m.some((x) => x.state !== 'ACTIVE'))
      throw new Error('the session never reached ACTIVE; nothing below is meaningful');

    // One Pokemon each. Script 21 rather than the usual six-strong test team:
    // a 6v6 between two level-50 teams runs for thousands of frames and puts a
    // replacement-choice screen in front of the rig every time something
    // faints. What this stage is about is the battle starting, ending, and
    // costing nothing -- all of which one Pokemon a side settles in a minute.
    console.log('\n--- giving both players a Pokemon ---');
    for (const w of [0, 1]) {
      await runDebugScriptOn(rig, w, 21);
      await rig.wait(60);
      await freeUp(rig, w);
    }

    const before = [await partySnapshot(rig, 0), await partySnapshot(rig, 1)];
    t.note('p1 party', describe(before[0]));
    t.note('p2 party', describe(before[1]));
    if (before.some((p) => p.length === 0))
      throw new Error('a console has no Pokemon; there is no battle to have');

    // Money is stored XOR'd with the save's encryption key, so the raw word
    // is meaningless on its own -- and two meaningless words that differ look
    // exactly like money that changed. Decrypt it, and print it, so a wrong
    // offset shows up as a nonsense amount rather than as a failed check.
    const money = async (w) =>
      (await rig.u32(w, OFFSETS.saveBlock1Addr + OFFSETS.sb1Money))
      ^ (await rig.u32(w, OFFSETS.saveBlock2Addr + OFFSETS.encryptionKey));
    const moneyBefore = [await money(0), await money(1)];
    t.note('money', `p1=${moneyBefore[0]} p2=${moneyBefore[1]}`);

    // ------------------------------------------------------- one yes is not enough
    console.log('\n--- player 1 offers; player 2 says no ---');
    await rig.clearGateLog();
    await offer(rig, 0);
    await rig.wait(180);
    await rig.shot('/tmp/claude-0/stage15-offer');

    t.check('both consoles are asking', (await isBusy(rig, 0)) && (await isBusy(rig, 1)),
            `p1 busy=${await isBusy(rig, 0)} p2 busy=${await isBusy(rig, 1)}`);

    const agreed = await answerOffer(rig, 0, true, atReadyGate(rig, 0));
    t.check('saying yes puts that player on the gate', agreed,
            `player 1 answered and is on gate ${await gateId(rig, 0)}`);
    const refused = await answerOffer(rig, 1, false, backInTheWorld(rig, 1));
    t.check('saying no ends it for that player', refused,
            'player 2 is still in the prompt after declining');
    await rig.wait(180);

    t.note('after one yes and one no',
           `p1 gate=${await gateId(rig, 0)} p1 busy=${await isBusy(rig, 0)} ` +
           `p2 busy=${await isBusy(rig, 1)}`);

    t.check('the declined offer started no battle',
            !(await inBattle(rig, 0)) && !(await inBattle(rig, 1)),
            'a battle began that one player said no to');
    t.check('and the player who agreed is waiting, not gone',
            (await gateId(rig, 0)) === GATE_COOP_PVP_READY,
            `player 1 is sat on gate ${await gateId(rig, 0)}, not the PvP one`);

    // The whole reason the give-up gate exists. coopgate waits for ever, which
    // is right between the two halves of one scene and ruinous here: the
    // player who agreed would be locked in place by a partner who never will.
    console.log('\n--- and is let go when the offer lapses ---');
    await rig.wait(READY_GATE_TIMEOUT + 300);
    await rig.shot('/tmp/claude-0/stage15-lapsed');

    t.check('the waiting player was released', !(await isBusy(rig, 0)),
            'player 1 is still locked at the gate a minute later');
    t.check('still no battle', !(await inBattle(rig, 0)) && !(await inBattle(rig, 1)));

    for (const w of [0, 1]) await freeUp(rig, w);

    // ----------------------------------------------------------- two yeses
    console.log('\n--- and now both say yes ---');
    await offer(rig, 1);           // the other player offers this time
    await rig.wait(180);
    t.check('the offer mirrors from player 2 as well',
            (await isBusy(rig, 0)) && (await isBusy(rig, 1)),
            'only the console that pressed A is asking');

    // The second one to answer never sees the gate: by the time it could be
    // read, both answers are in and the battle is already being built. So the
    // first player's answer is the one checked against the gate, and the
    // second's against the battle appearing below.
    await answerOffer(rig, 1, true, atReadyGate(rig, 1));
    await answerOffer(rig, 0, true, async () => (await pvp(rig, 0)) === 1);

    // The entry runs a link teardown, a rebuild and a player exchange before
    // the battle exists. Wait for the thing itself rather than a frame count.
    let started = false;
    for (let i = 0; i < 25 && !started; i++) {
      await rig.wait(60);
      started = (await inBattle(rig, 0)) && (await inBattle(rig, 1));
    }
    await rig.shot('/tmp/claude-0/stage15-battle');

    t.check('both consoles are in the battle', started,
            'the battle never started on both');

    // Started is not the same as built. The entry tears one link down and
    // builds another, exchanges players, swaps six Pokemon two at a time and
    // runs the VS screen before there is a battle to look at -- so every
    // reading taken at `started` describes a battle that does not exist yet.
    // Two battlers is the battle itself saying it is ready.
    const battlers = (w) => rig.u8(w, OFFSETS.battlersCountAddr);
    let built = false;
    for (let i = 0; i < 30 && !built; i++) {
      await rig.wait(60);
      built = (await battlers(0)) === 2 && (await battlers(1)) === 2;
    }
    t.check('and it finished setting up', built,
            `battlers p1=${await battlers(0)} p2=${await battlers(1)} after 1800 frames`);
    await rig.shot('/tmp/claude-0/stage15-built');
    t.check('and both know it is each other', (await pvp(rig, 0)) === 1 && (await pvp(rig, 1)) === 1,
            `p1 pvp=${await pvp(rig, 0)} p2 pvp=${await pvp(rig, 1)}`);

    // A plain two-player link battle, which is the one configuration in the
    // game that already means this. MULTI and BATTLE_TOWER are what the co-op
    // battle against trainers needs to make four players' machinery behave
    // for two; with two humans on opposite sides there is nothing to correct,
    // and leaving them on would put the partner's Pokemon on our own side.
    const flags = await rig.u32(0, OFFSETS.battleTypeFlagsAddr);
    t.note('battle type', `0x${flags.toString(16)}`);
    t.check('it is a link trainer battle',
            (flags & BATTLE_TYPE_LINK) !== 0 && (flags & BATTLE_TYPE_TRAINER) !== 0,
            `0x${flags.toString(16)} is not a link trainer battle`);
    t.check('and not the multi configuration',
            (flags & BATTLE_TYPE_MULTI) === 0 && (flags & BATTLE_TYPE_DOUBLE) === 0,
            `0x${flags.toString(16)} still carries MULTI or DOUBLE`);

    // Each side brings its own team, uncapped: the picker's three-a-side cap
    // is there to keep a gym leader's six honest, and between two players
    // whatever they are carrying is a fair fight by definition.
    //
    // Counted out of the party itself rather than read from gPartiesCount.
    // That counter is filled by CalculateEnemyPartyCount, which only the
    // non-link path calls -- a link battle memcpys the other player's six
    // straight into the row and leaves the count at zero, so believing it
    // reports every link battle as being against nobody.
    {
      const mine = await partySnapshot(rig, 0, 0);
      const theirs = await partySnapshot(rig, 0, 1);
      t.note('in the battle', `yours=${describe(mine)}`);
      t.note('', `theirs=${describe(theirs)}`);
      t.check('both sides brought their whole team',
              mine.length === before[0].length && theirs.length === before[1].length,
              `${mine.length} v ${theirs.length} from parties of ` +
              `${before[0].length} and ${before[1].length}`);
      t.check('and it is the partner\'s own team on the far side',
              sameParty(theirs, before[1]),
              `${describe(before[1])}  ->  ${describe(theirs)}`);
    }

    // Where each console is in the battle's own setup handshake. Priceless
    // when one of them stops: gBattleCommunication[MULTIUSE_STATE] is the
    // state number in CB2_HandleStartBattle, so a console stuck at 2 is
    // waiting for a block its partner never sent, and the screen is black
    // either way.
    const commState = (w) => rig.u8(w, OFFSETS.battleCommAddr);
    if (process.env.COOP_PVP_TRACE) {
      for (let i = 0; i < 30; i++) {
        await rig.wait(i < 10 ? 10 : 60);
        const line = async (w) =>
          `comm=${await commState(w)} entry=${await rig.u8(w, OFFSETS.dbgBattleStepAddr)}` +
          ` players=${await rig.u8(w, OFFSETS.dbgBattlePlayersAddr)}` +
          ` close=${await rig.u8(w, OFFSETS.dbgBattleCbAddr)}` +
          ` refused=${await rig.u16(w, OFFSETS.readyCloseAttemptsAddr)}` +
          ` closed=${await rig.u16(w, OFFSETS.linkClosedCountAddr)}` +
          ` q=${await rig.u8(w, OFFSETS.dbgBattleQueueAddr)}` +
          ` bail=${await rig.u8(w, OFFSETS.dbgBattleBailAddr)}` +
          ` battlers=${await rig.u8(w, OFFSETS.battlersCountAddr)}` +
          ` pvp=${await pvp(rig, w)}`;
        console.log(`      t+${(i + 1) * 60}:  p1 ${await line(0)}   p2 ${await line(1)}`);
        if (i === 12) {
          for (const w of [0, 1]) {
            const n = await rig.u8(w, OFFSETS.dbgCloseTraceLenAddr);
            const bytes = await rig.readAt(w, OFFSETS.dbgCloseTraceAddr, Math.max(n, 1));
            console.log(`      p${w + 1} close trace (${n}): ` +
              [...bytes].slice(0, n).map((b) =>
                `stage${b & 15}${b & 0x10 ? '+up' : ''}${b & 0x20 ? '+players' : ''}`).join(' -> '));
          }
        }
        if (i % 10 === 9) await rig.shot(`/tmp/claude-0/stage15-trace${i}`);
      }
    }

    if (process.env.COOP_SETUP_ONLY) { t.summary(); return; }

    // ------------------------------------------------------------ fight it
    //
    // Two identical level-50 teams, so straight A -- FIGHT, first move,
    // target -- settles it soon enough. No directions: in a battle the cursor
    // opens on FIGHT, so a Down spends the turn opening POKEMON instead.
    // ---------------------------------------------------------- fight it
    //
    // Straight A -- FIGHT, first move, target -- and let it run. No
    // directions: in a battle the cursor opens on FIGHT, so a Down spends the
    // turn opening POKEMON instead.
    //
    // Six level-50 Pokemon a side over a link is a long fight, and every turn
    // is several round trips through the relay. The round budget is generous
    // for that reason; the fight is skipped entirely unless asked for, since
    // everything above it is what usually needs re-checking.
    console.log('\n--- fighting it out ---');
    const lead = async (w) => (await partySnapshot(rig, w, 0))[0]?.hp ?? 0;
    let hurt = false;
    let ended = null;
    for (let round = 0; round < 600; round++) {
      await rig.tap('both', 'A', 8);
      await rig.wait(24);
      // A only, and no B at all.
      //
      // stage 5 sprinkles B presses in so a mistaken menu cannot park the run
      // in a submenu for ever. In a single battle it does the opposite: B
      // backs out of the move list to the action menu, the next A opens it
      // again, and the pair of them can walk the cursor onto POKEMON and
      // leave a console sat on the party screen -- which is where this run
      // spent two thousand rounds while its partner waited, correctly, on
      // "Link standby...". Straight A walks FIGHT -> first move -> done, every
      // turn, and picks a replacement when something faints.
      if (round % 5 === 4 && !(await inBattle(rig, 0)) && !(await inBattle(rig, 1))) {
        ended = round;
        break;
      }
      if (round % 25 === 24) {
        const [a, b] = [await lead(0), await lead(1)];
        if (a < before[0][0].hp || b < before[1][0].hp) hurt = true;
        console.log(`      r${round}: leads ${a}/${before[0][0].maxHp} ` +
          `and ${b}/${before[1][0].maxHp}  ` +
          `exec p1=${(await rig.u32(0, OFFSETS.execFlagsAddr)).toString(16)}` +
          ` p2=${(await rig.u32(1, OFFSETS.execFlagsAddr)).toString(16)}  ` +
          `drops=${await rig.u16(0, OFFSETS.dbgSendDropsAddr)}` +
          `/${await rig.u16(1, OFFSETS.dbgSendDropsAddr)}  ` +
          `backlogMax=${await rig.u8(0, OFFSETS.dbgBacklogMaxAddr)}` +
          `/${await rig.u8(1, OFFSETS.dbgBacklogMaxAddr)}  ` +
          `recvQ=${await rig.u8(0, OFFSETS.dbgRecvQueueAddr)}` +
          `/${await rig.u8(1, OFFSETS.dbgRecvQueueAddr)}`);
      }
    }
    t.check('the two of them actually fought', hurt,
            'nobody took a point of damage, so no turn ever resolved');
    t.check('the battle ended and both consoles came out of it', ended !== null,
            'still fighting after 600 rounds');
    t.note('ended', ended === null ? 'never' : `round ${ended}`);

    // Back to the world, and the session rebuilt: it was torn down to make
    // the battle's link, and if it does not come back the two players are
    // standing in the same world unable to see each other.
    await rig.wait(600);
    for (const w of [0, 1]) await freeUp(rig, w);
    await rig.wait(240);
    await rig.shot('/tmp/claude-0/stage15-after');

    const after = [await partySnapshot(rig, 0), await partySnapshot(rig, 1)];
    t.note('p1 party after', describe(after[0]));
    t.note('p2 party after', describe(after[1]));

    t.check('the co-op session came back',
            (await rig.mailboxes()).every((x) => x.state === 'ACTIVE'),
            (await rig.mailboxes()).map((x) => x.state).join('/'));
    t.check('neither console thinks it is still in a PvP battle',
            (await pvp(rig, 0)) === 0 && (await pvp(rig, 1)) === 0,
            `p1 pvp=${await pvp(rig, 0)} p2 pvp=${await pvp(rig, 1)}`);

    // The point of the whole thing: it cost nothing.
    t.check('player 1 got their team back untouched', sameParty(before[0], after[0]),
            `${describe(before[0])}  ->  ${describe(after[0])}`);
    t.check('player 2 got their team back untouched', sameParty(before[1], after[1]),
            `${describe(before[1])}  ->  ${describe(after[1])}`);

    const moneyAfter = [await money(0), await money(1)];
    t.check('and nobody paid the loser\'s forfeit',
            moneyAfter[0] === moneyBefore[0] && moneyAfter[1] === moneyBefore[1],
            `p1 ${moneyBefore[0]}->${moneyAfter[0]}  p2 ${moneyBefore[1]}->${moneyAfter[1]}`);

    t.summary();
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
