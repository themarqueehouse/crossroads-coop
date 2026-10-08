#!/usr/bin/env node
// Stage-5 check: can a co-op battle be FOUGHT, and does it end cleanly?
//
// Stage 4 proved a co-op battle starts on both consoles. Starting is not the
// hard half. The configuration we ride is the Battle Tower's, and the tower has
// opinions about what happens when a battle ends -- prize money, its own
// win/loss bookkeeping, where it sends you afterwards. None of that has run
// over this transport either, and sixteen gyms is a bad place to find out.
//
// So: give both consoles something that can win, start the battle, mash
// through it, and watch for three things that matter more than the result.
//   1. the battle ends at all, rather than hanging mid-fight
//   2. both consoles come back to the overworld
//   3. the co-op session rebuilds itself afterwards -- it was suspended for the
//      battle, and if it does not come back the two players are standing in the
//      same world unable to see each other
//
//   node coop/harness/stage5.mjs path/to/rom.gba
import { startRig, tally, OFFSETS, pickThreeOnBoth } from './rig.mjs';

const PORT = 8793;
const ROM = process.argv[2] || '/home/claude/crossroads/pokeemerald.gba';
const SCRIPTS_MENU_INDEX = 5;

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

// Whether a co-op battle is running.
//
// NOT gBattleTypeFlags: the game never clears it when a battle ends, so it
// still reads as a four-battler linked double battle while the player walks
// around the overworld afterwards. An earlier version of this check believed
// that and reported a battle which had in fact finished minutes ago. The
// session's own suspend flag is published every frame it is stood down for a
// battle, and only then.
const inBattle = async (rig, w) =>
  (await rig.mailbox(w)).flags.includes('BATTLE');

async function main() {
  const rig = await startRig({ rom: ROM, port: PORT });
  const t = tally();

  try {
    let m = await rig.mailboxes();
    if (m.some((x) => x.state !== 'ACTIVE'))
      throw new Error('the session never reached ACTIVE');

    console.log('\n--- giving both players something that can fight ---');
    for (const w of [0, 1]) {
      await runDebugScript(rig, w, 5);
      await rig.wait(60);
      await freeUp(rig, w);
    }

    console.log('\n--- starting the co-op battle ---');
    await rig.clearGateLog();
    await runDebugScript(rig, 0, Number(process.env.COOP_SLOT || 4));

    // A split battle now opens a party picker on BOTH consoles before it
    // starts. Photograph it, then choose three on each.
    if (process.env.COOP_PICK) {
      // The guest starts the mirrored scene a little after the host, so its
      // picker opens later. Waiting 120 frames caught only the host's and made
      // it look as though the partner never got one.
      await rig.wait(180);
      await rig.shot('/tmp/claude-0/stage5-picker');
      const picked = await pickThreeOnBoth(rig, OFFSETS.selectedOrderAddr, OFFSETS.pickerOpenedAddr);
      t.check('both players chose three', picked[0] === 3 && picked[1] === 3,
              `p1 chose ${picked[0]}, p2 chose ${picked[1]}`);
      await rig.shot('/tmp/claude-0/stage5-picked');
      await rig.wait(120);
    }

    // Wait for the battle to actually be SET UP, not merely started.
    //
    // A fixed wait here is what produced an evening of fiction. The entry runs
    // a link teardown, a rebuild, a player exchange and a party preview before
    // CB2_InitBattleInternal -- where the opponents are generated -- and that
    // takes around 480 frames. Measuring at 400 caught it mid-flight every
    // time, and every reading taken then described a battle that had not been
    // built yet: no opponents, an init function that "never ran". All true at
    // the moment asked, all meaningless.
    let setUp = false;
    for (let i = 0; i < 20 && !setUp; i++) {
      await rig.wait(60);
      setUp = (await rig.u8(0, OFFSETS.dbgPathAddr)) === 14
           && (await rig.u8(1, OFFSETS.dbgPathAddr)) === 14;
    }
    t.check('the battle finished setting up on both consoles', setUp,
            'still in the entry sequence after 1200 frames');
    await rig.wait(120);

    t.check('both consoles are in the battle',
            (await inBattle(rig, 0)) && (await inBattle(rig, 1)));

    // How many Pokemon each trainer slot actually has. gPartiesCount is
    // indexed by B_TRAINER_0..3: your party, opponent A, your partner's party,
    // opponent B. Opponents reading 0 means the generation never happened,
    // which is what "two trainers at level 0" looks like from the inside.
    for (const w of [0, 1]) {
      const counts = await rig.readAt(w, OFFSETS.partiesCountAddr, 4);
      t.note(`p${w + 1} party counts`,
             `you=${counts[0]} foeA=${counts[1]} partner=${counts[2]} foeB=${counts[3]}`);
    }
    {
      const c0 = await rig.readAt(0, OFFSETS.partiesCountAddr, 4);
      t.check('the opponents have Pokemon at all', c0[1] > 0 && c0[3] > 0,
              'generation produced empty parties');
    }
    for (const w of [0, 1]) {
      t.note(`p${w + 1} generation`,
        `branch ran=${await rig.u8(w, OFFSETS.dbgMadeAddr)} ` +
        `foeA made=${await rig.u8(w, OFFSETS.dbgFoeAAddr)} ` +
        `foeB made=${await rig.u8(w, OFFSETS.dbgFoeBAddr)} ` +
        `count right after=${await rig.u8(w, OFFSETS.dbgAfterAddr)} ` +
        `species[0] early=${await rig.u16(w, OFFSETS.dbgSpeciesEarlyAddr)} ` +
        `late=${await rig.u16(w, OFFSETS.dbgSpeciesAddr)} ` +
        `trainer partySize=${await rig.u8(w, OFFSETS.dbgPartySizeAddr)} ` +
        `data.species=${await rig.u16(w, OFFSETS.dbgDataSpeciesAddr)} ` +
        `data.lvl=${await rig.u8(w, OFFSETS.dbgDataLevelAddr)} ` +
        `poolSize=${await rig.u8(w, OFFSETS.dbgPoolSizeAddr)}`);
      t.note(`p${w + 1} init`,
        `reached CB2_InitBattleInternal=${await rig.u8(w, OFFSETS.dbgReachedAddr)} ` +
        `coopActive=${await rig.u8(w, OFFSETS.dbgCoopActiveAddr)} ` +
        `isDebugBattle=${await rig.u8(w, OFFSETS.dbgIsDebugAddr)} ` +
        `path=${await rig.u8(w, OFFSETS.dbgPathAddr)} ` +
        `multiuseState=${await rig.u8(w, OFFSETS.battleCommAddr)}`);
      t.note(`p${w + 1} link`,
        `status=0x${(await rig.u32(w, OFFSETS.dbgLinkStatusAddr)).toString(16)} ` +
        `callbackInstalled=${await rig.u8(w, OFFSETS.dbgHasCallbackAddr)} ` +
        `recvPlayers=${await rig.u8(w, OFFSETS.dbgRecvPlayersAddr)} ` +
        `recvQueue=${await rig.u8(w, OFFSETS.dbgRecvQueueAddr)}`);
      const pf = await rig.readAt(w, OFFSETS.paletteFadeAddr, 16);
      t.note(`p${w + 1} paletteFade`,
             pf.map((b) => b.toString(16).padStart(2, '0')).join(' '));
    }

    // Mash. In a double battle A walks FIGHT -> move -> target, and a level 50
    // starter against two bug catchers does not need the moves chosen well.
    // B as well as A, so a mistaken menu does not park us in a submenu for ever.
    // The fight and everything after it is the slow half of this run. When
    // only the SETUP is under test -- which trainers, which teams -- skip it.
    if (process.env.COOP_SETUP_ONLY) {
      // Screenshot BEFORE returning. Skipping it left the previous run's
      // image on disk, and I read it as if it were this one -- twice now.
      await rig.shot('/tmp/claude-0/stage5-setup-only');
      t.summary();
      return;
    }

    console.log('\n--- fighting it ---');
    let endedAt = null;
    let lastExec = null;
    let stuckFor = 0;
    // 60 rounds was enough when this fought two bug catchers with a full party
    // of six. It is not enough now: the picker caps each side at three, and
    // Crossroads scales the opponents to the player's level, so the same
    // script now has three Pokemon beating a level-50 gym leader with
    // whichever move happens to be first. A run that stopped at 60 reported a
    // battle that would not end -- it was simply still going.
    for (let round = 0; round < 700; round++) {
      await rig.tap('both', 'A', 8);
      await rig.wait(24);
      // A only. Directions were added here to vary which move gets picked, and
      // they did the opposite: in a battle the cursor starts on FIGHT, so a
      // Down or Right moves it onto POKEMON or BAG and the turn is spent
      // opening a menu instead of attacking. A run doing that sat through
      // hundreds of rounds with all four Pokemon still at full health, which
      // looked like a battle that would not end and was really a battle in
      // which nobody was fighting. Straight A walks FIGHT -> first move ->
      // target, every turn.
      //
      // B stays, occasionally, so a mistaken menu does not park the run in a
      // submenu for ever -- but not often enough to cancel the move choice.
      if (round % 11 === 10) { await rig.tap('both', 'B', 8); await rig.wait(24); }

      if (round % 5 === 4) {
        const live = [await inBattle(rig, 0), await inBattle(rig, 1)];
        if (!live[0] && !live[1]) { endedAt = round; break; }
      }

      // Catch the exact round the battle wedges, and photograph it.
      //
      // gBattleControllerExecFlags is the battle saying what it is waiting
      // for: bits 0-3 are a battler active for player 0, bits 4-7 the same for
      // player 1, bits 28-31 a message still outbound over the link. A link
      // battle that stops is always a bit in here that never clears, and the
      // screen cannot say which one -- "Link standby..." is printed either
      // way. Sampling only every 60 rounds showed it already stuck without
      // showing what it was doing when it got there.
      if (round % 100 === 99) {
        console.log(`      r${round} drops p1=${await rig.u16(0, OFFSETS.dbgSendDropsAddr)}` +
          ` p2=${await rig.u16(1, OFFSETS.dbgSendDropsAddr)}` +
          `  backlog p1=${await rig.u8(0, OFFSETS.dbgBacklogMaxAddr)}` +
          ` p2=${await rig.u8(1, OFFSETS.dbgBacklogMaxAddr)}` +
          `  recvQ p1=${await rig.u8(0, OFFSETS.dbgRecvQueueAddr)}` +
          ` p2=${await rig.u8(1, OFFSETS.dbgRecvQueueAddr)}` +
          `  stalls p1=${await rig.u16(0, OFFSETS.dbgStallsAddr)}` +
          ` p2=${await rig.u16(1, OFFSETS.dbgStallsAddr)}`);
      }

      if (round % 10 === 9) {
        const ex = [await rig.u32(0, OFFSETS.execFlagsAddr),
                    await rig.u32(1, OFFSETS.execFlagsAddr)];
        const key = ex.join('/');
        if (key === lastExec && key !== '0/0') {
          stuckFor++;
          if (stuckFor === 4) {
            console.log(`      wedged at round ${round}: exec=0x${ex[0].toString(16)}` +
                        ` / 0x${ex[1].toString(16)}`);
            for (const w of [0, 1]) {
              const cmds = await rig.readAt(w, OFFSETS.dbgBattlerCmdAddr, 4);
              console.log(`        p${w + 1} pending cmd per battler = ` +
                `[${[...cmds].join(', ')}]  sendQueued=` +
                `${await rig.u8(w, OFFSETS.dbgSendPendingAddr)}`);
              const sent = await rig.readAt(w, OFFSETS.dbgDoneSentAddr, 8);
              const recv = await rig.readAt(w, OFFSETS.dbgDoneRecvAddr, 8);
              const pair = (b) => [b[0] | (b[1] << 8), b[2] | (b[3] << 8)];
              console.log(`        p${w + 1} done msgs  sent[p0,p1]=` +
                `${pair(sent)}  recv[p0,p1]=${pair(recv)}`);
              console.log(`        p${w + 1} transport  drops=` +
                `${await rig.u16(w, OFFSETS.dbgSendDropsAddr)}  backlogMax=` +
                `${await rig.u8(w, OFFSETS.dbgBacklogMaxAddr)}  recvQueue=` +
                `${await rig.u8(w, OFFSETS.dbgRecvQueueAddr)}`);
            }
            await rig.shot('/tmp/claude-0/stage5-wedged');
          }
        } else {
          if (stuckFor >= 4) console.log(`      ...moved again at round ${round}`);
          stuckFor = 0;
          lastExec = key;
        }
      }
    }

    // What is under test is that the battle keeps MOVING, not that it is won.
    //
    // It used to be "the battle ended", which was the wrong bar twice over.
    // Random button presses cannot reliably beat a gym leader scaled to the
    // player's level, so the check failed on runs where nothing was wrong --
    // and it would have passed on a battle that ended by hanging, which is the
    // failure that actually mattered. A dropped link message left a battler
    // permanently owed an acknowledgement, with every controller idle and
    // nothing left to send: gBattleControllerExecFlags simply stopped changing
    // and both consoles sat on "Link standby..." for ever.
    //
    // So the assertion is that the flags never froze. Finishing is reported
    // either way, because it is worth seeing, and the checks that follow only
    // mean anything if it did.
    t.note('battle ended', endedAt === null ? 'no -- still going at the end of the run'
                                            : `after ~${endedAt} rounds of input`);
    t.check('the battle never wedged', stuckFor < 4,
            `gBattleControllerExecFlags stopped changing at ${lastExec}`);

    await rig.shot('/tmp/claude-0/stage5-after-battle');

    // Back to the overworld, and the session rebuilt.
    console.log('\n--- after the battle ---');
    for (let i = 0; i < 10; i++) {
      await rig.wait(120);
      m = await rig.mailboxes();
      console.log(`  t+${(i + 1) * 120}: ` + m.map((x) =>
        `p${x.id} ${x.state} map=${x.selfMap} flags=${x.flags}`).join('   '));
      if (m.every((x) => x.state === 'ACTIVE')) break;
      await rig.tap('both', 'A', 8);
    }

    if (endedAt === null) {
      t.note('after the battle', 'skipped -- the fight was still going');
      t.summary();
      return;
    }

    m = await rig.mailboxes();
    t.check('both consoles came back to the overworld',
            !(await inBattle(rig, 0)) && !(await inBattle(rig, 1)));
    t.check('the co-op session rebuilt itself',
            m.every((x) => x.state === 'ACTIVE'),
            m.map((x) => x.state).join('/'));
    t.check('they can see each other again',
            m.every((x) => x.flags.includes('PEER_SAME_MAP')),
            m.map((x) => x.flags).join(' | '));

    for (const w of [0, 1]) {
      const site = await rig.u8(w, OFFSETS.linkErrorSiteAddr);
      if (site) t.note(`p${w + 1} link error`, `site ${site}`);
    }
    t.check('no communication error anywhere',
            (await rig.u8(0, OFFSETS.linkErrorSiteAddr)) === 0 &&
            (await rig.u8(1, OFFSETS.linkErrorSiteAddr)) === 0);

    await rig.shot('/tmp/claude-0/stage5-back-in-world');
    t.summary();
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
