'use strict';

// Touch control layout and hit-testing.
//
// Kept as pure geometry, separate from the DOM, because this is the part with
// real logic in it and the part most likely to feel wrong on a phone. Testing
// it without a browser means the tuning decisions below are at least provably
// doing what they claim.
//
// Layout is in normalized units (0..1 of the overlay box) so it scales to any
// screen without a second set of numbers.

export const BTN = {
  UP: 'up',
  DOWN: 'down',
  LEFT: 'left',
  RIGHT: 'right',
  A: 'a',
  B: 'b',
  L: 'l',
  R: 'r',
  START: 'start',
  SELECT: 'select',
};

// mGBA's input names, which is what buttonPress/buttonUnpress expect.
export const MGBA_NAMES = {
  [BTN.UP]: 'Up',
  [BTN.DOWN]: 'Down',
  [BTN.LEFT]: 'Left',
  [BTN.RIGHT]: 'Right',
  [BTN.A]: 'A',
  [BTN.B]: 'B',
  [BTN.L]: 'L',
  [BTN.R]: 'R',
  [BTN.START]: 'Start',
  [BTN.SELECT]: 'Select',
};

// Keyboard mapping, for playing in a desktop browser.
//
// Keyed on KeyboardEvent.code rather than .key so the mapping is physical: the
// same keys work on a non-QWERTY layout, and holding a modifier does not change
// what a key reports. X/Z for A/B follows mGBA's own default, which is the core
// this runs on.
export const KEY_MAP = {
  ArrowUp: BTN.UP,
  ArrowDown: BTN.DOWN,
  ArrowLeft: BTN.LEFT,
  ArrowRight: BTN.RIGHT,
  KeyX: BTN.A,
  KeyZ: BTN.B,
  KeyA: BTN.L,
  KeyS: BTN.R,
  Enter: BTN.START,
  Backspace: BTN.SELECT,
  ShiftLeft: BTN.SELECT,
  ShiftRight: BTN.SELECT,
};

/**
 * Union of every input source, for InputState.diff.
 *
 * diff() takes the complete set of buttons held right now, so the sources --
 * touch, mouse, keyboard -- must be merged before it is called. Tracking them
 * separately and unioning here means releasing a key cannot cancel a button the
 * other hand is still holding on screen.
 *
 * @param {...Set<string>} sets
 * @returns {Set<string>}
 */
export function unionButtons(...sets) {
  const out = new Set();
  for (const s of sets) {
    if (!s) continue;
    for (const b of s) out.add(b);
  }
  return out;
}

/**
 * Default landscape layout.
 *
 * Everything lives in the lower half, within thumb reach. On real hardware L
 * and R sit on the back edge, which maps naturally to the top of a screen --
 * but a thumb holding a phone in landscape cannot get there, so they go in the
 * bottom corners instead. Nothing overlaps the top of the picture.
 */
export const DEFAULT_LAYOUT = {
  // A floating thumbstick rather than a d-pad.
  //
  // A d-pad is drawn in one place and your thumb has to find it, which on a
  // phone it does not: you look at the game, not at your hands, and the
  // reported symptom is "the controls are off from where you click". The
  // region below is where putting a thumb down CREATES a stick, centred
  // wherever you touched. There is nothing to aim at, so there is nothing to
  // miss.
  //
  // The region is deliberately most of the left half: anywhere down there is
  // a reasonable place to rest a thumb, and the face buttons are far enough
  // right to stay out of it.
  stick: { x0: 0.0, y0: 0.28, x1: 0.45, y1: 1.0, r: 0.11 },
  buttons: [
    { id: BTN.A, cx: 0.88, cy: 0.6, r: 0.072 },
    { id: BTN.B, cx: 0.75, cy: 0.72, r: 0.072 },
    { id: BTN.L, cx: 0.06, cy: 0.94, r: 0.055 },
    { id: BTN.R, cx: 0.94, cy: 0.94, r: 0.055 },
    { id: BTN.START, cx: 0.57, cy: 0.94, r: 0.05 },
    { id: BTN.SELECT, cx: 0.43, cy: 0.94, r: 0.05 },
  ],
};

// A finger is not a point. Buttons get a generous invisible margin so a touch
// landing just outside the drawn circle still counts -- without it, play feels
// unresponsive in exactly the way people blame on lag.
export const TOUCH_SLOP = 1.35;

// Inside this fraction of the stick's travel radius, no direction is pressed.
// A thumb never rests perfectly still, and without this the avatar twitches
// between directions while you are standing still reading something.
export const STICK_DEADZONE = 0.26;

// Half-width of each diagonal band, in radians. Pi/8 would make the four
// cardinals and four diagonals equal; a wider band makes diagonals easier to
// hold, which matters for walking around corners and for Acro Bike tricks.
export const DIAGONAL_BAND = Math.PI / 6;

function hypot(dx, dy) {
  return Math.sqrt(dx * dx + dy * dy);
}

/** True if (x, y) is somewhere a thumb-down would grab the stick. */
export function inStickRegion(layout, x, y) {
  const s = layout.stick;
  return x >= s.x0 && x <= s.x1 && y >= s.y0 && y <= s.y1;
}

/**
 * Which directions a thumb at (x, y) presses, given where it went down.
 *
 * Deliberately has no outer limit. Once a thumb owns the stick it keeps
 * steering however far it travels -- running the length of a route and sliding
 * past some invisible edge, which is what a d-pad does, is the whole problem
 * being fixed here.
 *
 * @returns {string[]} zero, one, or two of up/down/left/right
 */
export function stickDirections(layout, originX, originY, x, y) {
  const r = layout.stick.r;
  const dx = x - originX;
  // Screen y grows downward; flip so maths angles read naturally.
  const dy = -(y - originY);
  const d = hypot(dx, dy);

  if (d < r * STICK_DEADZONE) return [];

  // Angle measured from +x (right), counter-clockwise.
  let a = Math.atan2(dy, dx);
  if (a < 0) a += Math.PI * 2;

  const out = [];
  const near = (target) => {
    let diff = Math.abs(a - target);
    if (diff > Math.PI) diff = Math.PI * 2 - diff;
    return diff;
  };

  // A direction is pressed when the thumb is within 45 degrees + half the
  // diagonal band of its axis, which is what lets two fire at once.
  const limit = Math.PI / 4 + DIAGONAL_BAND;
  if (near(0) < limit) out.push(BTN.RIGHT);
  if (near(Math.PI / 2) < limit) out.push(BTN.UP);
  if (near(Math.PI) < limit) out.push(BTN.LEFT);
  if (near((Math.PI * 3) / 2) < limit) out.push(BTN.DOWN);

  // Opposite directions cancel: the hardware cannot report both, and letting
  // them through makes the player avatar stutter.
  if (out.includes(BTN.LEFT) && out.includes(BTN.RIGHT)) {
    return out.filter((b) => b !== BTN.LEFT && b !== BTN.RIGHT);
  }
  if (out.includes(BTN.UP) && out.includes(BTN.DOWN)) {
    return out.filter((b) => b !== BTN.UP && b !== BTN.DOWN);
  }

  return out;
}

/**
 * The stick's state across frames.
 *
 * Stateful because a thumbstick is: where the thumb went down is the centre,
 * and it stays the centre until that same finger lifts. Which finger matters
 * -- the other thumb is on the face buttons, and a stick that re-centred on
 * whichever touch happened to be first in the list would jump across the
 * screen every time somebody pressed A.
 */
export class TouchStick {
  constructor(layout) {
    this.layout = layout;
    this.id = null;      // the touch identifier that owns the stick
    this.origin = null;  // where it went down
    this.point = null;   // where it is now
  }

  /**
   * @param {{id:number,x:number,y:number}[]} touches every touch down now
   * @returns {string[]} directions pressed
   */
  update(touches) {
    const mine = this.id === null ? null : touches.find((t) => t.id === this.id);

    if (mine === undefined || mine === null) {
      // Our finger has lifted, or we have none. Look for a new one -- but only
      // a touch that is not already doing something else.
      this.id = null;
      this.origin = null;
      this.point = null;

      for (const t of touches) {
        if (!inStickRegion(this.layout, t.x, t.y)) continue;
        if (buttonAt(this.layout, t.x, t.y)) continue;
        this.id = t.id;
        this.origin = { x: t.x, y: t.y };
        this.point = { x: t.x, y: t.y };
        break;
      }
      // Nothing is pressed on the frame it goes down: the thumb is at the
      // centre by definition.
      return [];
    }

    this.point = { x: mine.x, y: mine.y };
    return stickDirections(this.layout, this.origin.x, this.origin.y,
                           mine.x, mine.y);
  }

  /** Let go of everything, for a backgrounded tab or a lost touch. */
  clear() {
    this.id = null;
    this.origin = null;
    this.point = null;
  }

  /** Where to draw it, or null when no thumb is on it. */
  visual() {
    if (!this.origin) return null;
    const r = this.layout.stick.r;
    const dx = this.point.x - this.origin.x;
    const dy = this.point.y - this.origin.y;
    const d = hypot(dx, dy);
    // The knob stops at the rim however far the thumb goes, which is what
    // makes it read as a stick rather than a dot chasing your finger.
    const k = d > r ? r / d : 1;
    return {
      origin: this.origin,
      knob: { x: this.origin.x + dx * k, y: this.origin.y + dy * k },
      r,
    };
  }
}

/** Which face/shoulder button a touch at (x, y) presses, if any. */
export function buttonAt(layout, x, y) {
  let best = null;
  let bestD = Infinity;

  for (const b of layout.buttons) {
    const d = hypot(x - b.cx, y - b.cy);
    // Nearest wins, so overlapping slop regions resolve predictably rather
    // than by array order.
    if (d <= b.r * TOUCH_SLOP && d < bestD) {
      best = b.id;
      bestD = d;
    }
  }

  return best;
}

/**
 * Drop left+right and up+down when both are held.
 *
 * The hardware cannot report an axis in both directions at once, and letting
 * both through makes the avatar stutter in place instead of walking. The stick
 * already refuses to do this within one thumb, but the sources are merged after
 * that -- an arrow key on a desktop keyboard and a thumb on screen can disagree
 * -- so the merged set gets the same treatment.
 *
 * @param {Set<string>} pressed mutated in place
 * @returns {Set<string>} the same set, for chaining
 */
export function cancelOpposites(pressed) {
  if (pressed.has(BTN.LEFT) && pressed.has(BTN.RIGHT)) {
    pressed.delete(BTN.LEFT);
    pressed.delete(BTN.RIGHT);
  }
  if (pressed.has(BTN.UP) && pressed.has(BTN.DOWN)) {
    pressed.delete(BTN.UP);
    pressed.delete(BTN.DOWN);
  }
  return pressed;
}

/**
 * Resolve a set of active touches into the set of pressed buttons.
 *
 * Multi-touch is the point: running is B plus a direction, and plenty of the
 * game needs two or three at once. Each touch is resolved independently and
 * the results unioned.
 *
 * `stick` is optional so a mouse, which has one pointer and no persistent
 * thumb, can use this without one.
 *
 * @param {{id?:number,x:number,y:number}[]} touches normalized positions
 * @param {TouchStick} [stick]
 * @returns {Set<string>}
 */
export function resolveTouches(layout, touches, stick) {
  const pressed = new Set();

  if (stick) for (const d of stick.update(touches)) pressed.add(d);

  for (const t of touches) {
    const b = buttonAt(layout, t.x, t.y);
    if (b) pressed.add(b);
  }

  return cancelOpposites(pressed);
}

/**
 * Tracks pressed state across frames and emits only the changes, so the
 * emulator sees one press and one release per button rather than a storm of
 * redundant calls every touchmove.
 */
export class InputState {
  constructor() {
    this.pressed = new Set();
  }

  /**
   * @param {Set<string>} next
   * @returns {{press: string[], release: string[]}}
   */
  diff(next) {
    const press = [];
    const release = [];

    for (const b of next) if (!this.pressed.has(b)) press.push(b);
    for (const b of this.pressed) if (!next.has(b)) release.push(b);

    this.pressed = new Set(next);
    return { press, release };
  }

  /** Release everything. Used when the page loses focus or a session ends. */
  clear() {
    const release = [...this.pressed];
    this.pressed.clear();
    return { press: [], release };
  }
}
