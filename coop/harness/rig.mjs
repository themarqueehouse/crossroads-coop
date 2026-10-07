// Shared setup for the co-op checks: boot two cores in one page, get both to
// the overworld, open the session, and hand back a few helpers.
//
// Pulled out of stage1 when stage2 arrived, rather than copied. The parts worth
// not having two copies of are the EWRAM addressing and the intro walk: the
// first is the thing that silently reads plausible garbage when it drifts, and
// the second took several goes to make reliable.
import { chromium } from 'playwright';
import { globSync, readFileSync } from 'node:fs';
import { stat } from 'node:fs/promises';
import { basename } from 'node:path';
import server, { setRom } from './serve.mjs';

// Offsets AND EWRAM addresses, both from the build: the struct offsets via the
// compiler-generated probe, the addresses from the ELF symbol table.
//
// stage1 used to carry the addresses as three hex constants copied out of the
// linker map. They were stale by the time this was written -- the mailbox had
// moved 7 KB -- and a stale EWRAM address does not fail, it reads zeroes that
// look like a feature not working. Nothing here is hand-copied now.
export const OFFSETS = JSON.parse(
  readFileSync(new URL('./coop-offsets.json', import.meta.url), 'utf8'));

// Emerald's text encoding: 0xBB..0xD4 are A..Z, 0xD5..0xEE are a..z, 0xFF ends.
export function decodeName(bytes) {
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

// The button taps that get a fresh game from the title screen to the overworld.
// Not a scripted sequence of exact presses: the intro has a naming screen and
// several waits whose timing moves, so this just mashes a rotation that covers
// every prompt and lets the repetition do the work.
const NUDGE = [null, 'A', 'A', 'A', 'Start', null, 'A', 'Start'];

export async function startRig({ rom, port, introLoops = 300, settle = 1200 }) {
  const { size } = await stat(rom);
  console.log(`rom: ${rom} (${(size / 1048576).toFixed(1)} MiB)`);
  setRom(rom);
  await new Promise((r) => server.listen(port, r));

  const exe = globSync('/opt/pw-browsers/chromium-*/chrome-linux/chrome');
  const browser = await chromium.launch({
    ...(exe.length ? { executablePath: exe[0] } : {}),
    args: ['--no-sandbox'],
  });

  const page = await (await browser.newContext()).newPage();
  page.on('pageerror', (e) => console.log(`  [pageerror] ${e.message}`));
  await page.goto(`http://127.0.0.1:${port}/pair.html`);
  await page.waitForFunction(() => !!window.__pair);

  console.log('booting both cores in one page...');
  await page.evaluate(([u, n]) => window.__pair.boot(u, n),
    [`http://127.0.0.1:${port}/rom.gba`, basename(rom)]);
  await page.evaluate(() => window.__pair.wait(120));

  const found = await page.evaluate(() => window.__pair.locate());
  for (const f of found)
    console.log(`  core ${f.id}: mailbox 0x${f.base.toString(16)} ` +
                `(${f.candidates} candidates, ${f.live} live)`);
  if (found.some((f) => f.base === null))
    throw new Error('a core never published a live mailbox');

  await page.evaluate(() => window.__pair.openSession());
  console.log('session opened; relay pumping in-page every frame');

  console.log('walking both through the intro...');
  await page.evaluate(() => window.__pair.wait(1400));
  await page.evaluate(() => window.__pair.tap('both', 'Select', 12));
  for (let i = 0; i < introLoops; i++) {
    await page.evaluate(() => window.__pair.wait(12));
    const key = NUDGE[i % NUDGE.length];
    if (key) await page.evaluate((k) => window.__pair.tap('both', k, 6), key);
  }
  if (settle) await page.evaluate((n) => window.__pair.wait(n), settle);

  // Every core's mailbox sits at the same EWRAM address; the heap offset it
  // landed at differs per core, because each has its own WASM heap.
  const heapOf = (which, addr) =>
    found[which].base - (OFFSETS.mailboxAddr - addr);

  const api = {
    page,
    found,
    heapOf,
    wait: (n) => page.evaluate((x) => window.__pair.wait(x), n),
    tap: (who, key, hold = 6) =>
      page.evaluate(([w, k, h]) => window.__pair.tap(w, k, h), [who, key, hold]),
    mailbox: (which) => page.evaluate((n) => window.__pair.mailbox(n), which),
    mailboxes: () => page.evaluate(() => [0, 1].map((n) => window.__pair.mailbox(n))),
    reconnect: (gap = 240) => page.evaluate((g) => window.__pair.reconnect(g), gap),

    read: (which, addr, len) =>
      page.evaluate(([w, a, l]) => window.__pair.readAt(w, a, l), [which, addr, len]),
    write: (which, addr, bytes) =>
      page.evaluate(([w, a, b]) => window.__pair.writeAt(w, a, b), [which, addr, bytes]),
    orByte: (which, addr, v) =>
      page.evaluate(([w, a, x]) => window.__pair.orByte(w, a, x), [which, addr, v]),

    // The same, addressed by EWRAM address rather than heap offset.
    readAt: (which, addr, len) => api.read(which, heapOf(which, addr), len),
    writeAt: (which, addr, bytes) => api.write(which, heapOf(which, addr), bytes),

    async u8(which, addr) { return (await api.readAt(which, addr, 1))[0]; },
    async u16(which, addr) {
      const b = await api.readAt(which, addr, 2);
      return b[0] | (b[1] << 8);
    },
    async u32(which, addr) {
      const b = await api.readAt(which, addr, 4);
      return (b[0] | (b[1] << 8) | (b[2] << 16) | (b[3] << 24)) >>> 0;
    },
    setU8: (which, addr, v) => api.writeAt(which, addr, [v & 0xff]),
    setU16: (which, addr, v) => api.writeAt(which, addr, [v & 0xff, (v >> 8) & 0xff]),

    async shot(prefix) {
      await page.locator('#s0').screenshot({ path: `${prefix}-p1.png` });
      await page.locator('#s1').screenshot({ path: `${prefix}-p2.png` });
      console.log(`screens -> ${prefix}-p{1,2}.png`);
    },

    async close() {
      await browser.close();
      server.close();
    },
  };

  return api;
}

// Tiny result tally, so a run ends with a verdict rather than a wall of prose
// the reader has to grade themselves.
export function tally() {
  const results = [];
  return {
    check(name, ok, detail = '') {
      results.push(ok);
      console.log(`${ok ? 'PASS' : 'FAIL'}: ${name}${detail ? ` -- ${detail}` : ''}`);
    },
    note(name, detail) {
      console.log(`      ${name}${detail ? `: ${detail}` : ''}`);
    },
    summary() {
      const bad = results.filter((r) => !r).length;
      console.log(`\n${results.length - bad}/${results.length} checks passed`);
      return bad === 0;
    },
  };
}
