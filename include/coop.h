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
    u16 vars[VARS_COUNT];
    u8 dexSeen[NUM_DEX_FLAG_BYTES];
    u8 dexCaught[NUM_DEX_FLAG_BYTES];

    // Bag and PC items. Quantities travel DECRYPTED -- see GatherWorldState for
    // why that is not optional.
    struct Bag bag;
    struct ItemSlot pcItems[PC_ITEMS_COUNT];

    // Decrypted, like the bag quantities above: the two consoles have
    // different encryption keys, so the stored form means nothing across the
    // link.
    u32 money;

    // Which half of the world this save belongs to, and with it the whole
    // presentation: menu frames, battle backgrounds, music, door animations
    // and the overworld skin all key off it. Player 1 owns the save, so Player
    // 1 owns the region.
    //
    // Player 2 cannot know it before joining -- their console starts the
    // moment the pair is up, which is before a single byte has crossed the
    // link -- so they start in Hoenn and adopt this. Left out, the two phones
    // sat in different regions wearing different skins for the whole session.
    u8 playerRegion;
}; // about 1.6 KB

// Live changes, broadcast as they happen.
//
// The join sync brings a player up to date once. Without this, that is ALL it
// does: beat a gym and your partner's game never hears about it, and when
// Player 1 saves, everything Player 2 achieved is gone. Shared progression that
// only works at the moment of joining is not shared progression.
enum CoopDeltaKind
{
    COOP_DELTA_FLAG,
    COOP_DELTA_VAR,
    COOP_DELTA_DEX_SEEN,
    COOP_DELTA_DEX_CAUGHT,
    // The bag. Sent as the change rather than the resulting slot, because the
    // two consoles hold their quantities XOR'd under different keys and a slot
    // copied across raw is not slightly wrong, it is meaningless. Applied
    // through the game's own AddBagItem/RemoveBagItem, which encrypt under the
    // receiver's key and find the right pocket without being told.
    COOP_DELTA_ITEM_ADD,
    COOP_DELTA_ITEM_REMOVE,
    COOP_DELTA_MONEY,
};

// ---------------------------------------------------------------------------
// Pokemon storage.
//
// Player 2 has no save file, so anything they box would be lost -- and worse,
// their view of the boxes would drift from Player 1's, so depositing into what
// looks like an empty slot could overwrite a Pokemon they cannot see.
//
// So the boxes are shared the way flags and the bag are: every write to a slot
// is broadcast as it happens, and a joining player is brought up to date. Only
// the OCCUPIED slots are sent at join -- the storage is 34 KB and this
// transport moves about 800 bytes a second, so copying all of it would hold a
// joining player for three quarters of a minute, while copying what is actually
// in it costs a second or two.
// ---------------------------------------------------------------------------

/** Record a box slot write for broadcast. Called from the storage system. */
void Coop_QueueBoxWrite(u8 boxId, u8 position, const struct BoxPokemon *mon);

/** Record a box slot being emptied. */
void Coop_QueueBoxClear(u8 boxId, u8 position);

/** Apply a partner's box change. Called when its transfer completes. */
void Coop_ApplyBoxOp(const void *data);

/** Begin sending every occupied box slot to a joining player. */
void Coop_BeginBoxJoinSync(void);

/** Feed queued box changes onto the wire. Called once per frame. */
void Coop_UpdateBoxSync(void);

/**
 * Record a change for broadcast. Called from the game's own mutators.
 *
 * Safe to call at any time: it does nothing unless a co-op session is running,
 * and nothing while a received change is being applied -- otherwise applying a
 * partner's flag would queue it straight back and the two would echo forever.
 */
void Coop_QueueDelta(u8 kind, u16 id, u16 value);

/** Apply a change from the partner. Called from ProcessRecvCmds. */
void Coop_ReceiveDelta(u8 playerId, const u16 *cmd);

// The local id the partner's map object is spawned under.
//
// Well clear of the 1..n range maps use for their own object events, so it
// cannot collide with a real NPC's id on any map.
#define COOP_PEER_LOCAL_ID 0xF0

// Sync gates: a point in a script neither player passes alone.
//
// Story doors and cutscenes use these. A player who arrives first waits on a
// dark screen until the other reaches the same gate, then both carry on
// together.
//
// Gate ids are chosen by whoever writes the script, and both consoles must use
// the same one for the same moment -- which they do automatically, because both
// run the same script from the same ROM.

/** Arrive at a gate and start waiting. */
void Coop_BeginGate(u16 gateId);

/** TRUE once the partner has reached the same gate. Polled each frame. */
bool8 Coop_GateIsOpen(void);

/** TRUE while this console is sat at a gate -- used to draw the wait screen. */
bool8 Coop_IsWaitingAtGate(void);

/**
 * Put up "Waiting for your partner" once the wait has gone on a moment, and
 * take it down again. Called from the gate's native wait.
 *
 * Without it a gate is indistinguishable from a crash: the game stops and
 * nothing says why. Does nothing if a message box is already up, so a gate in
 * the middle of a scene leaves the scene's own dialogue alone.
 */
void Coop_UpdateWaitMessage(void);
void Coop_EndWaitMessage(void);

/** Called once per frame from the overworld to run the gate handshake. */
void Coop_UpdateGate(void);

/** Unpack a partner's gate broadcast. Called from ProcessRecvCmds. */
void Coop_ReceiveGate(u8 playerId, const u16 *cmd);

/**
 * A gate that gives up.
 *
 * For the gate at the START of a scene only, where nothing has happened yet and
 * walking away is clean. An ordinary mid-scene gate must never time out: half a
 * cutscene on one console and all of it on the other is worse than a pause.
 */
void Coop_BeginSceneGate(u16 gateId);

/**
 * A gate that waits much longer before giving up.
 *
 * For waiting on the other player to finish CHOOSING something. A scene gate's
 * ten seconds answers "did the scene reach them", which takes a frame or two;
 * it is hopeless for a menu a person is reading and changing their mind in.
 */
void Coop_BeginReadyGate(u16 gateId);

/** TRUE once a scene gate has waited long enough to give up on. */
bool8 Coop_GateTimedOut(void);

// ---------------------------------------------------------------------------
// Scene mirroring.
//
// A gate on its own is not enough for a story scene, and the reason is easy to
// miss: when you step on a trigger tile, only YOUR console runs that script.
// Your partner's game has no idea a scene started, so it never reaches the
// gate, and a gate nobody else can arrive at is a game that stops for ever.
//
// So the console that triggers a scene tells the other one where the script is.
// A ROM address is a perfectly good name for it: both consoles are running the
// same ROM, so the same address is the same scene on both, with nothing to
// negotiate and no table to keep in step.
// ---------------------------------------------------------------------------

/**
 * Mirror a scene onto the partner's console.
 *
 * `resume` is where THEY start, which is past the broadcast command, so the
 * mirrored copy does not broadcast it back.
 *
 * Returns FALSE if the scene must not run at all -- there is a partner, but
 * they are not here to see it. Returns TRUE with nothing sent when there is no
 * partner at all, so single-player keeps working.
 */
bool8 Coop_BroadcastScene(const u8 *resume, u16 gateId);

/**
 * Is the other player standing on this map?
 *
 * TRUE when nobody has joined, so single-player is never blocked by a rule
 * about a partner who does not exist.
 */
bool8 Coop_PartnerIsHere(void);

/** Unpack a scene broadcast. Called from ProcessRecvCmds. */
void Coop_ReceiveScene(u8 playerId, const u16 *cmd);

/**
 * Start a mirrored scene, once this console is in a fit state to.
 *
 * Called once per frame. A scene cannot just be started the moment it arrives:
 * this console might be mid-conversation with an NPC of its own, or mid-step,
 * or warping. The partner is sat at the scene's opening gate waiting, which is
 * what makes waiting for a quiet frame safe.
 */
void Coop_UpdatePendingScene(void);

/**
 * True while running a scene this console was HANDED rather than triggered.
 *
 * Mirroring means a scene's effects happen twice, once on each console, and
 * most of them do not mind: setting a flag that is already set, or a Pokedex
 * entry, or a var to the value it already holds, all land on the same answer
 * however many times they run. Handing over an item does not. Nor does handing
 * over a Pokemon -- though there that is sometimes exactly what is wanted, which
 * is why this is a question a script asks rather than a rule applied to it.
 *
 * So: leave a scene alone and both players get one each, which is right for the
 * starters. Guard the giving part with goto_if_coop_guest and only the player
 * who triggered it gets one, which is right for everything else.
 */
bool8 Coop_IsSceneGuest(void);

/**
 * True while BOTH consoles are already inside the same mirrored script.
 *
 * Scripts goto each other freely, so a scene that has been mirrored can run
 * straight into another script that also begins with coopscene. Mirroring that
 * one too would have the guest broadcast the scene back at the host. When this
 * is true the inner coopscene skips its broadcast and behaves as a plain gate,
 * which is all it needs to be: both consoles are already running the script.
 */
bool8 Coop_IsInMirroredScene(void);

/**
 * True if this object event is the co-op partner.
 *
 * Used to exempt them from collision in both directions. See the call site in
 * GetObjectObjectCollidesWith for why that is load-bearing rather than a
 * nicety.
 */
bool8 Coop_IsPartnerObject(const struct ObjectEvent *obj);

/**
 * "I am travelling -- come with me."
 *
 * Called by the four moves that cross the map: Fly, Teleport, Dig and an
 * Escape Rope. The partner follows once the traveller has arrived somewhere;
 * no destination is passed, because at the moment these are called the game
 * does not always know it yet.
 */
void Coop_FollowMe(void);
void Coop_ReceiveFollow(u8 playerId, const u16 *cmd);

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

/**
 * True when this console is the joining player (Player 2).
 *
 * Readable before the game starts: the wrapper writes the slot into the
 * mailbox at boot. Player 2 has no save of their own -- their character lives
 * inside Player 1's -- so their console skips the menu and the intro entirely.
 */
bool8 Coop_IsJoiningPlayer(void);

/**
 * Silence outgoing sync while this console rewrites its own world wholesale.
 *
 * For Player 2's intro-free start, which initialises a save from nothing with
 * the link already up. Everything it writes is about to be replaced by
 * Player 1's world anyway.
 */
void Coop_SuppressSync(bool8 suppress);
extern u8 gCoopDbgJoinEntry;
extern u8 gCoopDbgMenuAction;

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

/**
 * Hand the link over to a battle, and take it back afterwards.
 *
 * A co-op battle is a real link battle, and the battle machinery owns the link
 * completely while it runs -- it closes it, reopens it, runs its own player
 * exchange and then ships every controller command over the block layer. The
 * session layer cannot keep broadcasting positions underneath that, so it parks
 * itself until the battle is over.
 *
 * Both consoles must suspend at the same moment, which is what the sync gate in
 * front of a co-op battle is for.
 */
void Coop_SuspendForBattle(void);
void Coop_ResumeAfterBattle(void);
bool8 Coop_IsSuspendedForBattle(void);

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
