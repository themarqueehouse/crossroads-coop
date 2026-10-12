#!/usr/bin/env node
// Stage-17 check: the co-op starter screen.
//
// The starter choice used to be two text menus -- a region, then one of its
// three -- which named the Pokemon and showed nothing. It is now the game's own
// starter screen with all nine regions paged behind it, which means new C on
// the path every fresh game takes. If it hangs, a session is dead before it
// starts, and nothing else in this harness goes near it: the rig answers the
// first-run prompt and then skips the opening outright, so Birch's bag is never
// reached.
//
// Driven through debug script 27, which runs the same two commands the real
// scene does. What is worth establishing:
//
//   1. the screen comes up at all, rather than the script locking on waitstate
//   2. up and down page between regions -- the whole point of the change
//   3. confirming hands back the species that was on screen, not the slot
//   4. a Pokemon of that species actually lands in the party
//   5. the script comes back and the player has control again
//
// Sinnoh, by paging once from Hoenn, because a Turtwig proves the page turned
// and a Treecko would not.
//
//   node coop/harness/stage17.mjs path/to/rom.gba
import { startRig, tally, OFFSETS, runDebugScriptOn } from './rig.mjs';

const PORT = 8805;
const ROM = process.argv[2] || '/home/claude/crossroads/pokeemerald.gba';

// src/starter_choose.c, sStarterMon_AllRegions. Hoenn is the page the screen
// opens on, Sinnoh the next one down.
//
// A whole page rather than one Pokemon, because the screen opens with the
// MIDDLE of the three under the cursor, not the first -- which is how the first
// run of this reported a failure while the page had turned perfectly and handed
// back a Chimchar. What matters is that the species came from the next region,
// not which of its three the cursor happened to be resting on.
const HOENN = [252, 255, 258];      // Treecko, Torchic, Mudkip
const SINNOH = [387, 390, 393];     // Turtwig, Chimchar, Piplup

const isBusy = async (rig, w) =>
  (await rig.mailbox(w)).flags.includes('SCRIPT_BUSY');

// VAR_TEMP_E, which is where the screen leaves the species it was told to give.
// Read rather than decrypting a party slot: the party stores its species inside
// an encrypted substruct, and the question here is what the screen chose.
async function chosenSpecies(rig, w) {
  const { saveBlock1Addr, sb1Vars, varTempEIndex } = OFFSETS;
  return rig.u16(w, saveBlock1Addr + sb1Vars + varTempEIndex * 2);
}

// How many Pokemon are in the party, counted by max HP as the other stages do.
async function partySize(rig, w) {
  const { partiesAddr, sizeofPokemon, monMaxHp, monLevel, partySize: cap } = OFFSETS;
  const out = [];
  for (let i = 0; i < cap; i++) {
    const base = partiesAddr + i * sizeofPokemon;
    const maxHp = await rig.u16(w, base + monMaxHp);
    if (maxHp === 0) break;
    out.push({ maxHp, level: await rig.u8(w, base + monLevel) });
  }
  return out;
}

async function main() {
  const rig = await startRig({ rom: ROM, port: PORT });
  const t = tally();

  try {
    if ((await rig.mailboxes()).some((x) => x.state !== 'ACTIVE'))
      throw new Error('the session never reached ACTIVE');

    const before = await partySize(rig, 0);
    t.note('party before', `${before.length} Pokemon`);

    // ------------------------------------------------- the screen comes up
    await runDebugScriptOn(rig, 0, 27, { dismiss: false });
    await rig.wait(240);

    const camedUp = await isBusy(rig, 0);
    t.check('the starter screen opens', camedUp,
            'the script is not running -- waitstate never got a screen to wait for');
    await rig.shot('/tmp/claude-0/stage17-hoenn');

    // ------------------------------------------------- paging
    //
    // One press down: Hoenn to Sinnoh. Then A on the leftmost of the three,
    // which is Turtwig, and A again on the YES of the confirmation.
    await rig.tap(0, 'Down', 8);
    await rig.wait(90);
    await rig.shot('/tmp/claude-0/stage17-sinnoh');

    await rig.tap(0, 'A', 8);
    await rig.wait(180);          // the ball opens and the sprite flies in
    await rig.tap(0, 'A', 8);
    await rig.wait(240);          // confirm, then back to the field

    // ------------------------------------------------- what we ended up with
    const species = await chosenSpecies(rig, 0);
    const after = await partySize(rig, 0);
    t.note('species handed back', `${species}`);
    t.note('party after', after.map((m) => `L${m.level}`).join(' ') || 'empty');
    await rig.shot('/tmp/claude-0/stage17-after');

    t.check('paging reached another region', SINNOH.includes(species),
            HOENN.includes(species)
              ? 'still on the Hoenn page -- down did not turn it'
              : `expected one of Sinnoh's three (${SINNOH}), got ${species}`);

    t.check('a Pokemon arrived', after.length === before.length + 1,
            `party went from ${before.length} to ${after.length}`);

    t.check('and it came at level 5', after.length > before.length
            && after[after.length - 1].level === 5,
            `the new one is level ${after.length ? after[after.length - 1].level : '-'}`);

    // ------------------------------------------------- the script let go
    //
    // The first version of the co-op give-starter script ended one line short
    // of finishing and left the player locked for ever, so this is worth
    // asking every time.
    let free = false;
    for (let i = 0; i < 20 && !free; i++) {
      free = !(await isBusy(rig, 0));
      if (!free) await rig.wait(30);
    }
    t.check('the player has control again', free,
            'the script never ended -- the console is locked on the field');

    t.summary();
  } finally {
    await rig.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
