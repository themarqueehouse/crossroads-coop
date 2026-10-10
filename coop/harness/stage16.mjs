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

const inBattle = async (rig, w) =>
  (await rig.mailbox(w)).flags.includes('BATTLE');

// Every object event on the map, as the console has them: local id, where it
// is, and what it is drawn as. OBJ_EVENT_MON (1 << 14) marks a species sprite.
async function objects(rig, w) {
  const { objectEventsAddr, sizeofObjectEvent, objLocalId, objCurrentCoords,
          objGraphicsId, coordsX, coordsY, objectEventsCount } = OFFSETS;
  const out = [];
  for (let i = 0; i < objectEventsCount; i++) {
    const base = objectEventsAddr + i * sizeofObjectEvent;
    const localId = await rig.u8(w, base + objLocalId);
    const gfx = await rig.u16(w, base + objGraphicsId);
    const x = await rig.u16(w, base + objCurrentCoords + coordsX);
    const y = await rig.u16(w, base + objCurrentCoords + coordsY);
    if (gfx === 0 && localId === 0) continue;
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
    const theirs = (await objects(rig, 1)).filter((o) => (o.gfx & OBJ_EVENT_MON) !== 0);
    t.check('the partner is looking at the same one',
            theirs.some((o) => here && o.gfx === here.gfx),
            `partner sees ${theirs.length} species sprites`);

    // --- 3: talking to it starts a battle ---------------------------------
    //
    // Walk into it rather than guessing a facing: the warp lands the player
    // directly below, so north and A is the whole interaction.
    console.log('\n--- talking to it ---');
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
      const species = await rig.u16(0, OFFSETS.partiesAddr + OFFSETS.sizeofPokemon * 6);
      t.note('battle started', 'the encounter is running');
    }

    // And the partner was not dragged into it: a legendary is a wild battle,
    // which belongs to whoever walked up to it.
    t.check('the partner was left alone', !(await inBattle(rig, 1)),
            'both consoles went into one player\'s wild battle');

    t.summary();
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
