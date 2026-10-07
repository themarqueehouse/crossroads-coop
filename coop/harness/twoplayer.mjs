#!/usr/bin/env node
// Two-player co-op test rig.
//
// Boots the co-op ROM twice in headless Chromium, relays the mailbox between
// the two instances in-process, drives both past the title screen into the
// overworld, and then reports what each ROM says about itself.
//
// The point is to make the "we're connected but can't see each other" class of
// bug reproducible here, in seconds, instead of over a phone call. The relay
// logic below is deliberately the simplest thing that satisfies the mailbox
// contract -- if the real relay and this one disagree, that is itself a finding.
//
//   node coop/harness/twoplayer.mjs path/to/rom.gba
import { chromium } from 'playwright';
import { stat } from 'node:fs/promises';
import { globSync, mkdirSync } from 'node:fs';
import { basename } from 'node:path';
import server, { setRom } from './serve.mjs';

const PORT = 8777;
const ROM = process.argv[2] || 'pokeemerald_modern.gba';
const SHOTS = process.argv[3] || '/tmp/claude-0/shots-2p';

// struct NetMailbox, include/net_link.h. Kept here rather than imported so a
// silent struct change shows up as a loud mismatch in this rig.
const OFF = {
  magic: 0x00, version: 0x04, hostStatus: 0x05, localId: 0x06, playerCount: 0x07,
  outHead: 0x08, outTail: 0x09, inHead: 0x0a, inTail: 0x0c, heartbeat: 0x0e,
  out: 0x10, in: 0x90,
  coopState: 0x190, linkFlags: 0x191, posSent: 0x192, posRecv: 0x194,
  peerMap: 0x196, peerX: 0x198, peerY: 0x19a, selfMap: 0x19c, peerObjectId: 0x19e,
};
const RING_SLOTS = 8, RING_MASK = 7, CMD_LEN = 8, FRAME_BYTES = CMD_LEN * 2;
const SIZE = 0x1a0;

const HOST = { DOWN: 0, CONNECTING: 1, READY: 2, LOST: 3 };
const COOP_STATE = ['OFF', 'OPENING', 'EXCHANGING', 'ACTIVE', 'LOST'];
const DIAG = [
  [1 << 0, 'LINK_OPEN'], [1 << 1, 'PLAYERS_RECEIVED'], [1 << 2, 'CALLBACK_ARMED'],
  [1 << 3, 'PEER_VALID'], [1 << 4, 'PEER_SAME_MAP'],
];

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Button cycle used to walk through character creation unattended.
const NUDGE = [null, 'A', 'A', 'A', 'Start', null, 'A', 'Start'];

class Instance {
  constructor(page, id) { this.page = page; this.id = id; this.base = null; }

  async boot(romUrl, romName) {
    await this.page.evaluate(
      ([url, name]) => window.__rig.boot(url, name), [romUrl, romName]);
  }

  // The live mailbox is the one whose heartbeat is moving. Save states and
  // rewind buffers hold byte-identical stale copies, so the magic alone is not
  // enough to identify it -- this is the same discrimination the real wrapper
  // does, and the reason the heartbeat field exists at all.
  async locate() {
    const cands = await this.page.evaluate(() => window.__rig.findMailboxes());
    if (!cands.length) throw new Error(`player ${this.id}: no mailbox found`);
    const before = await this.readAt(cands, OFF.heartbeat, 2);
    await this.page.evaluate(() => window.__rig.waitFrames(10));
    const after = await this.readAt(cands, OFF.heartbeat, 2);
    const live = cands.filter((_, i) =>
      (before[i][0] | (before[i][1] << 8)) !== (after[i][0] | (after[i][1] << 8)));
    if (!live.length) {
      throw new Error(
        `player ${this.id}: found ${cands.length} mailbox candidate(s) but none ` +
        `has a moving heartbeat -- the ROM is not ticking VBlankIntr`);
    }
    this.base = live[0];
    return { candidates: cands.length, live: live.length, base: this.base };
  }

  readAt(bases, off, len) {
    return this.page.evaluate(
      ([bs, o, l]) => bs.map((b) => window.__rig.read(b + o, l)),
      [bases, off, len]);
  }

  read(off, len) {
    return this.page.evaluate(
      ([o, l]) => window.__rig.read(o, l), [this.base + off, len]);
  }

  write(off, bytes) {
    return this.page.evaluate(
      ([o, b]) => window.__rig.write(o, b), [this.base + off, bytes]);
  }

  all() { return this.read(0, SIZE); }

  tap(key, hold = 6) {
    return this.page.evaluate(([k, h]) => window.__rig.tap(k, h), [key, hold]);
  }

  waitFrames(n) { return this.page.evaluate((x) => window.__rig.waitFrames(x), n); }
}

function u16(b, o) { return b[o] | (b[o + 1] << 8); }

function describe(b, id) {
  const flags = DIAG.filter(([bit]) => b[OFF.linkFlags] & bit).map(([, n]) => n);
  return {
    player: id,
    coopState: COOP_STATE[b[OFF.coopState]] ?? b[OFF.coopState],
    linkFlags: flags.length ? flags.join('|') : '(none)',
    localId: b[OFF.localId],
    playerCount: b[OFF.playerCount],
    hostStatus: b[OFF.hostStatus],
    heartbeat: u16(b, OFF.heartbeat),
    posSent: u16(b, OFF.posSent),
    posRecv: u16(b, OFF.posRecv),
    selfMap: u16(b, OFF.selfMap).toString(16),
    peerMap: u16(b, OFF.peerMap).toString(16),
    peerXY: `${u16(b, OFF.peerX)},${u16(b, OFF.peerY)}`,
    peerObjectId: b[OFF.peerObjectId],
    outPending: (b[OFF.outHead] - b[OFF.outTail]) & RING_MASK,
  };
}

// One relay step.
//
// The subtlety that is easy to get wrong: a multi-player link cable gives every
// console EVERY console's command for that link frame, INCLUDING ITS OWN. The
// game relies on this -- DequeueRecvCmds (and our NetDequeueRecvCmds) will not
// deliver anything until every lane has a frame waiting, because a cable frame
// was atomic across all players. So each command goes to BOTH the peer's inbox
// and back into the sender's own inbox on its own lane. Forward only to the
// peer and both sides sit at CONN_ESTABLISHED forever, receiving nothing, which
// looks exactly like "connected but we cannot see each other".
//
// Backpressure also matters: writing past a full ring silently overwrites
// frames the game has not consumed, which desyncs in ways that surface later
// and look random.
async function pump(a, b) {
  for (const [src, dst] of [[a, b], [b, a]]) {
    const s = await src.all();
    const d = src === dst ? s : await dst.all();
    const lane = s[OFF.localId];

    // Both destinations for this sender's frames: the peer, and itself.
    const sinks = [
      { who: dst, buf: await dst.all() },
      { who: src, buf: s },
    ];

    let outTail = s[OFF.outTail];
    const outHead = s[OFF.outHead];
    const heads = sinks.map((k) => k.buf[OFF.inHead + lane]);
    const tails = sinks.map((k) => k.buf[OFF.inTail + lane]);

    let moved = 0;
    while (outTail !== outHead) {
      // Only move a frame if EVERY sink has room, so the two copies never
      // drift apart.
      if (heads.some((h, i) => ((h + 1) & RING_MASK) === tails[i])) break;
      const from = OFF.out + (outTail & RING_MASK) * FRAME_BYTES;
      const frame = s.slice(from, from + FRAME_BYTES);
      for (let i = 0; i < sinks.length; i++) {
        await sinks[i].who.write(
          OFF.in + (lane * RING_SLOTS + (heads[i] & RING_MASK)) * FRAME_BYTES, frame);
        heads[i] = (heads[i] + 1) & RING_MASK;
      }
      outTail = (outTail + 1) & RING_MASK;
      moved++;
    }

    if (moved) {
      for (let i = 0; i < sinks.length; i++) {
        await sinks[i].who.write(OFF.inHead + lane, [heads[i]]);
      }
      await src.write(OFF.outTail, [outTail]);
    }
  }
}

async function main() {
  const romName = basename(ROM);
  const { size } = await stat(ROM);
  console.log(`rom: ${ROM} (${(size / 1048576).toFixed(1)} MiB)`);

  setRom(ROM);
  await new Promise((r) => server.listen(PORT, r));
  // The pinned browser moves between images, so find it rather than hardcode a
  // path. The full chrome build, not headless_shell: we need real threads.
  const candidates = globSync('/opt/pw-browsers/chromium-*/chrome-linux/chrome');
  const browser = await chromium.launch({
    ...(candidates.length ? { executablePath: candidates[0] } : {}),
    args: ['--no-sandbox', '--enable-features=SharedArrayBuffer'],
  });

  try {
    const ctx = await browser.newContext();
    const players = [];
    for (let i = 0; i < 2; i++) {
      const page = await ctx.newPage();
      page.on('pageerror', (e) => console.log(`  [p${i} pageerror] ${e.message}`));
      await page.goto(`http://127.0.0.1:${PORT}/rig.html`);
      await page.waitForFunction(() => !!window.__rig);
      players.push(new Instance(page, i));
    }

    console.log('booting both cores...');
    await Promise.all(players.map((p) => p.boot(`http://127.0.0.1:${PORT}/rom.gba`, romName)));
    await Promise.all(players.map((p) => p.waitFrames(120)));

    for (const p of players) {
      const r = await p.locate();
      console.log(`  p${p.id}: mailbox at 0x${r.base.toString(16)} ` +
                  `(${r.candidates} candidate(s), ${r.live} live)`);
    }

    // Stand in for the relay's session setup.
    for (const p of players) {
      await p.write(OFF.localId, [p.id]);
      await p.write(OFF.playerCount, [2]);
      await p.write(OFF.hostStatus, [HOST.READY]);
    }
    console.log('session opened (localId 0/1, playerCount 2, hostStatus READY)');

    // The intro (Game Freak logo, then the title) runs for roughly 1500 frames
    // before SELECT means anything. Pressing earlier is not harmful but it is
    // not quickstart either, which is what made an earlier version of this rig
    // report "still on the title screen" and look like a co-op bug.
    console.log('waiting out the intro (~1400 frames)...');
    for (let i = 0; i < 14; i++) {
      await Promise.all(players.map((p) => p.waitFrames(100)));
      await pump(players[0], players[1]);
    }

    // Quickstart: SELECT at the title screen drops straight into the overworld
    // with a generated character, which is the only reason this rig is short.
    console.log('pressing SELECT at title on both...');
    await Promise.all(players.map((p) => p.tap('Select', 12)));

    // Quickstart still has to build a save, run the expansion splash and load
    // the first map. That is thousands of frames, not hundreds -- an earlier
    // version of this rig gave up during the splash and reported it as a co-op
    // failure. Keep pumping the relay throughout: the ROM is already running
    // our VBlank hook, so the mailbox is live well before the overworld is.
    // Character creation happens before any link traffic, so the relay does not
    // need tight pacing yet -- press often and pump loosely. Once we are in the
    // overworld the block exchange starts and the pacing flips: the rings hold
    // 7 frames and the ROM fills one per frame, so pumping must be frequent or
    // block fragments are dropped and the exchange never completes.
    console.log('walking through character creation...');
    let arrived = false;
    for (let i = 0; i < 300; i++) {
      await Promise.all(players.map((p) => p.waitFrames(12)));
      await pump(players[0], players[1]);
      const maps = await Promise.all(players.map(async (p) => u16(await p.all(), OFF.selfMap)));
      if (maps.every((m) => m !== 0)) {
        console.log(`  both in the overworld after ${i} steps`);
        arrived = true;
        break;
      }
      await Promise.all(players.map((p) => p.tap(NUDGE[i % NUDGE.length] || 'A', 6)));
    }
    if (!arrived) console.log('  WARNING: never reached the overworld');

    console.log('running 900 frames with the relay pumping...');
    console.log('  frame  p0 out/in0/in1 cmd      p1 out/in0/in1 cmd      states');
    for (let i = 0; i < 300; i++) {
      await Promise.all(players.map((p) => p.waitFrames(3)));
      await pump(players[0], players[1]);
      if (i % 40 === 39 || i < 4) {
        const bufs = await Promise.all(players.map((p) => p.all()));
        const cells = bufs.map((b) => {
          const ring = (h, t) => `${h}/${t}`;
          // First word of the most recently written outbox slot, which tells us
          // WHICH command the game is trying to send, not just that it tried.
          const slot = (b[OFF.outHead] - 1) & RING_MASK;
          const cmd = u16(b, OFF.out + slot * FRAME_BYTES);
          return `${ring(b[OFF.outHead], b[OFF.outTail])} ` +
                 `${ring(b[OFF.inHead], b[OFF.inTail])} ` +
                 `${ring(b[OFF.inHead + 1], b[OFF.inTail + 1])} ` +
                 `${cmd.toString(16).padStart(4, '0')}`;
        });
        const st = bufs.map((b) => COOP_STATE[b[OFF.coopState]]).join('/');
        console.log(`  ${String(i * 3).padStart(5)}  ${cells[0].padEnd(23)} ${cells[1].padEnd(23)} ${st}`);
      }
    }

    // The decisive evidence. Diagnostics can say the peer object exists while
    // the screen shows nothing; only the picture settles it.
    mkdirSync(SHOTS, { recursive: true });
    for (const p of players) {
      const f = `${SHOTS}/player${p.id}.png`;
      await p.page.locator('#screen').screenshot({ path: f });
      console.log(`  screen -> ${f}`);
    }

    console.log('\n=== what each ROM reports ===');
    for (const p of players) console.log(describe(await p.all(), p.id));
  } finally {
    await browser.close();
    server.close();
  }
}

main().catch((e) => { console.error('\nFAILED:', e.message); process.exit(1); });
