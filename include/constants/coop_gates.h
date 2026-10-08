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

// Both players have finished choosing their three and are ready to fight.
//
// One reserved id is enough: only one co-op battle is ever being set up at a
// time, since setting one up locks both players out of the overworld.
#define GATE_COOP_BATTLE_READY 99

// Gates 1-99 are reserved for testing the mechanism itself.
#define GATE_TEST        1
#define GATE_TEST_SECOND 2
#define GATE_TEST_GIFT   3
#define GATE_TEST_BATTLE 4

// Story gates start at 100, numbered in rough story order with gaps left for
// whatever gets inserted later.
#define GATE_STORY_BASE 100

// Birch's bag on Route 101. The one scene deliberately left unguarded, so that
// running it on both consoles gives both players a starter.
#define GATE_STORY_STARTERS (GATE_STORY_BASE + 0)

// Gym leaders. One each, numbered from 200 so story gates can grow freely
// between here and GATE_STORY_BASE.
#define GATE_GYM_BASE 200
#define GATE_GYM_ROXANNE    (GATE_GYM_BASE + 0)
#define GATE_GYM_BRAWLY     (GATE_GYM_BASE + 1)
#define GATE_GYM_WATTSON    (GATE_GYM_BASE + 2)
#define GATE_GYM_FLANNERY   (GATE_GYM_BASE + 3)
#define GATE_GYM_NORMAN     (GATE_GYM_BASE + 4)
#define GATE_GYM_WINONA     (GATE_GYM_BASE + 5)
#define GATE_GYM_TATE_LIZA  (GATE_GYM_BASE + 6)
#define GATE_GYM_JUAN       (GATE_GYM_BASE + 7)
#define GATE_GYM_BROCK      (GATE_GYM_BASE + 8)
#define GATE_GYM_MISTY      (GATE_GYM_BASE + 9)
#define GATE_GYM_LT_SURGE   (GATE_GYM_BASE + 10)
#define GATE_GYM_ERIKA      (GATE_GYM_BASE + 11)
#define GATE_GYM_KOGA       (GATE_GYM_BASE + 12)
#define GATE_GYM_SABRINA    (GATE_GYM_BASE + 13)
#define GATE_GYM_BLAINE     (GATE_GYM_BASE + 14)
#define GATE_GYM_GIOVANNI   (GATE_GYM_BASE + 15)

#endif // GUARD_CONSTANTS_COOP_GATES_H
