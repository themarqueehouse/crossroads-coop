#!/usr/bin/env node
// Static server for the test rig.
//
// The only non-obvious part is the two headers. @thenick775/mgba-wasm is a
// pthread build, so it needs SharedArrayBuffer, and browsers only hand that out
// to cross-origin-isolated pages. Without COOP/COEP the core fails to
// instantiate with an error that does not mention threads at all.
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { extname, join, normalize } from 'node:path';

const ROOT = new URL('.', import.meta.url).pathname;
const MGBA = '/home/claude/crossroads/coop/wrapper/node_modules/@thenick775/mgba-wasm/dist';

const TYPES = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'text/javascript; charset=utf-8',
  '.mjs': 'text/javascript; charset=utf-8',
  '.wasm': 'application/wasm',
  '.gba': 'application/octet-stream',
};

// The ROM under test, set by the driver before listen(). Served from wherever
// it sits on disk rather than copied into this directory.
export let romPath = null;
export function setRom(p) { romPath = p; }

async function resolve(urlPath) {
  const clean = normalize(decodeURIComponent(urlPath)).replace(/^(\.\.[/\\])+/, '');
  if (clean === '/rom.gba' && romPath) return romPath;
  // mgba.js and its .wasm/.worker.js siblings live in node_modules; everything
  // else is a file in this directory.
  if (/^\/mgba/.test(clean)) return join(MGBA, clean.slice(1));
  return join(ROOT, clean === '/' ? 'rig.html' : clean.slice(1));
}

const server = createServer(async (req, res) => {
  try {
    const file = await resolve(new URL(req.url, 'http://x').pathname);
    const body = await readFile(file);
    res.writeHead(200, {
      'Content-Type': TYPES[extname(file)] || 'application/octet-stream',
      // Cross-origin isolation, required for SharedArrayBuffer.
      'Cross-Origin-Opener-Policy': 'same-origin',
      'Cross-Origin-Embedder-Policy': 'require-corp',
      'Cross-Origin-Resource-Policy': 'same-origin',
      'Cache-Control': 'no-store',
    });
    res.end(body);
  } catch (e) {
    res.writeHead(404, { 'Content-Type': 'text/plain' });
    res.end(String(e && e.message));
  }
});

// Only listen when run directly. twoplayer.mjs imports this module and starts
// it itself; a listen at import time would double-bind and throw EADDRINUSE.
if (process.argv[1] && process.argv[1].endsWith('serve.mjs')) {
  const port = Number(process.argv[2] || 8777);
  server.listen(port, () => console.log(`rig server on http://127.0.0.1:${port}`));
}

export default server;
