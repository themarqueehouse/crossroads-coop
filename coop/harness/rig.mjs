// Shared setup for the co-op checks: boot two cores in one page, get both to
// the overworld, open the session, and hand back a few helpers.
//
// Pulled out of stage1 when stage2 arrived, rather than copied. The parts worth
// not having two copies of are the EWRAM addressing and the intro walk: the
// first is the thing that silently reads plausible garbage when it drifts, and
// the second took several goes to make reliable.
import { chromium } from 'playwright';
import { globSync, readFileSync, statSync } from 'node:fs';
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
const OFFSETS_PATH = new URL('./coop-offsets.json', import.meta.url);
export const OFFSETS = JSON.parse(readFileSync(OFFSETS_PATH, 'utf8'));

// Refuse to run against offsets older than the ROM.
//
// Every EWRAM address in here moves when anything before it in the build does,
// and a stale one does not fail -- it reads zeroes, or someone else's variable,
// which looks exactly like the feature under test not working. That is not a
// hypothetical: regenerating this was forgotten after one rebuild and a suite
// that had passed 17/17 came back 5/17, with twelve plausible, detailed and
// entirely fictional failures.
function assertOffsetsFresh(rom) {
  const romTime = statSync(rom).mtimeMs;
  const offTime = statSync(OFFSETS_PATH).mtimeMs;
  if (offTime >= romTime) return;
  throw new Error(
    `coop-offsets.json is older than ${rom}.\n` +
    '  Every address in it may have moved. Run:\n' +
    '      python3 tools/coop/emit_offsets.py coop/harness/coop-offsets.json');
}

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


// Walk one console through the first-run prompts: name, gender, region,
// starter. Shared with stage 14, which drives it deliberately rather than
// getting it out of the way.
//
// Start on the naming screen is its "done" shortcut, which keeps the default
// name rather than spelling one out a key at a time.
const CONTEXT_WAITING = 1;   // script.c: a menu or a prompt owns the script

export async function answerFirstRunPrompt(rig, w, region = 0, starter = 0,
                                           girl = false) {
  const status = () => rig.u8(w, OFFSETS.dbgScriptStatusAddr);

  // Wait for the next menu to actually be on screen.
  //
  // Driving this by fixed waits does not work and fails quietly when it
  // doesn't: a press sent a few frames before a menu opens goes to whatever
  // was there instead, and every press after it is one step out. The symptom
  // was both players coming out of the gender question as girls -- the Down
  // meant for "Girl" had landed on the message box before it, and the A
  // meant for the menu had opened it.
  //
  // The script is WAITING for exactly as long as a menu or the naming screen
  // has it, and RUNNING in between, so the edge between the two is the thing
  // to wait for rather than a number of frames.
  const untilNextMenu = async () => {
    for (let i = 0; i < 80 && (await status()) === CONTEXT_WAITING; i++)
      await rig.wait(10);
    for (let i = 0; i < 80; i++) {
      if ((await status()) === CONTEXT_WAITING) return;
      await rig.wait(10);
    }
    throw new Error(`core ${w}: the next first-run menu never opened`);
  };

  const pick = async (n) => {
    for (let i = 0; i < n; i++) { await rig.tap(w, 'Down', 6); await rig.wait(16); }
    await rig.tap(w, 'A', 8);
    await rig.wait(30);
  };

  const naming = () => rig.u8(w, OFFSETS.firstRunNamingAddr);
  const until = async (want, what) => {
    for (let i = 0; i < 80; i++) {
      if ((await naming()) === want) return;
      await rig.wait(10);
    }
    throw new Error(`core ${w}: ${what}`);
  };

  await rig.tap(w, 'A', 8);     // the welcome box
  await until(1, 'the naming screen never opened');

  // START is the keyboard's OK, and on an empty field it moves the cursor
  // there rather than pressing it, so A finishes the job. Alternating until
  // the screen reports itself gone beats guessing which of the two it wants.
  for (let i = 0; i < 12 && (await naming()) === 1; i++) {
    await rig.tap(w, i % 2 === 0 ? 'Start' : 'A', 8);
    await rig.wait(40);
  }
  await until(0, 'the naming screen would not close');

  await untilNextMenu();
  await pick(girl ? 1 : 0);     // Boy / Girl

  await untilNextMenu();
  await pick(region);           // whose starters

  await untilNextMenu();
  await pick(starter);          // which of its three

  // Press on until the prompt reports itself finished rather than a fixed
  // number of times: the closing message is two pages today and a counted
  // run that is one short leaves the console standing in a message box with
  // its controls locked, which nothing later can recover from.
  for (let i = 0; i < 25; i++) {
    if ((await rig.u8(w, OFFSETS.firstRunRunningAddr)) === 0)
      return;
    await rig.tap(w, 'A', 8);
    await rig.wait(40);
  }
  throw new Error(`core ${w}: the first-run prompt would not finish`);
}

export async function startRig({ rom, port, introLoops = 300, settle = 1200,
                                 paired = true, saveB64 = null,
                                 answerFirstRun = true }) {
  assertOffsetsFresh(rom);
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
  await page.evaluate(([u, n, s]) => window.__pair.boot(u, n, s),
    [`http://127.0.0.1:${port}/rom.gba`, basename(rom), saveB64]);
  await page.evaluate(() => window.__pair.wait(120));

  const found = await page.evaluate(() => window.__pair.locate());
  for (const f of found)
    console.log(`  core ${f.id}: mailbox 0x${f.base.toString(16)} ` +
                `(${f.candidates} candidates, ${f.live} live)`);
  if (found.some((f) => f.base === null))
    throw new Error('a core never published a live mailbox');

  await page.evaluate((p) => window.__pair.openSession(p), paired);
  console.log('session opened; relay pumping in-page every frame');

  // Press through the copyright screen, the title and the main menu -- and
  // stop the moment both consoles are standing in the world.
  //
  // This used to mash A for a fixed three hundred loops and then press B
  // eight times to back out of whatever the last press had opened. That was
  // the right shape when the console went through the van and Birch's speech;
  // now both skip the opening, reach the overworld in a few presses, and the
  // remaining two hundred and ninety A presses land on whatever is in front
  // of them. Once a fresh game started asking the player their name, those
  // presses were answering it -- typing a name out of the keyboard one letter
  // at a time and picking a starter at random, before any test had looked.
  //
  // selfMap is published from the overworld and nowhere else, so it is zero
  // for exactly as long as the game has not handed over control.
  console.log('walking both to the overworld...');
  await page.evaluate(() => window.__pair.wait(1400));
  await page.evaluate(() => window.__pair.tap('both', 'Select', 12));

  let reached = false;
  for (let i = 0; i < introLoops && !reached; i++) {
    const maps = await page.evaluate(() => [0, 1].map((n) => window.__pair.mailbox(n).selfMap));
    reached = maps.every((m) => parseInt(m, 16) !== 0);
    if (reached) break;

    await page.evaluate(() => window.__pair.wait(12));
    const key = NUDGE[i % NUDGE.length];
    if (key) await page.evaluate((k) => window.__pair.tap('both', k, 6), key);
  }
  if (introLoops && !reached)
    throw new Error('a console never reached the overworld');

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
    gateLog: (which) => page.evaluate((n) => window.__pair.gateLog(n), which),
    clearGateLog: () => page.evaluate(() => window.__pair.clearGateLog()),
    hold: (which, key) =>
      page.evaluate(([w, k]) => window.__pair.hold(w, k), [which, key]),
    letGo: (which, key) =>
      page.evaluate(([w, k]) => window.__pair.letGo(w, k), [which, key]),
    mailboxes: () => page.evaluate(() => [0, 1].map((n) => window.__pair.mailbox(n))),
    reconnect: (gap = 240) => page.evaluate((g) => window.__pair.reconnect(g), gap),
    pairUp: () => page.evaluate(() => window.__pair.pairUp()),

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

  // Get both consoles out of whatever the intro left open, then answer the
  // opening questions.
  //
  // The intro is got through by mashing A, which does not stop at the
  // overworld: the last press lands on a television, a sign or a menu. The
  // eight B presses above are a guess at how many it takes to back out of
  // whatever that was, and about one run in five it was not enough -- leaving
  // a console with its field controls locked, which is indistinguishable from
  // an idle one right up until something needs it free. A mirrored scene will
  // not start on a console in that state, so stage 3 reported "player 2 never
  // played the scene" at about that same rate, for a ROM that was working.
  //
  // Being in the first-run prompt counts as free: that script only starts on
  // a frame where the player was in control, so its presence is proof.
  for (const w of [0, 1]) {
    let ok = false;
    for (let i = 0; i < 40 && !ok; i++) {
      const asking = (await api.u8(w, OFFSETS.firstRunRunningAddr)) !== 0;
      const busy = (await api.mailbox(w)).flags.includes('SCRIPT_BUSY');
      ok = asking || !busy;
      if (!ok) {
        await api.tap(w, 'B', 6);
        await api.wait(20);
      }
    }
    if (!ok)
      throw new Error(`core ${w}: never came back to the player after the intro`);
  }

  // A fresh co-op game asks each player their name, their gender and which
  // starter they want -- the things the opening would have asked, which a
  // co-op game does not have. Every stage boots into that prompt, and every
  // stage that is about something else wants it dealt with and a playable
  // character on the other side. Stage 14, which is about the prompt itself,
  // passes answerFirstRun: false.
  //
  // Hoenn and its third starter, chosen for no reason beyond having to.
  if (answerFirstRun) {
    for (const w of [0, 1]) {
      let asking = false;
      for (let i = 0; i < 40 && !asking; i++) {
        asking = (await api.u8(w, OFFSETS.firstRunRunningAddr)) !== 0;
        if (!asking) await api.wait(30);
      }
      if (!asking)
        throw new Error(`core ${w}: the first-run prompt never appeared`);

      await answerFirstRunPrompt(api, w, 2, 2);

      if ((await api.u8(w, OFFSETS.firstRunRunningAddr)) !== 0)
        throw new Error(`core ${w}: still in the first-run prompt after answering`);
    }
  }

  return api;
}

// Tiny result tally, so a run ends with a verdict rather than a wall of prose
// the reader has to grade themselves.
export function tally() {
  const results = [];
  return {
    check(name, ok, detail = '') {
      results.push(ok);
      // The detail is an explanation of FAILURE, so printing it beside a PASS
      // reads as a contradiction -- "PASS: the opponents have Pokemon --
      // generation produced empty parties" was on screen for most of an
      // evening. Evidence that is worth seeing either way goes in note().
      console.log(`${ok ? 'PASS' : 'FAIL'}: ${name}` +
                  (ok || !detail ? '' : ` -- ${detail}`));
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

// Choose three on both consoles and confirm.
//
// The selection is written straight into gSelectedOrderFromParty rather than
// typed into the menu. That is deliberate. Driving the menu by button means
// guessing, every single press, which of several states the console is in:
// the picker may not have opened yet (the guest's opens later than the host's,
// by an amount that moves with how fast the two cores happen to run), the
// ENTER/SUMMARY/CANCEL submenu may be animating and swallowing input, or the
// cursor may be sitting on a Pokemon already chosen, where the same two
// presses deselect it instead. Three separate attempts at a press-and-check
// loop each produced a different wrong answer on a different run -- 0 and 0,
// 2 and 1, 3 and 0 -- and every one of them reported a battle that started
// with the wrong number of Pokemon, which reads exactly like a bug in the
// co-op code. It was not. It was this function.
//
// What is under test here is the co-op flow -- that both consoles are asked,
// that the ready gate holds the faster player, that the battle starts capped
// at three a side. The party menu's own input handling is vanilla code and is
// not what we are checking, so there is nothing lost by skipping it and a lot
// of flakiness avoided.
//
// Writing the array is enough because that is the same array every later step
// reads: CheckBattleEntriesAndGetMessage validates it (and returns early for
// FACILITY_MULTI_OR_EREADER, which ChooseHalfPartyForBattle sets, so the
// duplicate-species rule does not apply), CB2_ReturnFromChooseHalfParty sets
// gSpecialVar_Result from its first entry, and ReducePlayerPartyToSelectedMons
// builds the party from it. START then jumps the cursor to CONFIRM -- arrowing
// down never reaches it, see PartyMenuButtonHandler's START_BUTTON case -- and
// A takes it.
export async function pickThreeOnBoth(rig, orderAddr, pickerOpenedAddr, want = 3,
                                      settleFrames = 420) {
  const chosen = async (w) =>
    (await rig.readAt(w, orderAddr, 6)).filter((x) => x !== 0).length;

  // Wait for the menu to exist on both. sPickerOpened is set in the same
  // breath as ChooseHalfPartyForBattle, so this is the console telling us
  // rather than us assuming after N frames.
  let open = [false, false];
  for (let i = 0; i < 25 && !open.every(Boolean); i++) {
    await rig.wait(30);
    open = [await rig.u8(0, pickerOpenedAddr), await rig.u8(1, pickerOpenedAddr)]
      .map((v) => v === 1);
  }
  if (!open.every(Boolean))
    console.log(`      picker never opened: p1=${open[0]} p2=${open[1]}`);

  // sPickerOpened says the script asked for the menu, not that the menu is
  // ready: it is set immediately before ScriptContext_Stop, while the party
  // screen is still fading in, and Task_HandleChooseMonInput ignores every
  // button while gPaletteFade.active. Confirming at that moment does nothing,
  // and the screenshot taken to explain it catches the fade and comes back
  // black, which sent me looking for a hang that was not there.
  //
  // Hence the settle. It is longer than the fade needs on purpose: there is no
  // race in the too-late direction, because nothing in the game is counting.
  // The picker sits open indefinitely waiting for a human, and the only clock
  // in this flow -- the ready gate that holds whoever chose first -- does not
  // start until a console confirms.
  await rig.wait(settleFrames);

  // Slots are 1-based here: 1,2,3 is "the first three in the party".
  const order = [1, 2, 3, 4, 5, 6].map((n, i) => (i < want ? n : 0));
  for (const w of [0, 1]) await rig.writeAt(w, orderAddr, order);
  await rig.wait(20);

  const final = [await chosen(0), await chosen(1)];

  // Both consoles are pressed in lockstep -- both STARTs, then both As --
  // rather than one console all the way through and then the other. Staggering
  // them means the first console confirms and begins the ready gate while the
  // second is still being driven, and the second's presses then land in a
  // different context than the first's did. The lockstep version is the one
  // that works; the staggered one failed in a way that looked like the battle
  // refusing to start.
  for (let attempt = 0; attempt < 6; attempt++) {
    const done = await Promise.all([0, 1].map(async (w) => {
      const f = (await rig.mailbox(w)).flags;
      return f.includes('AT_GATE') || f.includes('BATTLE');
    }));
    if (done.every(Boolean)) break;
    console.log(`      confirm attempt ${attempt}: ` +
      `order=${(await rig.readAt(0, orderAddr, 6)).join('')}/` +
      `${(await rig.readAt(1, orderAddr, 6)).join('')} ` +
      `picker=${await rig.u8(0, pickerOpenedAddr)}/${await rig.u8(1, pickerOpenedAddr)} ` +
      (await rig.mailboxes()).map((x) => `p${x.id} ${x.flags}`).join('  '));
    for (const w of [0, 1]) await rig.tap(w, 'Start', 8);
    await rig.wait(45);
    for (const w of [0, 1]) await rig.tap(w, 'A', 8);
    await rig.wait(90);
  }

  return final;
}
