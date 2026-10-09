#!/usr/bin/env node
// Load the mid-game save and check it is what it claims to be.
//
// Handing somebody a save file that does not load, or loads into an empty
// bedroom, wastes their evening rather than mine. The file having the right
// size and the right sector signatures says it is a save; it does not say it
// is THIS save. So: boot the ROM with it on the chip, press Continue, and look
// at the badges and the party.
//
//   node coop/harness/checksave.mjs [save.sav]
import { readFileSync } from 'node:fs';
import { startRig, tally, OFFSETS } from './rig.mjs';

const PORT = 8807;
const ROM = '/home/claude/crossroads/pokeemerald.gba';
const SAVE = process.argv[2] || '/home/claude/crossroads/coop/crossroads-midgame.sav';

const NUDGE = [null, 'A', 'A', 'A', 'Start', null, 'A', 'Start'];

async function main() {
  const saveB64 = readFileSync(SAVE).toString('base64');
  console.log(`save: ${SAVE} (${readFileSync(SAVE).length} bytes)`);

  // introLoops 0: the save means there is a Continue to press rather than a
  // new game to sit through, and mashing past it would start a new one.
  const rig = await startRig({ rom: ROM, port: PORT, saveB64,
                               introLoops: 0, settle: 0 });
  const t = tally();

  try {
    // Copyright screen, title, then Continue.
    for (let i = 0; i < 90; i++) {
      await rig.wait(12);
      const key = NUDGE[i % NUDGE.length];
      if (key) await rig.tap('both', key, 6);
    }
    await rig.wait(600);
    for (let i = 0; i < 10; i++) { await rig.tap('both', 'B', 6); await rig.wait(20); }
    await rig.wait(240);
    await rig.shot('/tmp/claude-0/checksave');

    const m = await rig.mailboxes();
    t.note('maps', m.map((x) => `p${x.id} ${x.selfMap}`).join('  '));
    t.check('the save loaded into the world',
            (await rig.mailbox(0)).selfMap !== '0x0',
            'never reached the overworld -- the save may not have loaded');

    // Badges are plain flags in SaveBlock1.
    const flagsAddr = OFFSETS.saveBlock1Addr + OFFSETS.flags;
    const badgeBits = [];
    for (let i = 0; i < 8; i++) {
      const id = OFFSETS.flagBadge01 + i;
      const byte = await rig.u8(0, flagsAddr + (id >> 3));
      badgeBits.push((byte >> (id & 7)) & 1);
    }
    const badges = badgeBits.reduce((a, b) => a + b, 0);
    t.note('badges', `${badges} of 8`);
    t.check('all eight badges are set', badges === 8);

    // gPartiesCount[B_TRAINER_0] is this console's own party size. There is no
    // separate gPlayerPartyCount in this expansion -- the parties are one
    // array indexed by battler role.
    const partyCount = await rig.u8(0, OFFSETS.partiesCountAddr);
    t.note('party', `${partyCount} Pokemon`);
    t.check('the team is there', partyCount === 6, `only ${partyCount}`);

    // Who does the save think Player 2 is?
    //
    // This is the field that decides whether a joining player makes a
    // character or gets one back. A save shipped with it already set hands
    // the next person somebody else's character and overwrites the name and
    // gender they just chose.
    const rec = OFFSETS.saveBlock1Addr + OFFSETS.coopPlayer2;
    const claimed = await rig.u8(0, rec + OFFSETS.claimed);
    const p2Party = await rig.u8(0, rec + OFFSETS.partyCount);
    const nameBytes = await rig.readAt(0, rec + OFFSETS.playerName, 8);
    t.note('player 2 record', `claimed=${claimed} party=${p2Party} ` +
           `nameBytes=[${[...nameBytes].join(',')}]`);
    t.check('player 2 is unclaimed, so a joiner keeps their own character',
            claimed === 0,
            'the save already has a player 2 -- joining overwrites their name and gender');

    t.summary();
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
