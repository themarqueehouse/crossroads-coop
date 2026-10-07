#ifndef GUARD_CONSTANTS_COOP_GATES_H
#define GUARD_CONSTANTS_COOP_GATES_H

// Co-op sync gate ids, for the `coopgate` script command.
//
// A gate is a moment neither player passes alone. Whoever gets there first
// waits; when the other arrives at the SAME id, both carry on together.
//
// The id identifies the moment, not the place and not the player. Both consoles
// run the same script out of the same ROM, so they reach the same id without
// anything being negotiated -- which is the whole reason this is a plain number
// and not a handshake.
//
// Two rules for adding one:
//
//   1. Never reuse an id for a different moment. Two unrelated gates sharing an
//      id would open each other from opposite ends of the map.
//   2. Never put a gate somewhere only one player can reach. A gate in a script
//      that only fires for whoever stepped on the trigger is a game that stops
//      for ever. Those moments need the script itself mirrored to both
//      consoles; a gate alone is not enough.
//
// 0 is reserved: it means "no gate".
#define GATE_NONE 0

// Gates 1-99 are reserved for testing the mechanism itself.
#define GATE_TEST        1
#define GATE_TEST_SECOND 2

// Story gates start at 100, numbered in rough story order with gaps left for
// whatever gets inserted later.
#define GATE_STORY_BASE 100

#endif // GUARD_CONSTANTS_COOP_GATES_H
