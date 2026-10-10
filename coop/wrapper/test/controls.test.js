'use strict';

import test from 'node:test';
import assert from 'node:assert/strict';

import {
  BTN,
  MGBA_NAMES,
  DEFAULT_LAYOUT as L,
  TOUCH_SLOP,
  STICK_DEADZONE,
  stickDirections,
  inStickRegion,
  TouchStick,
  buttonAt,
  resolveTouches,
  cancelOpposites,
  InputState,
  KEY_MAP,
  unionButtons,
} from '../src/controls.js';

// Somewhere a thumb would plausibly land in the stick region.
const HOME = { x: 0.18, y: 0.7 };

/** A point at angle `deg`, `frac` of the stick's travel radius from HOME. */
function fromHome(deg, frac = 0.8, origin = HOME) {
  const a = (deg * Math.PI) / 180;
  const r = L.stick.r;
  return { x: origin.x + Math.cos(a) * r * frac, y: origin.y - Math.sin(a) * r * frac };
}

const dirs = (p, origin = HOME) =>
  stickDirections(L, origin.x, origin.y, p.x, p.y);

function at(id) {
  return L.buttons.find((b) => b.id === id);
}

// ---------------------------------------------------------------------------
// the thumbstick
// ---------------------------------------------------------------------------

test('the four cardinal directions resolve correctly', () => {
  assert.deepEqual(dirs(fromHome(0)), [BTN.RIGHT]);
  assert.deepEqual(dirs(fromHome(90)), [BTN.UP]);
  assert.deepEqual(dirs(fromHome(180)), [BTN.LEFT]);
  assert.deepEqual(dirs(fromHome(270)), [BTN.DOWN]);
});

test('up is up — screen y is inverted and must not be mixed up', () => {
  // A regression guard: getting this backwards is the classic bug here, and
  // it would make the game playable-but-wrong in a very confusing way.
  const r = L.stick.r;
  assert.deepEqual(dirs({ x: HOME.x, y: HOME.y - r * 0.8 }), [BTN.UP]);
  assert.deepEqual(dirs({ x: HOME.x, y: HOME.y + r * 0.8 }), [BTN.DOWN]);
});

test('diagonals press two directions at once', () => {
  for (const [deg, want] of [
    [45, [BTN.RIGHT, BTN.UP]],
    [135, [BTN.UP, BTN.LEFT]],
    [225, [BTN.LEFT, BTN.DOWN]],
    [315, [BTN.RIGHT, BTN.DOWN]],
  ]) {
    const got = dirs(fromHome(deg));
    assert.equal(got.length, 2, `${deg} deg gives two directions, got ${got}`);
    assert.deepEqual(new Set(got), new Set(want), `${deg} deg`);
  }
});

test('the diagonal band is wide enough to hold comfortably', () => {
  for (const deg of [32, 45, 58]) {
    assert.equal(dirs(fromHome(deg)).length, 2, `${deg} deg still diagonal`);
  }
});

test('a thumb resting at the centre presses nothing', () => {
  assert.deepEqual(dirs(HOME), [], 'dead centre');
  assert.deepEqual(dirs(fromHome(45, STICK_DEADZONE * 0.5)), [], 'inside deadzone');
});

test('the stick keeps steering however far the thumb travels', () => {
  // The whole point of replacing the d-pad. A d-pad has an edge you slide off
  // without noticing -- you are looking at the game, not your hands -- and
  // what it feels like is the game ignoring you.
  for (const frac of [1, 3, 10]) {
    assert.deepEqual(dirs(fromHome(90, frac)), [BTN.UP], `${frac}x out`);
  }
});

test('the stick is created wherever the thumb lands, not in one place', () => {
  // Two thumbs-down a long way apart both work, and each steers from its own
  // centre. A fixed control cannot do this and that is the complaint it fixes.
  for (const origin of [{ x: 0.05, y: 0.35 }, { x: 0.40, y: 0.95 }]) {
    assert.ok(inStickRegion(L, origin.x, origin.y), `${origin.x},${origin.y} usable`);
    assert.deepEqual(dirs(fromHome(180, 0.8, origin), origin), [BTN.LEFT]);
  }
});

test('no button can be turned into a stick by touching it', () => {
  // The region is deliberately generous -- anywhere down the left is a
  // reasonable place to rest a thumb -- so it does overlap the bottom row.
  // What matters is that a touch landing ON a control presses that control
  // instead of creating a stick under it.
  for (const b of L.buttons) {
    const s = new TouchStick(L);
    s.update([{ id: 1, x: b.cx, y: b.cy }]);
    assert.equal(s.origin, null, `${b.id} must not grab the stick`);
  }
});

// ---------------------------------------------------------------------------
// the stick across frames
// ---------------------------------------------------------------------------

test('the first frame of a touch presses nothing', () => {
  const s = new TouchStick(L);
  assert.deepEqual(s.update([{ id: 1, ...HOME }]), [],
    'the thumb is at the centre by definition on the frame it lands');
});

test('the stick follows the finger that made it, not the first in the list', () => {
  // The other thumb is on the face buttons. A stick that re-centred on
  // whichever touch came first would jump across the screen every time
  // somebody pressed A -- and you would be walking the wrong way.
  const s = new TouchStick(L);
  s.update([{ id: 7, ...HOME }]);

  const a = at(BTN.A);
  const moved = fromHome(90);
  const got = s.update([
    { id: 99, x: a.cx, y: a.cy },       // the other thumb, listed first
    { id: 7, x: moved.x, y: moved.y },  // ours
  ]);

  assert.deepEqual(got, [BTN.UP]);
  assert.equal(s.origin.x, HOME.x, 'origin unchanged');
  assert.equal(s.origin.y, HOME.y, 'origin unchanged');
});

test('lifting the finger lets the stick go', () => {
  const s = new TouchStick(L);
  s.update([{ id: 1, ...HOME }]);
  const moved = fromHome(0);
  assert.deepEqual(s.update([{ id: 1, x: moved.x, y: moved.y }]), [BTN.RIGHT]);
  assert.deepEqual(s.update([]), []);
  assert.equal(s.origin, null);
});

test('a new touch after a lift makes a new stick somewhere else', () => {
  const s = new TouchStick(L);
  s.update([{ id: 1, ...HOME }]);
  s.update([]);

  const elsewhere = { x: 0.35, y: 0.42 };
  s.update([{ id: 2, ...elsewhere }]);
  const moved = fromHome(270, 0.8, elsewhere);
  assert.deepEqual(s.update([{ id: 2, x: moved.x, y: moved.y }]), [BTN.DOWN]);
});

test('a touch on a face button never becomes the stick', () => {
  const s = new TouchStick(L);
  const a = at(BTN.A);
  s.update([{ id: 1, x: a.cx, y: a.cy }]);
  assert.equal(s.origin, null, 'pressing A must not create a stick');
});

test('the knob stops at the rim however far the thumb goes', () => {
  // Otherwise it reads as a dot chasing your finger rather than a stick.
  const s = new TouchStick(L);
  s.update([{ id: 1, ...HOME }]);
  const far = fromHome(0, 8);
  s.update([{ id: 1, x: far.x, y: far.y }]);

  const v = s.visual();
  const d = Math.hypot(v.knob.x - v.origin.x, v.knob.y - v.origin.y);
  assert.ok(Math.abs(d - L.stick.r) < 1e-9, `knob at ${d}, rim at ${L.stick.r}`);
});

test('clear lets go, so a backgrounded tab does not keep walking', () => {
  const s = new TouchStick(L);
  s.update([{ id: 1, ...HOME }]);
  s.clear();
  assert.equal(s.visual(), null);
});

// ---------------------------------------------------------------------------
// face and shoulder buttons
// ---------------------------------------------------------------------------

test('each button is hit at its own centre', () => {
  for (const b of L.buttons) {
    assert.equal(buttonAt(L, b.cx, b.cy), b.id, `${b.id} at centre`);
  }
});

test('no button is hit in empty space', () => {
  assert.equal(buttonAt(L, 0.5, 0.35), null);
});

test('overlapping slop resolves to the nearest button, not array order', () => {
  const a = at(BTN.A);
  const b = at(BTN.B);
  // A point biased strongly toward A must give A even though B is listed later.
  const x = a.cx + (b.cx - a.cx) * 0.2;
  const y = a.cy + (b.cy - a.cy) * 0.2;
  assert.equal(buttonAt(L, x, y), BTN.A);

  const x2 = b.cx + (a.cx - b.cx) * 0.2;
  const y2 = b.cy + (a.cy - b.cy) * 0.2;
  assert.equal(buttonAt(L, x2, y2), BTN.B);
});

test('A and B are far enough apart not to be hit together', () => {
  // If one finger could press both, battles would be miserable.
  const a = at(BTN.A);
  assert.equal(buttonAt(L, a.cx, a.cy), BTN.A);
  const pressed = resolveTouches(L, [{ x: a.cx, y: a.cy }]);
  assert.deepEqual([...pressed], [BTN.A]);
});

test('Start and Select sit clear of the face buttons', () => {
  for (const id of [BTN.START, BTN.SELECT]) {
    const s = at(id);
    const pressed = resolveTouches(L, [{ x: s.cx, y: s.cy }]);
    assert.deepEqual([...pressed], [id], `${id} is unambiguous`);
  }
});

test('no two buttons overlap at their drawn radii', () => {
  for (let i = 0; i < L.buttons.length; i++) {
    for (let j = i + 1; j < L.buttons.length; j++) {
      const p = L.buttons[i];
      const q = L.buttons[j];
      const d = Math.hypot(p.cx - q.cx, p.cy - q.cy);
      assert.ok(d > p.r + q.r, `${p.id} and ${q.id} must not overlap (d=${d.toFixed(3)})`);
    }
  }
});

test('every control sits inside the overlay box', () => {
  const s = L.stick;
  assert.ok(s.x0 >= 0 && s.x1 <= 1, 'stick region within x');
  assert.ok(s.y0 >= 0 && s.y1 <= 1, 'stick region within y');
  for (const b of L.buttons) {
    assert.ok(b.cx - b.r >= 0 && b.cx + b.r <= 1, `${b.id} within x`);
    assert.ok(b.cy - b.r >= 0 && b.cy + b.r <= 1, `${b.id} within y`);
  }
});

// ---------------------------------------------------------------------------
// multi-touch
// ---------------------------------------------------------------------------

test('running works: B held with a direction', () => {
  // The single most common two-finger combination in the whole game.
  const b = at(BTN.B);
  const stick = new TouchStick(L);
  stick.update([{ id: 1, ...HOME }]);          // thumb down
  const dir = fromHome(180);
  const pressed = resolveTouches(L, [
    { id: 1, x: dir.x, y: dir.y },
    { id: 2, x: b.cx, y: b.cy },
  ], stick);
  assert.deepEqual(new Set(pressed), new Set([BTN.LEFT, BTN.B]));
});

test('three simultaneous touches all register', () => {
  // A diagonal on the stick plus two buttons: four inputs from three fingers.
  const a = at(BTN.A);
  const r = at(BTN.R);
  const stick = new TouchStick(L);
  stick.update([{ id: 1, ...HOME }]);
  const dir = fromHome(45);
  const pressed = resolveTouches(L, [
    { id: 1, x: dir.x, y: dir.y },
    { id: 2, x: a.cx, y: a.cy },
    { id: 3, x: r.cx, y: r.cy },
  ], stick);
  assert.deepEqual(new Set(pressed), new Set([BTN.RIGHT, BTN.UP, BTN.A, BTN.R]));
});

test('the stick never reports opposite directions, whatever the angle', () => {
  // The hardware cannot report left and right together; letting both through
  // makes the avatar stutter in place. One thumb cannot be in two places, so
  // the guard here is against a too-wide diagonal band -- sweep the whole
  // circle rather than trusting the one angle that happens to be tested above.
  for (let deg = 0; deg < 360; deg++) {
    const got = new Set(dirs(fromHome(deg)));
    assert.ok(!(got.has(BTN.LEFT) && got.has(BTN.RIGHT)), `${deg} deg: L+R`);
    assert.ok(!(got.has(BTN.UP) && got.has(BTN.DOWN)), `${deg} deg: U+D`);
    assert.ok(got.size >= 1 && got.size <= 2, `${deg} deg gave ${got.size}`);
  }
});

test('a key and the thumb pulling opposite ways cancel', () => {
  // Desktop play: the sources are merged after the stick has had its say, so
  // this is the one place an opposing pair can still appear.
  const merged = cancelOpposites(
    unionButtons(new Set([BTN.RIGHT, BTN.B]), new Set([BTN.LEFT])));
  assert.deepEqual([...merged], [BTN.B]);
});

test('no touches means nothing pressed', () => {
  assert.equal(resolveTouches(L, []).size, 0);
});

// ---------------------------------------------------------------------------
// edge-triggered state
// ---------------------------------------------------------------------------

test('only changes are emitted, not the whole held state', () => {
  const s = new InputState();

  let d = s.diff(new Set([BTN.RIGHT]));
  assert.deepEqual(d, { press: [BTN.RIGHT], release: [] });

  // A held button must not re-press every frame: 60 presses a second would
  // swamp the emulator and break anything that counts button-down edges.
  d = s.diff(new Set([BTN.RIGHT]));
  assert.deepEqual(d, { press: [], release: [] }, 'holding emits nothing');

  d = s.diff(new Set([BTN.RIGHT, BTN.B]));
  assert.deepEqual(d, { press: [BTN.B], release: [] });

  d = s.diff(new Set([BTN.B]));
  assert.deepEqual(d, { press: [], release: [BTN.RIGHT] });

  d = s.diff(new Set());
  assert.deepEqual(d, { press: [], release: [BTN.B] });
});

test('a direction change presses the new one and releases the old', () => {
  const s = new InputState();
  s.diff(new Set([BTN.LEFT]));
  const d = s.diff(new Set([BTN.RIGHT]));
  assert.deepEqual(d.press, [BTN.RIGHT]);
  assert.deepEqual(d.release, [BTN.LEFT]);
});

test('clear releases everything held', () => {
  const s = new InputState();
  s.diff(new Set([BTN.A, BTN.UP]));
  const d = s.clear();
  assert.deepEqual(new Set(d.release), new Set([BTN.A, BTN.UP]));
  assert.equal(s.pressed.size, 0);
  // Without this, backgrounding the tab mid-walk leaves a direction stuck down
  // and the player keeps moving after they come back.
  assert.deepEqual(s.diff(new Set()), { press: [], release: [] });
});

// ---------------------------------------------------------------------------
// mapping
// ---------------------------------------------------------------------------

test('every button maps to an mGBA input name', () => {
  for (const id of Object.values(BTN)) {
    assert.ok(MGBA_NAMES[id], `${id} has an mGBA name`);
  }
  assert.equal(MGBA_NAMES[BTN.START], 'Start');
  assert.equal(MGBA_NAMES[BTN.SELECT], 'Select');
  assert.equal(MGBA_NAMES[BTN.UP], 'Up');
});

// --- keyboard and multi-source input ---------------------------------------
//
// Added when the wrapper gained desktop support: the page was touch-only, so a
// computer browser could neither type nor click the on-screen pad.

test('KEY_MAP covers every button the pad has', () => {
  const mapped = new Set(Object.values(KEY_MAP));
  for (const btn of Object.values(BTN)) {
    assert.ok(mapped.has(btn), `no key mapped to ${btn}`);
  }
});

test('KEY_MAP is keyed on physical codes, not characters', () => {
  // KeyboardEvent.code values, so the mapping survives a non-QWERTY layout.
  // A entry like 'x' or 'ArrowUp '.trim() slipping in would silently never match.
  for (const code of Object.keys(KEY_MAP)) {
    assert.match(code, /^(Arrow(Up|Down|Left|Right)|Key[A-Z]|Enter|Backspace|Shift(Left|Right))$/,
      `${code} is not a KeyboardEvent.code`);
  }
});

test('unionButtons merges every source', () => {
  const out = unionButtons(new Set(['up']), new Set(['a']), new Set(['up', 'b']));
  assert.deepStrictEqual([...out].sort(), ['a', 'b', 'up']);
});

test('unionButtons tolerates missing sources', () => {
  assert.deepStrictEqual([...unionButtons(null, undefined, new Set(['a']))], ['a']);
  assert.deepStrictEqual([...unionButtons()], []);
});

test('releasing a key does not cancel a button another source still holds', () => {
  // The reason the sources are tracked separately. Thumb on the stick, hand on
  // the keyboard: letting go of the key must not clear the held direction.
  const touch = new Set(['up']);
  const keys = new Set(['a']);
  const input = new InputState();

  input.diff(unionButtons(touch, keys));
  keys.delete('a');
  const { press, release } = input.diff(unionButtons(touch, keys));

  assert.deepStrictEqual(press, []);
  assert.deepStrictEqual(release, ['a']);
  assert.ok(input.pressed.has('up'), 'the held direction was wrongly released');
});

test('the same button from two sources survives one of them releasing', () => {
  const touch = new Set(['a']);
  const keys = new Set(['a']);
  const input = new InputState();

  input.diff(unionButtons(touch, keys));
  keys.delete('a');
  const { release } = input.diff(unionButtons(touch, keys));

  assert.deepStrictEqual(release, [], 'A was released while still held on screen');
});
