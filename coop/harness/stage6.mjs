#!/usr/bin/env node
// Stage-6 check: are the storage boxes shared?
//
// Player 2 has no save file, so anything they box is lost -- and worse, their
// view of the boxes drifts from Player 1's, so depositing into a slot that
// looks empty can overwrite a Pokemon they cannot see. This checks that a
// Pokemon boxed on one console turns up in the other console's box.
//
// Script 5 fills the party; Script 8 then gives one more, which the game sends
// straight to the PC because there is nowhere else for it to go.
//
//   node coop/harness/stage6.mjs path/to/rom.gba
import { startRig, tally, OFFSETS } from './rig.mjs';

const PORT = 8797;
const ROM = process.argv[2] || '/home/claude/crossroads/pokeemerald.gba';
const SCRIPTS_MENU_INDEX = 5;

// struct PokemonStorage: currentBox, then boxes[][]. A BoxPokemon starts with
// personality and otId, so a non-zero personality means the slot is occupied.
const BOXES_OFFSET = 1;

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
  await rig.hold(w, 'R'); await rig.wait(6);
  await rig.tap(w, 'Start', 8); await rig.wait(20);
  await rig.letGo(w, 'R'); await rig.wait(20);
  for (let i = 0; i < SCRIPTS_MENU_INDEX; i++) { await rig.tap(w, 'Down', 6); await rig.wait(8); }
  await rig.tap(w, 'A', 8); await rig.wait(25);
  for (let i = 1; i < slot; i++) { await rig.tap(w, 'Down', 6); await rig.wait(8); }
  await rig.tap(w, 'A', 8); await rig.wait(40);
  await freeUp(rig, w);
}

const slot0Personality = async (rig, w) => personalityAt(rig, w, 0);

// A BoxPokemon's personality, by slot. Non-zero means the slot is occupied,
// and the value identifies which Pokemon it is -- so comparing the two
// consoles slot by slot says whether the boxes really match.
const BOX_MON_SIZE = 80;
const personalityAt = async (rig, w, slot) => {
  const b = await rig.readAt(w, OFFSETS.storageAddr + BOXES_OFFSET + slot * BOX_MON_SIZE, 4);
  return (b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)) >>> 0;
};

const boxSlots = async (rig, w, n) => {
  const out = [];
  for (let i = 0; i < n; i++) out.push(await personalityAt(rig, w, i));
  return out;
};

async function main() {
  const rig = await startRig({ rom: ROM, port: PORT });
  const t = tally();

  try {
    if ((await rig.mailboxes()).some((x) => x.state !== 'ACTIVE'))
      throw new Error('the session never reached ACTIVE');

    t.check('both boxes start empty',
            (await slot0Personality(rig, 0)) === 0 &&
            (await slot0Personality(rig, 1)) === 0);

    console.log('\n--- filling player 1\'s party, then boxing one more ---');
    await runDebugScript(rig, 0, 5);
    await rig.wait(90);
    await runDebugScript(rig, 0, 8);
    await rig.wait(300);

    const host = await slot0Personality(rig, 0);
    const guest = await slot0Personality(rig, 1);
    t.note('box 1 slot 1', `p1=0x${host.toString(16)}  p2=0x${guest.toString(16)}`);

    t.check('the Pokemon went into player 1\'s box', host !== 0);
    t.check('and into player 2\'s box too', guest !== 0,
            'the boxes are not shared -- anything player 2 stores is lost');
    t.check('both consoles hold the SAME Pokemon', host === guest,
            'the boxes diverged, which is how a deposit overwrites one you cannot see');

    // ------------------------------------------- a burst of box changes
    //
    // Moving a handful of Pokemon around in a box makes a burst of writes,
    // and the queue that carries them to the other console used to hold four
    // and throw the oldest away when it was full. Nobody was told. The
    // partner's box was then missing Pokemon it had no way of knowing about,
    // and the next deposit into one of those slots landed on top of one --
    // which is three of the bug reports against this hack.
    //
    // Twenty at once, which is well past any queue, and then both consoles
    // have to agree about all twenty.
    console.log('\n--- twenty at once, which used to be too many ---');
    await runDebugScript(rig, 0, 26);
    await rig.wait(1800);

    const mine = await boxSlots(rig, 0, 20);
    let theirs = await boxSlots(rig, 1, 20);
    for (let i = 0; i < 20 && theirs.some((v, n) => v !== mine[n]); i++) {
      await rig.wait(120);
      theirs = await boxSlots(rig, 1, 20);
    }

    const filled = mine.filter((v) => v !== 0).length;
    const matched = mine.filter((v, i) => v !== 0 && v === theirs[i]).length;
    t.note('box 1 after the burst',
           `player 1 holds ${filled}, player 2 agrees about ${matched}`);
    t.note('overflows', `${await rig.u16(0, OFFSETS.dbgBoxOverflowsAddr)} changes refused for want of room`);

    t.check('all twenty arrived on player 1', filled === 20,
            `only ${filled} of 20 went into the box at all`);
    t.check('and player 2 has every one of them', matched === filled,
            `player 2 is missing ${filled - matched} of them -- those slots look ` +
            'empty to them, and the next thing deposited there overwrites one');

    await rig.shot('/tmp/claude-0/stage6-boxes');
    process.exitCode = t.summary() ? 0 : 1;
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
