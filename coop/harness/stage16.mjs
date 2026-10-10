#!/usr/bin/env node
// Stage-16 check: the scattered legendaries are actually standing there.
//
// 108 of them were placed by a tool that read the maps' own collision data,
// and a tool that is wrong about collision puts a Celebi inside a tree. The
// things worth establishing are cheap and none of them can be read off the
// generated files:
//
//   1. the object exists on the map at all -- a species graphics id is not
//      an ordinary sprite, and one the ROM cannot draw is a blank tile
//   2. it is where the tool said, and the player can walk up to it
//   3. talking to it starts a battle, rather than running a script that
//      falls through or locks the console
//   4. the partner sees it too, since the world is shared
//
// Petalburg Woods, because Celebi is the one placed there and the woods are
// an hour into the game rather than behind Surf and four badges.
//
//   node coop/harness/stage16.mjs path/to/rom.gba
import { startRig, tally, OFFSETS, runDebugScriptOn } from './rig.mjs';

const PORT = 8804;
const ROM = process.argv[2] || '/home/claude/crossroads/pokeemerald.gba';

// data/maps/PetalburgWoods/map.json, written by place_legendaries.py.
// The tile the tool chose, plus MAP_OFFSET: a map's own coordinates and the
// ones an object event carries differ by 7, which is how the first run of this
// decided Celebi was in the wrong place while it was standing exactly where it
// had been put.
const MAP_OFFSET = 7;
const CELEBI = { x: 24 + MAP_OFFSET, y: 25 + MAP_OFFSET };
const MAP_PETALBURG_WOODS = 0x0f22;   // group 0x22, map 0x0f -- read below anyway

const isBusy = async (rig, w) =>
  (await rig.mailbox(w)).flags.includes('SCRIPT_BUSY');

// Whether a battle is running, read from the battle rather than the mailbox.
//
// The mailbox's BATTLE flag means "the co-op session stood down for a battle",
// which a co-op battle does and a wild battle does not -- so the first version
// of this reported that talking to Celebi had done nothing while a Celebi was
// on screen at level 50. An opponent with hit points is the battle itself
// saying it exists.
const inBattle = async (rig, w) => {
  const { partiesAddr, sizeofPokemon, monMaxHp, partySize } = OFFSETS;
  const enemy = partiesAddr + partySize * sizeofPokemon;   // B_TRAINER_1
  return (await rig.u16(w, enemy + monMaxHp)) > 0;
};

// Clear the last battle's leftovers, so the check above is about this one.
const forgetLastBattle = async (rig, w) => {
  const { partiesAddr, sizeofPokemon, monMaxHp, partySize } = OFFSETS;
  await rig.writeAt(w, partiesAddr + partySize * sizeofPokemon + monMaxHp, [0, 0]);
};

// Every object event on the map, as the console has them: local id, where it
// is, and what it is drawn as. OBJ_EVENT_MON (1 << 14) marks a species sprite.
async function objects(rig, w) {
  const { objectEventsAddr, sizeofObjectEvent, objLocalId, objCurrentCoords,
          objGraphicsId, coordsX, coordsY, objectEventsCount } = OFFSETS;
  const out = [];
  for (let i = 0; i < objectEventsCount; i++) {
    const base = objectEventsAddr + i * sizeofObjectEvent;
    // struct ObjectEvent opens with its bitfields, and `active` is bit 0 of
    // the first byte. Without checking it this counts slots that merely hold
    // the leftovers of something that used to be there, which is how a map
    // with room to spare reported itself full.
    const live = ((await rig.u8(w, base)) & 1) !== 0;
    if (!live) continue;
    const localId = await rig.u8(w, base + objLocalId);
    const gfx = await rig.u16(w, base + objGraphicsId);
    const x = await rig.u16(w, base + objCurrentCoords + coordsX);
    const y = await rig.u16(w, base + objCurrentCoords + coordsY);
    out.push({ i, localId, gfx, x, y });
  }
  return out;
}

const OBJ_EVENT_MON = 1 << 14;

async function main() {
  const rig = await startRig({ rom: ROM, port: PORT });
  const t = tally();

  try {
    if ((await rig.mailboxes()).some((x) => x.state !== 'ACTIVE'))
      throw new Error('the session never reached ACTIVE');

    console.log('\n--- a team, and then into the woods ---');
    await runDebugScriptOn(rig, 0, 5);
    await rig.wait(60);
    // Script 22 warps to Petalburg Woods, one tile from where Celebi was put.
    await runDebugScriptOn(rig, 0, 22);
    await rig.wait(240);

    // The partner follows: the co-op session pulls them along, which is how
    // they come to be looking at the same Celebi.
    for (let i = 0; i < 20; i++) {
      await rig.wait(60);
      const m = await rig.mailboxes();
      if (m[0].selfMap === m[1].selfMap) break;
    }
    await rig.shot('/tmp/claude-0/stage16-woods');

    const m = await rig.mailboxes();
    t.note('maps', m.map((x) => `p${x.id} ${x.selfMap}`).join('  '));

    // --- 1 and 2: it is there, and it is where it should be --------------
    const mine = await objects(rig, 0);
    const mons = mine.filter((o) => (o.gfx & OBJ_EVENT_MON) !== 0);
    t.note('species-sprite objects on the map',
           mons.map((o) => `#${o.localId} species ${o.gfx & 0x3fff} at ${o.x},${o.y}`)
               .join('  ') || 'none');

    t.check('a Pokemon is standing in the woods', mons.length > 0,
            'no object on the map is drawn as a species');

    const here = mons.find((o) => Math.abs(o.x - CELEBI.x) <= 1 &&
                                  Math.abs(o.y - CELEBI.y) <= 1);
    t.check('and it is where the tool put it', here !== undefined,
            `expected something near ${CELEBI.x},${CELEBI.y}`);

    // --- 4: the partner sees it too ---------------------------------------
    // Give their console time to put the map together. Objects spawn as the
    // camera settles, and the partner arrives a moment after we do.
    let allTheirs = await objects(rig, 1);
    for (let i = 0; i < 12; i++) {
      if (allTheirs.some((o) => (o.gfx & OBJ_EVENT_MON) !== 0 && o.localId !== 241)) break;
      await rig.wait(60);
      allTheirs = await objects(rig, 1);
    }
    const theirs = allTheirs.filter((o) => (o.gfx & OBJ_EVENT_MON) !== 0);
    // How full their object list is. There are only sixteen slots, and a map
    // with its own NPCs plus a mirrored partner and two follower Pokemon can
    // run out -- in which case whatever spawns last is simply not there.
    t.note('partner object slots used', `${allTheirs.length} of ${OFFSETS.objectEventsCount}`);
    t.note('ours', `${mine.length} of ${OFFSETS.objectEventsCount}`);
    // Is it hidden rather than missing? The object's hide flag is also the
    // "already dealt with" flag, and flags are shared across the pair -- so a
    // flag that came up set on one console and not the other would explain an
    // object that exists for one player and not the other.
    const FLAG_LEGENDARY_CELEBI = 0xB26;
    const flagSet = async (w, id) => {
      const byte = await rig.u8(w, OFFSETS.saveBlock1Addr + OFFSETS.flags + (id >> 3));
      return ((byte >> (id & 7)) & 1) !== 0;
    };
    t.note('Celebi\'s flag',
           `p1 ${await flagSet(0, FLAG_LEGENDARY_CELEBI)}  ` +
           `p2 ${await flagSet(1, FLAG_LEGENDARY_CELEBI)}`);

    t.note('what the partner sees',
           theirs.map((o) => `#${o.localId} species ${o.gfx & 0x3fff} at ${o.x},${o.y}`)
                 .join('  ') || 'none');
    t.check('the partner is looking at the same one',
            theirs.some((o) => here && o.gfx === here.gfx),
            `partner sees ${theirs.length} species sprites, none of them it`);

    // --- 3: talking to it starts a battle ---------------------------------
    //
    // Walk into it rather than guessing a facing: the warp lands the player
    // directly below, so north and A is the whole interaction.
    console.log('\n--- talking to it ---');
    await forgetLastBattle(rig, 0);
    // The warp lands the player on the tile directly below it, so one press
    // north to face it and A to talk.
    await rig.tap(0, 'Up', 8);
    await rig.wait(40);
    for (let i = 0; i < 6 && !(await inBattle(rig, 0)); i++) {
      await rig.tap(0, 'A', 8);
      await rig.wait(120);
    }
    await rig.wait(180);
    await rig.shot('/tmp/claude-0/stage16-encounter');

    const fighting = await inBattle(rig, 0);
    t.check('talking to it starts a battle', fighting,
            `script busy=${await isBusy(rig, 0)}, no battle`);

    if (fighting) {
      const { partiesAddr, sizeofPokemon, monMaxHp, monLevel, partySize } = OFFSETS;
      const enemy = partiesAddr + partySize * sizeofPokemon;
      t.note('what stepped out',
             `level ${await rig.u8(0, enemy + monLevel)}, ` +
             `${await rig.u16(0, enemy + monMaxHp)} HP`);
    }

    // And the partner was not dragged into it: a legendary is a wild battle,
    // which belongs to whoever walked up to it.
    t.check('the partner was left alone', !(await inBattle(rig, 1)),
            'both consoles went into one player\'s wild battle');

    // ------------------------------------------------- the crowded route
    //
    // "None of the trainers there challenged any of us" came back from the
    // first real playtest, and Route 103 is where it was seen. It carries 20
    // object events -- more than the sixteen slots the game used to have for
    // the whole map, with four of those going to the two players and their
    // Pokemon -- so the ones listed last never spawned, and a trainer who is
    // not there cannot see you.
    // Out of the Celebi battle first. The previous version of this went
    // straight on to open the debug menu while a battle was still running,
    // where B does not open menus -- so the warp never happened and every
    // reading below described Petalburg Woods.
    console.log('\n--- leaving the Celebi battle ---');
    for (let i = 0; i < 25 && (await inBattle(rig, 0)); i++) {
      await rig.tap(0, 'Down', 8);
      await rig.wait(16);
      await rig.tap(0, 'Right', 8);
      await rig.wait(16);
      await rig.tap(0, 'A', 8);
      await rig.wait(60);
      await rig.tap(0, 'A', 8);
      await rig.wait(60);
    }
    t.check('you can run from it', !(await inBattle(rig, 0)),
            'still in the Celebi battle after 25 attempts to run');
    await rig.wait(180);

    console.log('\n--- Route 103, where nobody challenged anybody ---');
    await forgetLastBattle(rig, 0);
    await runDebugScriptOn(rig, 0, 23);
    await rig.wait(300);

    const onRoute = await objects(rig, 0);
    const where = await rig.mailboxes();
    t.note('where we ended up', where.map((x) => `p${x.id} map ${x.selfMap}`).join('  '));
    t.note('objects live on Route 103', `${onRoute.length} of ${OFFSETS.objectEventsCount}`);
    // Not a check. The game only spawns what is near the camera, so a wide
    // route never has all twenty of its objects up at once -- 11 here, with
    // twenty-one slots to spare. Worth printing, because "the trainers never
    // challenged us" could have been them never spawning, and this says it
    // was not that.
    t.note('spare slots', `${OFFSETS.objectEventsCount - onRoute.length} free`);

    // Amy and Liv are two tiles up, both facing down with a one-tile line of
    // sight, so the player has to actually step into it.
    //
    // More than one press, because the first one does not move anybody: the
    // warp lands them facing south, and a direction you are not already
    // facing turns you before it walks you. The previous version pressed Up
    // once, the player turned on the spot two tiles short of the twins, and
    // the check reported that trainers do not notice you.
    for (let i = 0; i < 3 && !(await inBattle(rig, 0)); i++) {
      await rig.tap(0, 'Up', 10);
      await rig.wait(120);
    }
    await rig.wait(240);
    await rig.shot('/tmp/claude-0/stage16-route103');

    // A trainer who notices you walks over and talks first. Press through the
    // introduction before asking whether there is a battle -- checking at the
    // moment of the step reported that nobody reacted while Amy was on screen
    // saying "I'm AMY, and this is my little sister LIV".
    const challenged = await isBusy(rig, 0);
    for (let i = 0; i < 10 && !(await inBattle(rig, 0)); i++) {
      await rig.tap(0, 'A', 8);
      await rig.wait(90);
    }
    t.check('a trainer notices you and comes over', challenged,
            'walked into two trainers\' line of sight and neither reacted');
    t.check('and it turns into a battle', await inBattle(rig, 0),
            'the introduction ran but no battle started');

    t.summary();
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
