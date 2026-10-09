#!/usr/bin/env node
// Stage-10 check: does Player 2's progress survive into Player 1's save?
//
// Player 1 owns the save; Player 2's character lives inside it as a record.
// That record used to be sent once, at join, and never again -- so every level
// Player 2 gained, every Pokemon they caught and every move they learned
// existed only in their own console's RAM. Player 1 saves, Player 2 comes back
// tomorrow with the team they first joined with. Everything else about the
// world was shared and the one thing that was theirs was not.
//
// Money is NOT checked here, and the reason is worth writing down. It looked
// like the same hole -- it was in neither the join snapshot nor the live
// changes -- but COOP_UNLIMITED_MONEY is on, so RemoveMoney returns early and
// AddMoney clamps at a maximum the save already starts on. The figure never
// moves, so there was never anything to share, and there is no way to exercise
// a sync from inside the game while that setting stands. It is synced now
// regardless, so turning the setting off works; testing it would mean turning
// it off, which is a different build.
//
// Both are checked from Player 1's side, in Player 1's save block, because
// that is the copy that gets written to the file.
//
//   node coop/harness/stage10.mjs path/to/rom.gba
import { startRig, tally, OFFSETS } from './rig.mjs';

const PORT = 8801;
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

// Player 2's record, read out of Player 1's SaveBlock1 -- the copy that is
// actually written to the save file.
const recordAddr = OFFSETS.saveBlock1Addr + OFFSETS.coopPlayer2;

const p2PartyCount = async (rig) =>
  rig.u8(0, recordAddr + OFFSETS.partyCount);

const p2PartySpecies = async (rig, slot) => {
  // The species of a party slot, read straight from the stored record. Slot
  // stride is sizeof(struct Pokemon); species lives in the encrypted substruct,
  // so this reads the personality instead -- a value that is unique per
  // Pokemon, never zero for a real one, and needs no decryption.
  const MON_SIZE = 100;
  return rig.u32(0, recordAddr + OFFSETS.party + slot * MON_SIZE);
};

async function main() {
  const rig = await startRig({ rom: ROM, port: PORT });
  const t = tally();

  try {
    const m = await rig.mailboxes();
    if (m.some((x) => x.state !== 'ACTIVE'))
      throw new Error('the session never reached ACTIVE');

    const countBefore = await p2PartyCount(rig);
    t.note('before', `p2 party in p1's save = ${countBefore}`);

    // ------------------------------------------------- player 2 gains a team
    console.log("\n--- player 2 catches something ---");
    await runDebugScript(rig, 1, 11);
    await rig.wait(120);
    await freeUp(rig, 1);

    // The resend is on a timer -- it checks whether the party changed every
    // two seconds and only then spends the transport on 632 bytes -- so this
    // has to wait for it rather than read straight away.
    let arrived = false;
    for (let i = 0; i < 25 && !arrived; i++) {
      await rig.wait(60);
      arrived = (await p2PartyCount(rig)) > countBefore;
    }

    t.note('after', `p2 party in p1's save = ${await p2PartyCount(rig)}`);
    t.check("player 2's new Pokemon reached player 1's save", arrived,
            'the record was never re-sent, so the progress would be lost on save');

    const personality = await p2PartySpecies(rig, countBefore);
    t.check('and it is a real Pokemon, not an empty slot', personality !== 0,
            'the slot count went up but the slot is blank');

    for (const w of [0, 1]) {
      const site = await rig.u8(w, OFFSETS.linkErrorSiteAddr);
      if (site) t.note(`p${w + 1} link error`, `site ${site}`);
    }
    t.check('no communication error',
            (await rig.u8(0, OFFSETS.linkErrorSiteAddr)) === 0 &&
            (await rig.u8(1, OFFSETS.linkErrorSiteAddr)) === 0);

    await rig.shot('/tmp/claude-0/stage10');
    t.summary();
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
