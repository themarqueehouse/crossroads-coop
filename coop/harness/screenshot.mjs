#!/usr/bin/env node
// Boot one ROM and capture the screen at intervals.
//
// When the two-player rig reports the game never left the title screen, the
// fastest way to find out why is to look at it. Guessing from the mailbox alone
// cannot distinguish "quickstart didn't fire" from "stuck on a save-error
// screen" from "still in the Game Freak intro".
//
//   node coop/harness/screenshot.mjs path/to/rom.gba [outDir]
import { chromium } from 'playwright';
import { globSync, mkdirSync } from 'node:fs';
import { stat } from 'node:fs/promises';
import { basename, join } from 'node:path';
import server, { setRom } from './serve.mjs';

const PORT = 8778;
const ROM = process.argv[2];
const OUT = process.argv[3] || '/tmp/claude-0/shots';

// Frame counts to capture at, and what to press just before each.
const PLAN = [
  { at: 1450, press: null, note: 'title up' },
  { at: 1500, press: 'Select', note: 'SELECT 1' },
  { at: 1560, press: null, note: 'after SELECT 1' },
  { at: 1620, press: 'Select', note: 'SELECT 2' },
  { at: 1700, press: null, note: 'after SELECT 2' },
  { at: 1800, press: 'Start', note: 'START' },
  { at: 1900, press: null, note: 'after START' },
  { at: 2000, press: 'A', note: 'A' },
  { at: 2150, press: null, note: 'after A' },
];

async function main() {
  if (!ROM) { console.error('usage: screenshot.mjs <rom> [outDir]'); process.exit(2); }
  mkdirSync(OUT, { recursive: true });
  const { size } = await stat(ROM);
  console.log(`rom: ${ROM} (${(size / 1048576).toFixed(1)} MiB) -> ${OUT}`);

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
    page.on('console', (m) => { if (m.type() === 'error') console.log(`  [console] ${m.text()}`); });
    await page.goto(`http://127.0.0.1:${PORT}/rig.html`);
    await page.waitForFunction(() => !!window.__rig);
    await page.evaluate(
      ([url, name]) => window.__rig.boot(url, name),
      [`http://127.0.0.1:${PORT}/rom.gba`, basename(ROM)]);

    const canvas = page.locator('#screen');
    for (const step of PLAN) {
      await page.evaluate((n) => window.__rig.waitFrames(n), 60).catch(() => {});
      await page.evaluate((f) => {
        // waitFrames is relative; drive to the absolute mark instead.
        return window.__rig.frames() >= f ? null : window.__rig.waitFrames(f - window.__rig.frames());
      }, step.at);
      if (step.press) await page.evaluate((k) => window.__rig.tap(k, 10), step.press);
      const file = join(OUT, `f${String(step.at).padStart(5, '0')}-${step.note.replace(/\W+/g, '_')}.png`);
      await canvas.screenshot({ path: file });
      console.log(`  frame ${step.at}: ${step.note} -> ${basename(file)}`);
    }
  } finally {
    await browser.close();
    server.close();
  }
}

main().catch((e) => { console.error('FAILED:', e.message); process.exit(1); });
