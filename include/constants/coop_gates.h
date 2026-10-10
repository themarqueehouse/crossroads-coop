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
#define GATE_TEST_LOCKSTEP 5

// Story gates start at 100, numbered in rough story order with gaps left for
// whatever gets inserted later.
#define GATE_STORY_BASE 100

// Birch's bag on Route 101. The one scene deliberately left unguarded, so that
// running it on both consoles gives both players a starter.
#define GATE_STORY_STARTERS (GATE_STORY_BASE + 0)

// Both players have finished choosing one. The scene around it is shared, so
// the faster chooser has to wait or they spend the rest of Birch's dialogue a
// beat apart.
#define GATE_STORY_STARTERS_PICKED (GATE_STORY_BASE + 1)

// Birch being chased across Route 101. Shared, because it is the first thing
// that happens in the game and watching it alone while your partner watches
// their own copy of it is not playing together.
#define GATE_STORY_BIRCH_RESCUE (GATE_STORY_BASE + 2)

// Birch handing the starter over in his lab, and the Pokedex afterwards.
//
// These two are on-frame triggers keyed on VAR_BIRCH_LAB_STATE, and the var
// is shared -- so the first player to finish advances it and the trigger
// stops matching for the other, who is left standing in a lab where nothing
// is going to happen. Mirroring them means it runs once, for both.
#define GATE_STORY_LAB_STARTER (GATE_STORY_BASE + 3)
#define GATE_STORY_LAB_POKEDEX (GATE_STORY_BASE + 4)

// The rest of the opening, map by map. Every one of these is a scene the game
// starts AT the player -- a trigger tile or an on-frame table -- which is
// exactly the kind that cannot be left to one console: your partner's game
// never hears about it, and several of them move a shared var on the way out,
// so the first player through takes the scene away from the second.
#define GATE_STORY_GO_SAVE_BIRCH      (GATE_STORY_BASE + 5)
#define GATE_STORY_RUNNING_SHOES      (GATE_STORY_BASE + 6)
#define GATE_STORY_MEET_RIVAL         (GATE_STORY_BASE + 7)
#define GATE_STORY_NEW_NEIGHBOUR      (GATE_STORY_BASE + 8)
#define GATE_STORY_GYM_REPORT         (GATE_STORY_BASE + 9)
#define GATE_STORY_OLDALE_RIVAL       (GATE_STORY_BASE + 10)
#define GATE_STORY_PETALBURG_GYM_SHOW (GATE_STORY_BASE + 11)
#define GATE_STORY_WALLY_HOUSE        (GATE_STORY_BASE + 12)
#define GATE_STORY_WALLY_TUTORIAL     (GATE_STORY_BASE + 13)
#define GATE_STORY_WALLY_SURF         (GATE_STORY_BASE + 14)
#define GATE_STORY_SCOTT_PETALBURG    (GATE_STORY_BASE + 15)
#define GATE_STORY_PETALBURG_WOODS    (GATE_STORY_BASE + 16)

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
