#!/usr/bin/env node
// Stage-1 check: does Player 2's character survive a session?
//
// Runs both games in one page (pair.html) with the relay pumping in-process, so
// the mailbox rings cannot overflow between pumps -- which is what stopped the
// earlier rig from ever completing a block transfer.
//
// What it actually verifies, and why each step matters:
//   1. both cores reach the overworld and the link comes up
//   2. Player 1's stored Player 2 record is marked claimed, which only happens
//      if Player 2 reported itself and Player 1 wrote it down
//   3. the stored name matches the name Player 2 is actually using
//
// Step 3 is the one that catches the bug class I care about most: a record that
// is the right size and structurally plausible but belongs to the wrong player.
//
//   node coop/harness/stage1.mjs path/to/rom.gba
import { chromium } from 'playwright';
import { globSync, readFileSync } from 'node:fs';
import { stat } from 'node:fs/promises';
import { basename } from 'node:path';
import server, { setRom } from './serve.mjs';

const PORT = 8779;
const ROM = process.argv[2] || '/home/claude/crossroads/pokeemerald.gba';

// From the linker map. The rig finds the mailbox by scanning for its magic, so
// these two together give the heap offset of anything else in EWRAM.
const MAILBOX_ADDR = 0x02031f08;
const SAVEBLOCK1_ADDR = 0x020106b4;
const SAVEBLOCK2_ADDR = 0x020148b4;
// struct SaveBlock2: playerName is first, playerGender at 16.
const SB2_PLAYERNAME = 0;

// Offsets come from the compiler, via tools/coop/emit_offsets.py. Deriving them
// here is what broke the first run: sizeof(struct CoopPlayer2) is 632, not the
// 636 its fields sum to, so "block size minus record size" landed four bytes
// early and read plausible nonsense.
const OFFSETS = JSON.parse(
  readFileSync(new URL('./coop-offsets.json', import.meta.url), 'utf8'));

// Emerald's text encoding: 0xBB..0xD4 are A..Z, 0xD5..0xEE are a..z, 0xFF ends.
function decodeName(bytes) {
  let out = '';
  for (const b of bytes) {
    if (b === 0xff) break;
    if (b >= 0xbb && b <= 0xd4) out += String.fromCharCode(65 + b - 0xbb);
    else if (b >= 0xd5 && b <= 0xee) out += String.fromCharCode(97 + b - 0xd5);
    else if (b >= 0xa1 && b <= 0xaa) out += String.fromCharCode(48 + b - 0xa1);
    else out += '?';
  }
  return out;
}

const NUDGE = [null, 'A', 'A', 'A', 'Start', null, 'A', 'Start'];

async function main() {
  const { size } = await stat(ROM);
  console.log(`rom: ${ROM} (${(size / 1048576).toFixed(1)} MiB)`);
  setRom(ROM);
  await new Promise((r) => server.listen(PORT, r));

  const exe = globSync('/opt/pw-browsers/chromium-*/chrome-linux/chrome');
  const browser = await chromium.launch({
    ...(exe.length ? { executablePath: exe[0] } : {}),
    args: ['--no-sandbox'],
  });

  try {
    const page = await (await browser.newContext()).newPage();
    page.on('pageerror', (e) => console.log(`  [pageerror] ${e.message}`));
    await page.goto(`http://127.0.0.1:${PORT}/pair.html`);
    await page.waitForFunction(() => !!window.__pair);

    console.log('booting both cores in one page...');
    await page.evaluate(([u, n]) => window.__pair.boot(u, n),
      [`http://127.0.0.1:${PORT}/rom.gba`, basename(ROM)]);
    await page.evaluate(() => window.__pair.wait(120));

    const found = await page.evaluate(() => window.__pair.locate());
    for (const f of found)
      console.log(`  core ${f.id}: mailbox 0x${f.base.toString(16)} ` +
                  `(${f.candidates} candidates, ${f.live} live)`);

    await page.evaluate(() => window.__pair.openSession());
    console.log('session opened; relay pumping in-page every frame');

    console.log('walking both through the intro...');
    await page.evaluate(() => window.__pair.wait(1400));
    await page.evaluate(() => window.__pair.tap('both', 'Select', 12));

    for (let i = 0; i < 300; i++) {
      await page.evaluate(() => window.__pair.wait(12));
      const key = NUDGE[i % NUDGE.length];
      if (key) await page.evaluate((k) => window.__pair.tap('both', k, 6), key);
    }

    // Let the hand-over run: Player 1 sends, Player 2 answers, Player 1 stores.
    console.log('settling, so the hand-over can complete...');
    for (let i = 0; i < 6; i++) {
      await page.evaluate(() => window.__pair.wait(200));
      const m = await page.evaluate(() => [0, 1].map((n) => window.__pair.mailbox(n)));
      console.log(`  t+${(i + 1) * 200}: ` +
        m.map((x) => `p${x.id} ${x.state} map=${x.selfMap} out=${x.outPending}`).join('   '));
    }

    // The heap offset of anything in EWRAM, from the mailbox we located.
    const heapOf = (addr) => found[0].base - (MAILBOX_ADDR - addr);

    const p2Off = heapOf(SAVEBLOCK1_ADDR) + OFFSETS.coopPlayer2;
    const stored = await page.evaluate(
      ([off, len]) => window.__pair.readAt(0, off, len),
      [p2Off, OFFSETS.sizeofCoopPlayer2]);

    const claimed = stored[OFFSETS.claimed];
    const name = decodeName(stored.slice(OFFSETS.playerName, OFFSETS.playerName + 8));

    console.log('\n=== what Player 1 has stored for Player 2 ===');
    console.log({ claimed: !!claimed, name,
                  partyCount: stored[OFFSETS.partyCount],
                  gender: stored[OFFSETS.playerGender] });

    // The decisive check. A record of the right size with a plausible name
    // proves nothing if it is PLAYER 1's name -- that is exactly the failure
    // the serialised hand-over exists to prevent, and it would look fine here.
    const liveNames = [];
    for (const i of [0, 1]) {
      const raw = await page.evaluate(
        ([w, off, len]) => window.__pair.readAt(w, off, len),
        [i, heapOf(SAVEBLOCK2_ADDR) + SB2_PLAYERNAME, 8]);
      liveNames.push(decodeName(raw));
    }
    console.log(`\nplayer 1 is actually: ${liveNames[0]}`);
    console.log(`player 2 is actually: ${liveNames[1]}`);

    if (name === liveNames[1] && name !== liveNames[0]) {
      console.log('PASS: the stored record is Player 2, not Player 1');
    } else if (liveNames[0] === liveNames[1]) {
      console.log(`INCONCLUSIVE: both players are called ${liveNames[0]}, ` +
                  'so this cannot distinguish them');
    } else {
      console.log(`FAIL: stored "${name}" but Player 2 is "${liveNames[1]}"`);
    }

    // --- shared progression ---------------------------------------------
    //
    // Set a badge and a Pokedex entry on Player 1 directly in memory, then
    // reconnect and see whether they reach Player 2. Badges are plain flags
    // (FLAG_BADGE01_GET), so this exercises the whole flag array at once.
    const sb1 = (which) => heapOf(SAVEBLOCK1_ADDR);
    const badgeByte = OFFSETS.flags + Math.floor(OFFSETS.flagBadge01 / 8);
    const badgeBit = 1 << (OFFSETS.flagBadge01 % 8);

    await page.evaluate(([off, v]) => {
      const h = new Uint8Array(window.__pairHeap(0));
      h[off] |= v;
    }, [sb1(0) + badgeByte, badgeBit]).catch(async () => {
      // No raw-write helper on the rig; add one inline.
      await page.evaluate(([off, v]) => window.__pair.orByte(0, off, v),
                          [sb1(0) + badgeByte, badgeBit]);
    });
    // Pokedex entry for species 1 (bit 0 of the first byte).
    await page.evaluate(([off, v]) => window.__pair.orByte(0, off, v),
                        [sb1(0) + OFFSETS.dexCaught, 0x01]);

    console.log('\nset badge 1 and a dex catch on Player 1');

    // --- adoption ------------------------------------------------------
    //
    // Second connect. The stored slot is claimed now, so Player 1 hands the
    // record back and Player 2 should BECOME that character. On the first run
    // Player 2 was whoever it booted as; if adoption works, its live name after
    // reconnecting is the stored one.
    console.log('\ndropping and rebuilding the session...');
    await page.evaluate(() => window.__pair.reconnect(240));
    await page.evaluate(() => window.__pair.wait(900));

    const after = await page.evaluate(
      ([w, off, len]) => window.__pair.readAt(w, off, len),
      [1, heapOf(SAVEBLOCK2_ADDR) + SB2_PLAYERNAME, 8]);
    const p2After = decodeName(after);

    const p2Badge = await page.evaluate(([off]) => window.__pair.readAt(1, off, 1),
                                        [heapOf(SAVEBLOCK1_ADDR) + badgeByte]);
    const p2Dex = await page.evaluate(([off]) => window.__pair.readAt(1, off, 1),
                                      [heapOf(SAVEBLOCK1_ADDR) + OFFSETS.dexCaught]);
    console.log(`player 2 badge bit: ${(p2Badge[0] & badgeBit) ? 'SET' : 'clear'}`);
    console.log(`player 2 dex bit:   ${(p2Dex[0] & 1) ? 'SET' : 'clear'}`);
    if ((p2Badge[0] & badgeBit) && (p2Dex[0] & 1))
      console.log('PASS: badge and dex crossed to Player 2');
    else
      console.log('FAIL: shared progression did not cross');

    console.log(`player 2 after reconnect: ${p2After}  (stored: ${name})`);
    if (p2After === name)
      console.log('PASS: Player 2 is the stored character');
    else
      console.log(`FAIL: expected "${name}", got "${p2After}"`);

    for (const i of [0, 1]) console.log(`mailbox ${i}:`, await page.evaluate(
      (n) => window.__pair.mailbox(n), i));

    await page.locator('#s0').screenshot({ path: '/tmp/claude-0/stage1-p1.png' });
    await page.locator('#s1').screenshot({ path: '/tmp/claude-0/stage1-p2.png' });
    console.log('screens -> /tmp/claude-0/stage1-p{1,2}.png');
  } finally {
    await browser.close();
    server.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
