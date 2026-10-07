#ifndef GUARD_COOP_H
#define GUARD_COOP_H

#include "global.h"

// ---------------------------------------------------------------------------
// Co-op session: a link that stays up during ordinary overworld play.
//
// The game's own link rooms (Trade Center, Colosseum) hold a link open too,
// but they do it by replacing CB1_Overworld with CB1_OverworldLink, which
// swaps the whole player-movement system for a cut-down one that can only
// walk. That is why those rooms have no running, bikes, surfing or ledges --
// nobody needed them in a trade room.
//
// Co-op keeps CB1_Overworld, so both players run the real game with every
// movement system intact, and we send each other our positions instead of our
// button presses. Keeping CB1_Overworld also means IsOverworldLinkActive()
// stays FALSE, which is what preserves the normal Start menu, the Save option,
// and script lock/lockall.
// ---------------------------------------------------------------------------

enum CoopState
{
    // No co-op. Either the host is not a co-op wrapper at all, or the second
    // player has not joined yet.
    COOP_STATE_OFF,
    // Both players present; bringing the link up.
    COOP_STATE_OPENING,
    // Link open, waiting for the player data exchange to complete. Until it
    // does, gReceivedRemoteLinkPlayers is clear and nothing may be sent.
    COOP_STATE_EXCHANGING,
    // Running: positions are being broadcast every frame.
    COOP_STATE_ACTIVE,
    // The peer went away. Distinct from OFF so the game can say so.
    COOP_STATE_LOST,
};

// What one player broadcasts about itself, once per frame.
struct CoopPeer
{
    bool8 valid;        // have we ever received anything from them
    u8 mapGroup;
    u8 mapNum;
    u8 facing;          // DIR_*
    u16 x;              // map coords, MAP_OFFSET already applied
    u16 y;
    u8 elevation;
    u8 avatarState;     // PLAYER_AVATAR_STATE_* -- walking, biking, surfing...
    u8 gender;
    bool8 moving;       // mid-step, so the sprite should animate rather than idle
    u16 lastSeenFrame;  // for noticing a peer that has gone quiet
};

extern struct CoopPeer gCoopPeer;

// The world both players share, as it goes over the wire.
//
// Not a save structure: there is exactly one copy of each of these in
// SaveBlock1 already, and giving Player 2 a second copy is how two copies drift
// apart. This is the shape used to bring a joining player up to date.
//
// Flags carry all eight badges -- they are plain flags, FLAG_BADGE01_GET and
// friends -- along with every story flag in the game, so syncing this array is
// what makes it one playthrough rather than two.
struct CoopWorldState
{
    u8 flags[NUM_FLAG_BYTES];
    u8 dexSeen[NUM_DEX_FLAG_BYTES];
    u8 dexCaught[NUM_DEX_FLAG_BYTES];
}; // 653 bytes

/** Player 2's stored character, inside Player 1's save. */
struct CoopPlayer2 *GetCoopPlayer2(void);

/**
 * True while a co-op session is running.
 *
 * Deliberately NOT the same question as "is a link active". Co-op is a link,
 * but it is not the kind of link that menus like the bag mean when they ask --
 * those are asking "am I in a trade room, where items must be restricted".
 * Everyday play must stay fully available during co-op, so anything gating on
 * gReceivedRemoteLinkPlayers needs to exclude this case explicitly.
 */
bool8 IsCoopLinkActive(void);

/** True once both players are present, whether or not the link is up yet. */
bool8 IsCoopSessionPaired(void);

/**
 * True whenever the co-op session layer owns the link -- from the moment it
 * starts opening one until it gives up, not merely while the link is running.
 *
 * This is the right question to ask before turning a dropped peer into a fatal
 * "Communication error". IsCoopLinkActive is narrower: it means the handshake
 * finished. Gating the error on that one meant the entire opening and exchange
 * phase was unprotected, so any blip there -- a relay waking from idle, a phone
 * switching network -- killed the session outright, at exactly the point where
 * the session layer was most able to simply retry.
 */
bool8 IsCoopSessionEngaged(void);

u8 GetCoopState(void);

/** Called once per frame from the overworld. Drives the state machine. */
void Coop_Update(void);

/** Reset everything; used when a session ends or the peer is lost. */
void Coop_Reset(void);

/** Unpack a position broadcast. Called from ProcessRecvCmds. */
void Coop_ReceivePosition(u8 playerId, const u16 *cmd);

/**
 * Spawn, move or despawn the partner's sprite to match what we last heard.
 * Called once per frame from the overworld, after the field has updated.
 */
void Coop_UpdatePeerSprite(void);

#endif // GUARD_COOP_H
