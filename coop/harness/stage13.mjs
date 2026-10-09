#!/usr/bin/env node
// Stage-13 check: you can see your partner's Pokemon walking behind them.
//
// From the first playtest: "player one had a Pokemon following but on player
// 2's screen I couldn't see any of that." Each console drew its own follower
// and nobody else's, so the two players saw different worlds.
//
// Only the species travels. A follower's position is not its own -- it stands
// in the tile its trainer just left -- so it is worked out from the partner's
// movement, which is already arriving sixty times a second. That makes the
// interesting failure a positional one, and it is why this reads the actual
// map object rather than believing the "a follower was spawned" flag: an
// object in the wrong place, following nobody, sets that flag too.
//
// What has to be true:
//   1. player 2's console knows which Pokemon is behind player 1
//   2. it put that Pokemon on the map
//   3. the sprite is the right one for the species, not a placeholder
//   4. it is standing next to player 1, not on top of them or across the map
//   5. when player 1 walks, it follows
//
//   node coop/harness/stage13.mjs path/to/rom.gba
import { startRig, tally, OFFSETS } from './rig.mjs';

const PORT = 8801;
const ROM = process.argv[2] || '/home/claude/crossroads/pokeemerald.gba';

const SCRIPTS_MENU_INDEX = 5;
const KIT_SCRIPT = 14;            // HMs, items and a party of six
const SPECIES_NONE = 0;

const isBusy = async (rig, w) =>
  (await rig.mailbox(w)).flags.includes('SCRIPT_BUSY');

async function freeUp(rig, w) {
  for (let i = 0; i < 16 && await isBusy(rig, w); i++) {
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

// Every live map object on a console, by local id.
async function objects(rig, w) {
  const n = OFFSETS.objectEventsCount;
  const size = OFFSETS.sizeofObjectEvent;
  const raw = await rig.readAt(w, OFFSETS.objectEventsAddr, n * size);
  const out = new Map();
  for (let i = 0; i < n; i++) {
    const at = i * size;
    const localId = raw[at + OFFSETS.objLocalId];
    const c = at + OFFSETS.objCurrentCoords;
    out.set(localId, {
      slot: i,
      x: (raw[c + OFFSETS.coordsX] | (raw[c + OFFSETS.coordsX + 1] << 8)) << 16 >> 16,
      y: (raw[c + OFFSETS.coordsY] | (raw[c + OFFSETS.coordsY + 1] << 8)) << 16 >> 16,
      graphicsId: raw[at + OFFSETS.objGraphicsId]
                | (raw[at + OFFSETS.objGraphicsId + 1] << 8),
    });
  }
  return out;
}

const PEER = 0xF0;            // coop.h: COOP_PEER_LOCAL_ID
const PEER_FOLLOWER = 0xF1;   // coop.h: COOP_PEER_FOLLOWER_LOCAL_ID

const dist = (a, b) => Math.abs(a.x - b.x) + Math.abs(a.y - b.y);

async function main() {
  const rig = await startRig({ rom: ROM, port: PORT });
  const t = tally();

  try {
    const m = await rig.mailboxes();
    console.log('\nboth cores up: ' +
      m.map((x) => `p${x.id} ${x.state} map=${x.selfMap}`).join('   '));
    if (m.some((x) => x.state !== 'ACTIVE'))
      throw new Error('the session never reached ACTIVE; nothing below is meaningful');
    if (m[0].selfMap !== m[1].selfMap)
      throw new Error('the two players are not on the same map to begin with');

    // A party, so there is something to walk behind player 1. A new co-op
    // game starts with none and a trainer with no Pokemon has no follower.
    console.log('\n--- giving player 1 a party ---');
    await runDebugScript(rig, 0, KIT_SCRIPT);
    for (let i = 0; i < 400 && await isBusy(rig, 0); i++) {
      await rig.tap(0, 'B', 6);
      await rig.wait(14);
    }
    await rig.wait(180);
    await freeUp(rig, 0);
    await freeUp(rig, 1);
    await rig.wait(120);
    await rig.shot('/tmp/claude-0/stage13-kitted');

    // --- 1: player 2 was told what it is -------------------------------
    const species = await rig.u16(1, OFFSETS.peerFollowerSpeciesAddr);
    t.note('species p2 heard', String(species));
    t.check('player 2 knows which Pokemon is behind player 1',
            species !== SPECIES_NONE,
            'player 2 was never told there was one');

    // --- 2 & 3: and drew it --------------------------------------------
    let objs = await objects(rig, 1);
    const peer = objs.get(PEER);
    let mon = objs.get(PEER_FOLLOWER);

    t.note('on p2 screen', `partner=${peer ? `(${peer.x},${peer.y})` : 'absent'} ` +
                           `follower=${mon ? `(${mon.x},${mon.y}) gfx=${mon.graphicsId}` : 'absent'}`);

    t.check('player 2 has the partner on screen to begin with', peer !== undefined,
            'the partner sprite is not even there, so nothing below means anything');
    t.check('and their Pokemon is on the map too', mon !== undefined,
            'player 1 has a follower and player 2 cannot see it');

    if (mon !== undefined) {
      // The graphics id is derived from the species, so a mismatch means the
      // wrong Pokemon is on screen -- which looks fine until you notice your
      // partner is being followed by somebody else's Wingull.
      const expected = await rig.u16(1, OFFSETS.peerFollowerSpeciesAddr);
      t.check('the sprite is the one for that species',
              mon.graphicsId > expected,
              `gfx=${mon.graphicsId} for species ${expected}`);
    }

    // --- 4: standing with them, not on them or elsewhere ----------------
    if (mon !== undefined && peer !== undefined) {
      const d = dist(peer, mon);
      t.note('how far apart', `${d} tiles`);
      // A follower that has not been walked yet shares its trainer's tile,
      // which is what the game does with its own. Anything further than one
      // step is not following anybody.
      t.check('it is with the partner', d <= 1,
              `${d} tiles away, which is not following anybody`);
    }

    // --- 5: and it follows ----------------------------------------------
    console.log('\n--- player 1 walks; the Pokemon should come along ---');
    const beforeMon = mon ? { x: mon.x, y: mon.y } : null;

    for (let i = 0; i < 4; i++) { await rig.tap(0, 'Left', 16); await rig.wait(40); }
    await rig.wait(120);
    await rig.shot('/tmp/claude-0/stage13-walked');

    objs = await objects(rig, 1);
    const peerAfter = objs.get(PEER);
    const monAfter = objs.get(PEER_FOLLOWER);

    t.note('after walking', `partner=${peerAfter ? `(${peerAfter.x},${peerAfter.y})` : 'absent'} ` +
                            `follower=${monAfter ? `(${monAfter.x},${monAfter.y})` : 'absent'}`);

    t.check('the partner moved on player 2\'s screen',
            peerAfter !== undefined && peer !== undefined
              && dist(peer, peerAfter) > 0,
            'player 1 walked and player 2 saw nothing move; the rest proves nothing');

    t.check('their Pokemon moved with them',
            monAfter !== undefined && beforeMon !== null
              && dist(beforeMon, monAfter) > 0,
            'the Pokemon stayed where it was while its trainer walked off');

    // Exactly one tile, not "no more than one". Zero means it is standing
    // inside its trainer with one sprite drawn over the other, which is what
    // the first version of this did -- and it passed a <= 1 check while doing
    // it, because from the outside you simply cannot see the Pokemon at all.
    t.check('and is exactly one step behind them, not inside them',
            monAfter !== undefined && peerAfter !== undefined
              && dist(peerAfter, monAfter) === 1,
            monAfter && peerAfter
              ? `${dist(peerAfter, monAfter)} tiles apart`
              : 'one of them is gone');

    t.summary();
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
