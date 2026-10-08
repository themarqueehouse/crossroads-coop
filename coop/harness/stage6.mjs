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

const slot0Personality = async (rig, w) => {
  const b = await rig.readAt(w, OFFSETS.storageAddr + BOXES_OFFSET, 4);
  return (b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)) >>> 0;
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

    await rig.shot('/tmp/claude-0/stage6-boxes');
    process.exitCode = t.summary() ? 0 : 1;
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
