#include "global.h"
#include "coop.h"
#include "coop_sync.h"
#include "link.h"
#include "net_link.h"
#include "task.h"
#include "overworld.h"
#include "event_object_movement.h"
#include "field_player_avatar.h"
#include "script.h"
#include "field_screen_effect.h"
#include "constants/maps.h"
#include "constants/flags.h"
#include "constants/vars.h"
#include "event_data.h"
#include "pokedex.h"
#include "item.h"
#include "field_message_box.h"
#include "field_control_avatar.h"
#include "event_scripts.h"
#include "constants/event_objects.h"

// ---------------------------------------------------------------------------
// Co-op session state machine. See include/coop.h for why this exists rather
// than reusing the game's own link-room machinery.
// ---------------------------------------------------------------------------

EWRAM_DATA struct CoopPeer gCoopPeer = {0};
static EWRAM_DATA u8 sCoopState = 0;
// Defined with the sprite code below; declared here so the diagnostics can
// report whether the partner is currently spawned.
static EWRAM_DATA u8 sPeerObjectId;
// Cleared with the rest of the session state, so a reconnect re-sends.
// Where the join handshake has got to. Both consoles run the same enum through
// different branches, which is why the names describe the step rather than the
// side.
enum CoopJoinStep
{
    COOP_JOIN_SEND_PLAYER2,
    COOP_JOIN_SEND_WORLD,
    COOP_JOIN_AWAIT_PLAYER2,
    COOP_JOIN_DONE,
};
static EWRAM_DATA u8 sJoinStep = COOP_JOIN_SEND_PLAYER2;
static EWRAM_DATA struct CoopWorldState sOutgoingWorld = {0};
// Player 2 only: which halves of the join have arrived. Tracked separately
// because the two arrive independently and in no guaranteed order.
static EWRAM_DATA bool8 sGotPlayer2 = FALSE;
static EWRAM_DATA bool8 sGotWorld = FALSE;
// Staging for an outgoing record. The transfer reads its source across
// many frames, so it cannot point at a caller's stack.
static EWRAM_DATA struct CoopPlayer2 sOutgoingRecord = {0};
// Player 2 only: the stored character handed back by Player 1, waiting to be
// taken over. Held rather than applied immediately because adopting an identity
// and party needs a safe moment, not whichever frame the last chunk landed on.
static EWRAM_DATA struct CoopPlayer2 sPendingRecord = {0};
static EWRAM_DATA bool8 sHasPendingRecord = FALSE;
static EWRAM_DATA u16 sStateTimer = 0;

// The player data exchange normally gets 600 frames (10s) before the cable
// code calls it a timeout. The relay has already told us both players are
// present, so a stall here means something is wrong rather than merely slow;
// still, be generous enough to survive a bad moment on mobile data.
#define EXCHANGE_TIMEOUT_FRAMES 900

// How long without hearing from the peer before we treat them as gone. The
// transport delivers nothing at all when a frame is missing, so silence is the
// only signal available.
#define PEER_SILENCE_FRAMES 300 // 5 seconds

// ---------------------------------------------------------------------------
// The shared save.
//
// Player 2 has no save file. Their character lives inside Player 1's, and the
// session hands it back to them when they reconnect. Everything SHARED --
// badges, the Pokedex, the bag, the PC -- is deliberately absent from that
// record: shared state has exactly one copy, in the surrounding SaveBlock1, and
// a second copy is how two copies drift apart.
// ---------------------------------------------------------------------------

bool8 Coop_IsPartnerObject(const struct ObjectEvent *obj)
{
    return obj != NULL && obj->active && obj->localId == COOP_PEER_LOCAL_ID;
}

struct CoopPlayer2 *GetCoopPlayer2(void)
{
    return &gSaveBlock1Ptr->coopPlayer2;
}

// ---------------------------------------------------------------------------
// Live changes.
//
// The join sync is a snapshot. On its own it means a gym beaten by Player 2 is
// news that never reaches Player 1, and vanishes the moment Player 1 saves.
// These carry each change as it happens.
//
// One change per frame is plenty: the game sets a handful of flags at a story
// beat, not hundreds, and the ring absorbs the bursts.
// ---------------------------------------------------------------------------

#define COOP_DELTA_SLOTS 64
#define COOP_DELTA_MASK  (COOP_DELTA_SLOTS - 1)

struct CoopDelta
{
    u8 kind;
    u16 id;
    u16 value;
};

static EWRAM_DATA struct CoopDelta sDeltaRing[COOP_DELTA_SLOTS] = {0};
static EWRAM_DATA u8 sDeltaHead = 0;
static EWRAM_DATA u8 sDeltaTail = 0;
// Set while a partner's change is being applied. Without it, applying their
// flag would queue it straight back to them, and the two would bounce the same
// change forever.
static EWRAM_DATA bool8 sApplyingRemote = FALSE;
static EWRAM_DATA u16 sDeltasDropped = 0;

void Coop_QueueDelta(u8 kind, u16 id, u16 value)
{
    u8 next;

    if (!IsCoopLinkActive() || sApplyingRemote)
        return;

    // Temp flags and vars are per-map scratch, cleared on every map change and
    // belonging to whatever script is running on THIS console. Broadcasting
    // them would be noise at best and would stamp on the partner's running
    // script at worst.
    if (kind == COOP_DELTA_FLAG && id < NUM_TEMP_FLAGS)
        return;
    if (kind == COOP_DELTA_VAR && id < VARS_START + NUM_TEMP_VARS)
        return;

    next = (sDeltaHead + 1) & COOP_DELTA_MASK;
    if (next == sDeltaTail)
    {
        // Full. Counted rather than silently discarded: if this is ever
        // non-zero in the diagnostics, one change per frame is too slow and
        // the design needs revisiting, not the buffer resizing.
        sDeltasDropped++;
        return;
    }

    sDeltaRing[sDeltaHead].kind = kind;
    sDeltaRing[sDeltaHead].id = id;
    sDeltaRing[sDeltaHead].value = value;
    sDeltaHead = next;
}

// Emit one queued change. Returns TRUE if it wrote a command.
static bool8 CoopSendDelta(u16 *sendCmd)
{
    const struct CoopDelta *d;

    if (sDeltaHead == sDeltaTail)
        return FALSE;

    d = &sDeltaRing[sDeltaTail];
    sendCmd[0] = LINKCMD_COOP_DELTA;
    sendCmd[1] = d->kind;
    sendCmd[2] = d->id;
    sendCmd[3] = d->value;
    sDeltaTail = (sDeltaTail + 1) & COOP_DELTA_MASK;
    gNetMailbox.deltasSent++;
    return TRUE;
}

void Coop_ReceiveDelta(u8 playerId, const u16 *cmd)
{
    u8 kind = cmd[1];
    u16 id = cmd[2];
    u16 value = cmd[3];

    // Our own changes come back looped, as the cable did. Applying them is
    // harmless but queueing them again is not.
    if (playerId == GetMultiplayerId())
        return;

    sApplyingRemote = TRUE;

    switch (kind)
    {
    case COOP_DELTA_FLAG:
        if (value)
            FlagSet(id);
        else
            FlagClear(id);
        break;
    case COOP_DELTA_VAR:
        VarSet(id, value);
        break;
    case COOP_DELTA_DEX_SEEN:
        GetSetPokedexFlag(id, FLAG_SET_SEEN);
        break;
    case COOP_DELTA_DEX_CAUGHT:
        GetSetPokedexFlag(id, FLAG_SET_CAUGHT);
        break;
    case COOP_DELTA_ITEM_ADD:
        AddBagItem(id, value);
        break;
    case COOP_DELTA_ITEM_REMOVE:
        RemoveBagItem(id, value);
        break;
    }

    sApplyingRemote = FALSE;
    gNetMailbox.deltasRecv++;
}

// ---------------------------------------------------------------------------
// Sync gates.
//
// A point in a script that neither player passes alone. The script blocks on
// Coop_GateIsOpen, so the game simply stops where it is until the partner
// arrives at the same gate.
//
// The protocol is one command: "I am at gate G, and this is the Nth gate I
// have arrived at". The transport below is ordered and lossless -- it is a ring
// the relay drains in order, not a datagram socket -- so a gate id broadcast
// once WILL arrive, and the peer's last report can simply be remembered. That
// is what makes this need no retransmission and no acknowledgement: whoever
// arrives second finds the first player's report already recorded and opens
// immediately, and whoever arrives first is opened by the report that follows.
//
// The arrival counter is the part that is easy to leave out and wrong to.
// Without it, a remembered report is indistinguishable from a present partner:
// walk back into a door you have both already been through and your own console
// would see the peer's stale "I am at gate G" and open the gate with the
// partner three towns away. The counter changes on every arrival, so a report
// can be recognised as one already used.
// ---------------------------------------------------------------------------

// The gate this console is sat at, or 0. Gate id 0 is reserved for "none", so
// scripts number from 1.
static EWRAM_DATA u16 sGateId = 0;
// Which arrival this is, counting from 1 and skipping 0 on wrap. Sent with the
// gate id so the peer can tell a fresh arrival from a remembered one.
static EWRAM_DATA u16 sGateSeq = 0;
static EWRAM_DATA bool8 sGateOpen = FALSE;
// Frames spent waiting. For the wait screen and the diagnostics.
static EWRAM_DATA u16 sGateWaitFrames = 0;

// The arrival still to be announced, held apart from the live gate above.
//
// It has to be separate, and the reason is the case that looks like it needs no
// announcement at all: the partner got here first, so our gate opens on the
// frame we arrive and the script walks straight on. Tie the broadcast to the
// live gate and that is precisely when it is cancelled before being sent -- and
// the partner, who is waiting to hear from US, waits for ever. Arriving at a
// gate is announced whether or not we stop at it.
static EWRAM_DATA u16 sGateSendId = 0;
static EWRAM_DATA u16 sGateSendSeq = 0;
// How many more times to put it on the wire. One would do, given ordered
// delivery; more costs nothing and covers a reconnect clearing the ring
// mid-wait.
static EWRAM_DATA u8 sGateSendsLeft = 0;

// The peer's last report, remembered across their arrivals and ours.
static EWRAM_DATA u16 sPeerGateId = 0;
static EWRAM_DATA u16 sPeerGateSeq = 0;
// The peer arrival that opened our last gate. Compared against, so that the
// same report cannot open two gates.
static EWRAM_DATA u16 sUsedPeerGateSeq = 0;

#define GATE_SEND_REPEATS 4

// How long a scene's opening gate waits before giving up. Generous, because the
// usual reason the partner is slow is that they are mid-conversation with an NPC
// of their own, and that is not a fault. Only the opening gate ever uses this.
#define SCENE_GATE_TIMEOUT_FRAMES 600 // 10 seconds

// A gate waiting on the other player to finish CHOOSING gets far longer.
//
// Ten seconds is right for "did the scene reach them" -- a question answered in
// a frame or two when all is well. It is hopeless for "have they picked their
// three yet", which is a menu a person reads, scrolls and changes their mind
// in. At ten seconds the first player to confirm abandoned the battle while the
// second was still on the party screen.
#define READY_GATE_TIMEOUT_FRAMES 3600 // one minute

// Whether this gate is allowed to give up, and whether it has.
static EWRAM_DATA bool8 sGateCanTimeOut = FALSE;
static EWRAM_DATA u16 sGateTimeoutFrames = 0;
static EWRAM_DATA bool8 sGateTimedOut = FALSE;

// Has the partner reported arriving at our gate, with an arrival we have not
// already spent?
static bool8 PeerIsAtOurGate(void)
{
    return sPeerGateId == sGateId && sPeerGateSeq != sUsedPeerGateSeq;
}

void Coop_BeginSceneGate(u16 gateId)
{
    Coop_BeginGate(gateId);
    sGateCanTimeOut = TRUE;
    sGateTimeoutFrames = SCENE_GATE_TIMEOUT_FRAMES;
}

void Coop_BeginReadyGate(u16 gateId)
{
    Coop_BeginGate(gateId);
    sGateCanTimeOut = TRUE;
    sGateTimeoutFrames = READY_GATE_TIMEOUT_FRAMES;
}

bool8 Coop_GateTimedOut(void)
{
    return sGateTimedOut;
}

void Coop_BeginGate(u16 gateId)
{
    sGateId = gateId;
    sGateWaitFrames = 0;
    sGateCanTimeOut = FALSE;
    sGateTimedOut = FALSE;

    if (++sGateSeq == 0)
        sGateSeq = 1;

    sGateSendId = gateId;
    sGateSendSeq = sGateSeq;
    sGateSendsLeft = GATE_SEND_REPEATS;

    // Whether there is a partner is asked once, here, and not again.
    //
    // The two cases look identical from any single frame and want opposite
    // answers. Nobody ever joined: open, or the ROM hangs on the first story
    // beat the moment it is played on its own. The partner was here and
    // dropped: keep waiting, because the alternative is one console walking the
    // shared story forward alone and there is one save between the two of them.
    // That case recovers -- a reconnect brings them back to the same gate -- so
    // waiting costs a pause while opening costs a playthrough. Asking every
    // frame would answer the first question with the second one's facts.
    if (!IsCoopSessionEngaged())
    {
        sGateOpen = TRUE;
        return;
    }

    // Open at once if the partner is already sat here. Their report arrived
    // before we did and has been waiting for us.
    sGateOpen = PeerIsAtOurGate();
    if (sGateOpen)
        sUsedPeerGateSeq = sPeerGateSeq;
}

bool8 Coop_GateIsOpen(void)
{
    if (sGateId == 0)
        return TRUE;
    if (!sGateOpen)
        return FALSE;

    // This call is the one that releases the script, so the gate is done with.
    // The announcement is not: it lives in sGateSend* and goes out regardless.
    sGateId = 0;
    sGateWaitFrames = 0;
    return TRUE;
}

bool8 Coop_IsWaitingAtGate(void)
{
    return sGateId != 0 && !sGateOpen;
}

// The handshake runs here, once a frame, rather than inside Coop_GateIsOpen.
//
// Putting it in the getter looked tidier and was wrong twice over: a predicate
// that only resolves when something happens to ask it is a predicate that does
// not resolve while the asking script is the thing being blocked, and it cannot
// be observed by anything that is not a script -- which is exactly what the
// test rig is.
void Coop_UpdateGate(void)
{
    if (sGateId == 0 || sGateOpen)
        return;

    if (PeerIsAtOurGate())
    {
        sUsedPeerGateSeq = sPeerGateSeq;
        sGateOpen = TRUE;
        return;
    }

    if (sGateWaitFrames < 0xFFFF)
        sGateWaitFrames++;

    if (sGateCanTimeOut && sGateWaitFrames >= sGateTimeoutFrames)
        sGateTimedOut = TRUE;
}

// ---------------------------------------------------------------------------
// Telling the waiting player what is going on.
//
// Without this a gate is indistinguishable from a crash: the game simply stops
// and nothing on screen says why. That is the whole experience of the feature
// for whoever gets there first, so it is not a polish item.
//
// A message box rather than the dark screen the design called for. It is the
// game's own furniture, it leaves the world visible behind it, and it does not
// involve driving a palette fade from inside a script that is mid-cutscene.
// ---------------------------------------------------------------------------

// Long enough that a gate whose partner is already waiting -- which opens on
// the frame it is reached -- does not flash a box up and take it away again.
#define WAIT_MESSAGE_DELAY_FRAMES 30

static EWRAM_DATA bool8 sShowingWaitMessage = FALSE;

void Coop_UpdateWaitMessage(void)
{
    if (sShowingWaitMessage || !Coop_IsWaitingAtGate())
        return;

    if (sGateWaitFrames < WAIT_MESSAGE_DELAY_FRAMES)
        return;

    // Returns FALSE when a box is already up, which is exactly right for a
    // gate in the middle of a scene: the scene's own dialogue stays, and we
    // simply do not get one. Paired with the flag below, that is also what
    // stops us closing a box we did not open.
    if (ShowFieldMessage(CoopText_WaitingForPartner))
        sShowingWaitMessage = TRUE;
}

void Coop_EndWaitMessage(void)
{
    if (!sShowingWaitMessage)
        return;

    HideFieldMessageBox();
    sShowingWaitMessage = FALSE;
}

// Announce our arrival, if it still needs announcing. Returns TRUE if it wrote
// a command.
static bool8 CoopSendGate(u16 *sendCmd)
{
    if (sGateSendId == 0 || sGateSendsLeft == 0)
        return FALSE;

    sendCmd[0] = LINKCMD_COOP_GATE;
    sendCmd[1] = sGateSendId;
    sendCmd[2] = sGateSendSeq;
    sGateSendsLeft--;

    return TRUE;
}

void Coop_ReceiveGate(u8 playerId, const u16 *cmd)
{
    if (playerId == GetMultiplayerId())
        return;

    sPeerGateId = cmd[1];
    sPeerGateSeq = cmd[2];
}

// ---------------------------------------------------------------------------
// Scene mirroring.
//
// The console that triggers a story scene tells the other one where the script
// is, as a ROM address -- the same address is the same scene on both, because
// both are running the same ROM.
//
// The address travels as an offset from the ROM base in two halfwords. 32 MB of
// ROM does not fit in one.
// ---------------------------------------------------------------------------

#define ROM_BASE 0x08000000

// Waiting to be sent. One slot: a console can only trigger one scene at a time,
// because triggering one locks its field controls.
static EWRAM_DATA const u8 *sSceneSendPtr = NULL;
static EWRAM_DATA u16 sSceneSendGate = 0;

// Which object event the scene was started by talking to, if any.
//
// Most story scenes begin with `lock` and `faceplayer`, and both of those act
// on whatever this console last talked to -- which, on the console that was
// handed the scene, is some unrelated NPC it spoke to ten minutes ago, or
// nothing. So the trigger says who it was.
//
// It travels as the map's local id, not as the object event index. The index
// is an allocation slot and the two consoles do not allocate alike: the co-op
// partner takes one of them. The local id comes from the map data and is the
// same number on both.
static EWRAM_DATA u16 sSceneSendLocalId = 0;

// Received, waiting for a frame where starting it is safe.
static EWRAM_DATA const u8 *sPendingScene = NULL;
static EWRAM_DATA u16 sPendingSceneFrames = 0;
static EWRAM_DATA u16 sPendingSceneLocalId = 0;

// Set while running a scene handed to us rather than triggered by us. Cleared
// when that script finishes, which is the only honest end for it: a script can
// stop at any one of a hundred commands, so the state has to be watched out
// rather than reset by whatever is presumed to be the last one.
static EWRAM_DATA bool8 sInGuestScene = FALSE;

// Set on the console that STARTED a mirrored scene, for as long as it runs.
// Paired with sInGuestScene it answers "are both consoles already inside the
// same script", which is what makes a nested coopscene safe.
static EWRAM_DATA bool8 sInHostScene = FALSE;

// Set while a co-op battle owns the link. See Coop_SuspendForBattle.
static EWRAM_DATA bool8 sSuspendedForBattle = FALSE;

// How long a received scene waits for a safe frame before being dropped. Paired
// with the sender's gate timeout: the sender gives up at the same point, so
// neither side is left holding half an agreement.
#define PENDING_SCENE_TIMEOUT_FRAMES SCENE_GATE_TIMEOUT_FRAMES

static u16 OurMapWord(void)
{
    return gSaveBlock1Ptr->location.mapGroup
         | ((u16)gSaveBlock1Ptr->location.mapNum << 8);
}

static u16 PeerMapWord(void)
{
    return gCoopPeer.mapGroup | ((u16)gCoopPeer.mapNum << 8);
}

bool8 Coop_PartnerIsHere(void)
{
    // Nobody has joined. Everything is allowed, because a ROM that refuses to
    // play its own story when there is no second player is not playable at
    // all -- and that is the state it boots in.
    if (!IsCoopSessionEngaged())
        return TRUE;

    return IsCoopLinkActive() && gCoopPeer.valid && PeerMapWord() == OurMapWord();
}

bool8 Coop_BroadcastScene(const u8 *resume, u16 gateId)
{
    // There IS a partner and they are somewhere else: the scene does not
    // happen. The trigger is left unfired, so it runs again when they are both
    // here. (Or nobody has joined, in which case this is TRUE and the scene
    // plays as it would single-player.)
    if (!Coop_PartnerIsHere())
        return FALSE;

    if (!IsCoopSessionEngaged())
        return TRUE;

    sSceneSendPtr = resume;
    sSceneSendGate = gateId;
    sSceneSendLocalId = gSpecialVar_LastTalked;
    sInHostScene = TRUE;
    return TRUE;
}

// Emit a queued scene. Returns TRUE if it wrote a command.
static bool8 CoopSendScene(u16 *sendCmd)
{
    u32 off;

    if (sSceneSendPtr == NULL)
        return FALSE;

    off = (u32)sSceneSendPtr - ROM_BASE;

    sendCmd[0] = LINKCMD_COOP_SCENE;
    sendCmd[1] = sSceneSendGate;
    sendCmd[2] = off & 0xFFFF;
    sendCmd[3] = off >> 16;
    // Our map, so the receiver can refuse a scene for a map it is not on. They
    // were on it when we checked, but a warp one frame later is a scene whose
    // object events and coordinates belong somewhere else.
    sendCmd[4] = OurMapWord();
    sendCmd[5] = sSceneSendLocalId;

    sSceneSendPtr = NULL;
    return TRUE;
}

void Coop_ReceiveScene(u8 playerId, const u16 *cmd)
{
    u32 off;

    if (playerId == GetMultiplayerId())
        return;

    if (cmd[4] != OurMapWord())
        return;

    off = cmd[2] | ((u32)cmd[3] << 16);

    // A script pointer arriving over a wire gets checked before it is jumped
    // to. Everything below trusts it completely -- it goes straight into the
    // script interpreter -- so a corrupt value here is not a wrong scene, it is
    // arbitrary bytes run as bytecode.
    if (off >= 0x02000000)
        return;

    sPendingScene = (const u8 *)(ROM_BASE + off);
    sPendingSceneFrames = 0;
    sPendingSceneLocalId = cmd[5];
}

// Point this console at the same NPC the trigger was talking to.
//
// Resolved from the local id rather than copied as an index: both consoles
// have the same map loaded, so the local id finds the same NPC on each, while
// the index is an allocation slot that the co-op partner's own sprite has
// already shifted.
static void AdoptSceneSpeaker(void)
{
    u8 id;

    if (sPendingSceneLocalId == LOCALID_NONE || sPendingSceneLocalId == 0)
        return;

    // The var first and unconditionally: scripts compare VAR_LAST_TALKED, and
    // that comparison has to give the same answer on both consoles whether or
    // not the NPC happens to be spawned on this one.
    gSpecialVar_LastTalked = sPendingSceneLocalId;

    id = GetObjectEventIdByLocalIdAndMap(sPendingSceneLocalId,
                                         gSaveBlock1Ptr->location.mapNum,
                                         gSaveBlock1Ptr->location.mapGroup);
    if (id < OBJECT_EVENTS_COUNT)
        gSelectedObjectEvent = id;
}

void Coop_UpdatePendingScene(void)
{
    if (sPendingScene == NULL)
        return;

    if (++sPendingSceneFrames >= PENDING_SCENE_TIMEOUT_FRAMES)
    {
        // Give up at the same point the sender does. Their gate has timed out
        // too, so the scene is off on both sides and its trigger is still
        // unfired.
        sPendingScene = NULL;
        return;
    }

    // Wait for a frame where starting a script is safe: nothing else running,
    // the player in control and standing still. The partner is sat at the
    // scene's opening gate, which is what buys the time to wait.
    if (ScriptContext_IsEnabled() || ArePlayerFieldControlsLocked()
        || !IsPlayerStandingStill())
        return;

    AdoptSceneSpeaker();
    ScriptContext_SetupScript(sPendingScene);
    sPendingScene = NULL;
    sInGuestScene = TRUE;
}

bool8 Coop_IsSceneGuest(void)
{
    return sInGuestScene;
}

bool8 Coop_IsInMirroredScene(void)
{
    return sInGuestScene || sInHostScene;
}

// Notice the guest scene ending. Called every frame, before scripts run.
static void UpdateGuestScene(void)
{
    if (!ScriptContext_IsEnabled())
    {
        sInGuestScene = FALSE;
        sInHostScene = FALSE;
    }
}

static void ResetScenes(void)
{
    sSceneSendPtr = NULL;
    sSceneSendGate = 0;
    sPendingScene = NULL;
    sPendingSceneFrames = 0;
    sPendingSceneLocalId = 0;
    sSceneSendLocalId = 0;
    sInGuestScene = FALSE;
    sInHostScene = FALSE;
}

static void ResetGates(void)
{
    // sGateId is deliberately left alone: a script may be sat on it right now,
    // and a dropped link is not a reason to let one console past a story beat
    // on its own. It re-announces instead, so the gate closes again over the
    // new session rather than resolving on stale state.
    sGateOpen = FALSE;
    sGateWaitFrames = 0;
    sGateTimedOut = FALSE;
    if (sGateId != 0)
    {
        sGateSendId = sGateId;
        sGateSendSeq = sGateSeq;
        sGateSendsLeft = GATE_SEND_REPEATS;
    }
    else
    {
        sGateSendId = 0;
        sGateSendsLeft = 0;
    }
    // The peer's report belongs to the session that carried it. Keeping it
    // across a reconnect would let a gate open on a partner who was standing
    // there before the drop and has since walked off.
    sPeerGateId = 0;
    sPeerGateSeq = 0;
    sUsedPeerGateSeq = 0;
}

// The bag as a flat run of slots. Its five pockets are contiguous and all the
// same type, so the whole thing can be walked in one loop rather than five.
#define BAG_SLOT_COUNT (sizeof(struct Bag) / sizeof(struct ItemSlot))

// Snapshot the shared world: badges and story flags, the Pokedex, the bag.
//
// Bag quantities are stored XOR'd with gSaveBlock2Ptr->encryptionKey, and that
// key is drawn from Random32() on every heap reset -- so the two consoles have
// DIFFERENT keys. Copying those bytes across untouched does not give the other
// player slightly wrong quantities, it gives them meaningless ones: a single
// Potion arriving as sixty thousand Potions, or zero. They go over the wire
// decrypted and are re-encrypted on arrival under the receiver's own key.
//
// PC items are not obfuscated at all, so they copy straight across. That
// asymmetry is in the game, not here: BagPocket_SetSlotDataPC applies no XOR
// while the bag equivalent does.
static void GatherWorldState(struct CoopWorldState *out)
{
    const struct ItemSlot *src = (const struct ItemSlot *)&gSaveBlock1Ptr->bag;
    struct ItemSlot *dst = (struct ItemSlot *)&out->bag;
    u32 key = gSaveBlock2Ptr->encryptionKey;
    u16 i;

    for (i = 0; i < NUM_FLAG_BYTES; i++)
        out->flags[i] = gSaveBlock1Ptr->flags[i];

    for (i = 0; i < VARS_COUNT; i++)
        out->vars[i] = gSaveBlock1Ptr->vars[i];

    for (i = 0; i < NUM_DEX_FLAG_BYTES; i++)
    {
        out->dexSeen[i] = gSaveBlock1Ptr->dexSeen[i];
        out->dexCaught[i] = gSaveBlock1Ptr->dexCaught[i];
    }

    for (i = 0; i < BAG_SLOT_COUNT; i++)
    {
        dst[i].itemId = src[i].itemId;
        dst[i].quantity = src[i].quantity ^ (u16)key;
    }

    for (i = 0; i < PC_ITEMS_COUNT; i++)
        out->pcItems[i] = gSaveBlock1Ptr->pcItems[i];
}

// Bring this console up to date with the shared world.
//
// Flags are taken wholesale, because Player 1 owns the save and a joining
// player's own flags are not a second opinion worth merging -- they are the
// leftovers of whatever game its console happened to boot.
//
// Except the temp flags. The first TEMP_FLAGS_SIZE bytes are per-map scratch,
// cleared on every map change and used by whatever script is mid-run right now.
// Overwriting those with another console's scratch would corrupt a script in
// progress, and they carry no shared meaning, so they are left alone.
//
// The Pokedex is merged rather than replaced, by OR. Its flags are only ever
// set -- GetSetPokedexFlag has no clear path at all -- so the union of two
// consoles' dex is exactly right, needs no authority, and cannot lose a catch
// made while the two were apart.
static void ApplyWorldState(const struct CoopWorldState *in)
{
    u16 i;

    for (i = TEMP_FLAGS_SIZE; i < NUM_FLAG_BYTES; i++)
        gSaveBlock1Ptr->flags[i] = in->flags[i];

    // Vars, past the temp block, for the same reason the temp flags are
    // skipped: the first NUM_TEMP_VARS are per-map scratch belonging to
    // whatever script is running right now.
    //
    // NUM_TEMP_VARS, not event_data.c's TEMP_VARS_SIZE -- that one is in BYTES
    // (it is NUM_TEMP_VARS * 2, for a memset) and this is indexing an array of
    // u16. Using it here would have skipped twice as many vars as intended and
    // silently left sixteen real ones unsynced.
    for (i = NUM_TEMP_VARS; i < VARS_COUNT; i++)
        gSaveBlock1Ptr->vars[i] = in->vars[i];

    for (i = 0; i < NUM_DEX_FLAG_BYTES; i++)
    {
        gSaveBlock1Ptr->dexSeen[i] |= in->dexSeen[i];
        gSaveBlock1Ptr->dexCaught[i] |= in->dexCaught[i];
    }

    // Quantities arrived decrypted; re-encrypt under OUR key, which is not the
    // sender's. See GatherWorldState.
    {
        const struct ItemSlot *src = (const struct ItemSlot *)&in->bag;
        struct ItemSlot *dst = (struct ItemSlot *)&gSaveBlock1Ptr->bag;
        u32 key = gSaveBlock2Ptr->encryptionKey;

        for (i = 0; i < BAG_SLOT_COUNT; i++)
        {
            dst[i].itemId = src[i].itemId;
            dst[i].quantity = src[i].quantity ^ (u16)key;
        }
    }

    for (i = 0; i < PC_ITEMS_COUNT; i++)
        gSaveBlock1Ptr->pcItems[i] = in->pcItems[i];
}

// Take over the character Player 1 handed back.
//
// Waits for a quiet frame rather than acting the moment the last chunk lands.
// Replacing the party and the player's identity while a script is mid-cutscene
// or the avatar is mid-step would leave the game referring to a player that no
// longer exists; the two predicates below are the game's own tests for "the
// player is in control and standing still".
static void TryAdoptPendingRecord(void)
{
    struct WarpData warp;
    u8 i;

    if (!sHasPendingRecord)
        return;

    if (ArePlayerFieldControlsLocked() || !IsPlayerStandingStill())
        return;

    // Identity. Set before the warp, because the map reload rebuilds the avatar
    // from playerGender -- doing it after would leave the wrong sprite until
    // the next map change.
    for (i = 0; i < PLAYER_NAME_LENGTH + 1; i++)
        gSaveBlock2Ptr->playerName[i] = sPendingRecord.playerName[i];

    gSaveBlock2Ptr->playerGender = sPendingRecord.playerGender;

    for (i = 0; i < TRAINER_ID_LENGTH; i++)
        gSaveBlock2Ptr->playerTrainerId[i] = sPendingRecord.playerTrainerId[i];

    gSaveBlock2Ptr->playTimeHours = sPendingRecord.playTimeHours;
    gSaveBlock2Ptr->playTimeMinutes = sPendingRecord.playTimeMinutes;
    gSaveBlock2Ptr->playTimeSeconds = sPendingRecord.playTimeSeconds;

    // Party. Clamped because the count came over a wire -- a corrupt value here
    // would be a write past the end of the party array.
    gPlayerPartyCount = sPendingRecord.partyCount <= PARTY_SIZE
                      ? sPendingRecord.partyCount : PARTY_SIZE;
    for (i = 0; i < PARTY_SIZE; i++)
        gPlayerParty[i] = sPendingRecord.party[i];

    // Position. warpId is WARP_ID_NONE so SetPlayerCoordsFromWarp takes the
    // coordinates verbatim, in the same space as gSaveBlock1Ptr->pos -- which
    // is what the record stored. Routed through the full-width setter because
    // the ordinary ones truncate x and y to s8.
    warp.mapGroup = sPendingRecord.location.mapGroup;
    warp.mapNum = sPendingRecord.location.mapNum;
    warp.warpId = WARP_ID_NONE;
    warp.x = sPendingRecord.pos.x;
    warp.y = sPendingRecord.pos.y;

    SetWarpDestinationToWarpData(&warp);
    DoWarp();
    ResetInitialPlayerAvatarState();

    sHasPendingRecord = FALSE;
}

// Snapshot whatever this console's player currently is, in the shape the save
// stores. Player 2 calls this to report itself to Player 1.
//
// Reads the LIVE party (gPlayerParty) rather than the saved copy in SaveBlock1.
// The saved copy is only a snapshot taken at save time, and Player 2 never
// saves -- so it would be stale or, on a fresh boot, empty.
static void GatherLocalPlayerRecord(struct CoopPlayer2 *out)
{
    u8 i;

    for (i = 0; i < PLAYER_NAME_LENGTH + 1; i++)
        out->playerName[i] = gSaveBlock2Ptr->playerName[i];

    out->playerGender = gSaveBlock2Ptr->playerGender;

    for (i = 0; i < TRAINER_ID_LENGTH; i++)
        out->playerTrainerId[i] = gSaveBlock2Ptr->playerTrainerId[i];

    out->playTimeHours = gSaveBlock2Ptr->playTimeHours;
    out->playTimeMinutes = gSaveBlock2Ptr->playTimeMinutes;
    out->playTimeSeconds = gSaveBlock2Ptr->playTimeSeconds;

    out->partyCount = gPlayerPartyCount;
    for (i = 0; i < PARTY_SIZE; i++)
        out->party[i] = gPlayerParty[i];

    out->pos = gSaveBlock1Ptr->pos;
    out->location = gSaveBlock1Ptr->location;

    // Claimed the moment a real player reports themselves. Player 1 uses this
    // to tell "nobody has ever joined" from "the partner I know, currently
    // away", which decides whether a joiner makes a character or gets one back.
    out->claimed = TRUE;
    out->padding = 0;
}

bool8 IsCoopLinkActive(void)
{
    return sCoopState == COOP_STATE_ACTIVE;
}

bool8 IsCoopSessionEngaged(void)
{
    return gNetLinkActive && sCoopState != COOP_STATE_OFF;
}

bool8 IsCoopSessionPaired(void)
{
    return gNetLinkActive && NetLink_GetHostStatus() == NET_HOST_READY
        && NetLink_GetPlayerCount() == NET_MAX_PLAYERS;
}

u8 GetCoopState(void)
{
    return sCoopState;
}

static void CoopSendPositionCB(void);
static void DespawnPeer(void);

// ---------------------------------------------------------------------------
// Handing the link to a battle.
//
// A co-op battle is a real link battle, and the battle machinery expects to own
// the link completely: it closes it, reopens it, runs its own player exchange,
// and then ships every controller command over the block layer for the whole
// fight. The session layer cannot keep broadcasting positions underneath that.
//
// So it stands down. Not Coop_Reset -- that is for a session that ended, and it
// would have the state machine immediately start rebuilding the link the battle
// is trying to negotiate. This parks it instead, and the resume afterwards is
// what rebuilds.
//
// Both consoles must do this at the same moment, which is what the sync gate in
// front of the battle is for.
// ---------------------------------------------------------------------------

void Coop_SuspendForBattle(void)
{
    if (sSuspendedForBattle)
        return;

    sSuspendedForBattle = TRUE;

    // The partner's sprite belongs to the overworld and the battle is about to
    // tear that down around it.
    DespawnPeer();
    gCoopPeer.valid = FALSE;

    // Off rather than parked in a co-op state: IsCoopLinkActive gates the delta
    // queue, and a battle sets plenty of flags that both consoles will set for
    // themselves. Queueing them would fill a ring nothing is draining.
    sCoopState = COOP_STATE_OFF;

    // Only our own callback. The battle installs its own, and clearing one it
    // has already put there would strand it.
    if (gLinkCallback == CoopSendPositionCB)
        gLinkCallback = NULL;
}

void Coop_ResumeAfterBattle(void)
{
    if (!sSuspendedForBattle)
        return;

    sSuspendedForBattle = FALSE;

    // From scratch. The battle left the link closed and gReceivedRemoteLinkPlayers
    // clear, which is exactly the state the session machine starts from.
    Coop_Reset();
}

bool8 Coop_IsSuspendedForBattle(void)
{
    return sSuspendedForBattle;
}

void Coop_Reset(void)
{
    sCoopState = COOP_STATE_OFF;
    sStateTimer = 0;
    gCoopPeer.valid = FALSE;

    // A reconnect must re-send Player 2's record: the two sides may have been
    // apart long enough for it to have changed, and a half-finished transfer
    // from the dropped session would otherwise be stitched into the new one.
    sJoinStep = COOP_JOIN_SEND_PLAYER2;
    sHasPendingRecord = FALSE;
    sGotPlayer2 = FALSE;
    sGotWorld = FALSE;
    // Changes queued before a dropout are stale: the join sync that follows
    // sends the whole world anyway, so replaying them would be redundant at
    // best and would re-apply something since undone at worst.
    sDeltaHead = 0;
    sDeltaTail = 0;
    ResetGates();
    ResetScenes();
    CoopSync_Reset();
}

static bool8 PeerIsOnOurMap(void);
static void PublishDiagnostics(void);

static void EnterState(u8 state)
{
    sCoopState = state;
    sStateTimer = 0;
}

static void PublishDiagnostics(void)
{
    u8 flags = 0;

    if (gLinkStatus & LINK_STAT_CONN_ESTABLISHED) flags |= COOP_DIAG_LINK_OPEN;
    if (gReceivedRemoteLinkPlayers)               flags |= COOP_DIAG_PLAYERS_RECEIVED;
    if (gLinkCallback == CoopSendPositionCB)      flags |= COOP_DIAG_CALLBACK_ARMED;
    if (gCoopPeer.valid)                          flags |= COOP_DIAG_PEER_VALID;
    if (PeerIsOnOurMap())                         flags |= COOP_DIAG_PEER_SAME_MAP;
    if (Coop_IsWaitingAtGate())                   flags |= COOP_DIAG_AT_GATE;
    // Why a mirrored scene is waiting its turn, and what the tests use to
    // stage "your partner is mid-conversation" rather than pressing A and
    // hoping.
    if (ArePlayerFieldControlsLocked() || ScriptContext_IsEnabled())
                                                  flags |= COOP_DIAG_SCRIPT_BUSY;

    gNetMailbox.coopState = sCoopState;
    gNetMailbox.linkFlags = flags;
    gNetMailbox.joinStep = sJoinStep;
    gNetMailbox.deltasDropped = sDeltasDropped;
    gNetMailbox.gateId = sGateId;
    gNetMailbox.peerMap = gCoopPeer.mapGroup | ((u16)gCoopPeer.mapNum << 8);
    gNetMailbox.peerX = gCoopPeer.x;
    gNetMailbox.peerY = gCoopPeer.y;
    gNetMailbox.selfMap = gSaveBlock1Ptr->location.mapGroup
                        | ((u16)gSaveBlock1Ptr->location.mapNum << 8);
    gNetMailbox.peerObjectId = sPeerObjectId;
}

void Coop_Update(void)
{
    // A battle owns the link while it runs, so the session layer stands down
    // rather than competing for it. Checked before everything, including the
    // dormancy check below -- that one calls Coop_Reset, which would tear down
    // the very link the battle is in the middle of negotiating.
    if (sSuspendedForBattle)
    {
        // Say so before standing down. Everything below stops publishing while
        // suspended, which left the handover invisible: the diagnostics froze
        // at whatever they said the frame before the battle, so the wrapper and
        // the tests both went on reporting a healthy session right through a
        // battle that had taken the link away.
        gNetMailbox.linkFlags = COOP_DIAG_BATTLE;
        gNetMailbox.coopState = COOP_STATE_OFF;
        return;
    }

    // Nothing to do unless the wrapper is present. A plain emulator leaves the
    // mailbox untouched and we stay dormant, which is what makes the same ROM
    // still playable single-player.
    if (!gNetLinkActive)
    {
        if (sCoopState != COOP_STATE_OFF)
            Coop_Reset();
        return;
    }

    if (sStateTimer < 0xFFFF)
        sStateTimer++;

    switch (sCoopState)
    {
    case COOP_STATE_OFF:
        if (!IsCoopSessionPaired())
            break;

        // Both sides must agree on gLinkType before the exchange, or
        // GetLinkPlayerDataExchangeStatusTimed reports EXCHANGE_DIFF_SELECTIONS
        // and tears the link down. Both ROMs reach here by the same path, so
        // they always agree.
        gLinkType = LINKTYPE_COOP;
        OpenLink();
        EnterState(COOP_STATE_OPENING);
        break;

    case COOP_STATE_OPENING:
        if (!IsCoopSessionPaired())
        {
            EnterState(COOP_STATE_LOST);
            break;
        }
        // OpenLink spawns Task_TriggerHandshake, which pokes the state machine
        // five frames later. Once the transport reports a connection we move on
        // and wait for the player blocks to arrive.
        if (gLinkStatus & LINK_STAT_CONN_ESTABLISHED)
            EnterState(COOP_STATE_EXCHANGING);
        break;

    case COOP_STATE_EXCHANGING:
        if (!IsCoopSessionPaired())
        {
            EnterState(COOP_STATE_LOST);
            break;
        }
        // gReceivedRemoteLinkPlayers is the game's own "this link is usable"
        // flag. Nothing may be sent before it is set.
        if (gReceivedRemoteLinkPlayers == 1)
        {
            gCoopPeer.valid = FALSE;
            EnterState(COOP_STATE_ACTIVE);
            break;
        }
        if (sStateTimer > EXCHANGE_TIMEOUT_FRAMES)
        {
            // Give up rather than sit here forever; the next pass will retry
            // from OFF if both players are still present.
            CloseLink();
            EnterState(COOP_STATE_OFF);
        }
        break;

    case COOP_STATE_ACTIVE:
        if (!IsCoopSessionPaired() || !gReceivedRemoteLinkPlayers)
        {
            EnterState(COOP_STATE_LOST);
            break;
        }
        // Re-arm rather than set once. InitBlockSend overwrites gLinkCallback
        // outright, so any trade or battle setup silently evicts us and never
        // puts us back; checking for NULL each frame heals that automatically.
        if (gLinkCallback == NULL)
            gLinkCallback = CoopSendPositionCB;

        // The join handshake.
        //
        // Player 1 pushes both payloads back to back without waiting for an
        // answer; Player 2 replies whenever it is ready. An earlier version
        // chained them -- Player 1 sent the world only after Player 2 answered,
        // and Player 2 answered only after adopting its character, and adoption
        // waits for the player to be standing still and out of menus. So a
        // player who joined mid-cutscene, or just happened to be in a menu,
        // never received the badges and Pokedex at all. Two of three test runs
        // failed that way. Adoption and world sync have no reason to depend on
        // each other and no longer do.
        //
        // Simultaneous transfers in OPPOSITE directions are safe: each console
        // has its own receive buffer and only ever receives from the peer. What
        // is not safe is two transfers arriving at the SAME receiver, since
        // chunk 0 of either restarts the shared buffer -- which is why Player 1
        // waits for its first send to drain before starting the second.
        if (NetLink_IsMaster())
        {
            switch (sJoinStep)
            {
            case COOP_JOIN_SEND_PLAYER2:
                CoopSync_Send(COOP_STREAM_PLAYER2, GetCoopPlayer2(),
                              sizeof(struct CoopPlayer2));
                sJoinStep = COOP_JOIN_SEND_WORLD;
                break;

            case COOP_JOIN_SEND_WORLD:
                if (!CoopSync_IsSending())
                {
                    GatherWorldState(&sOutgoingWorld);
                    CoopSync_Send(COOP_STREAM_WORLD, &sOutgoingWorld,
                                  sizeof(sOutgoingWorld));
                    sJoinStep = COOP_JOIN_AWAIT_PLAYER2;
                }
                break;

            case COOP_JOIN_AWAIT_PLAYER2:
                if (CoopSync_HasReceived(COOP_STREAM_PLAYER2))
                {
                    u16 size;
                    const void *rec = CoopSync_GetReceived(COOP_STREAM_PLAYER2, &size);

                    if (rec != NULL && size == sizeof(struct CoopPlayer2))
                        *GetCoopPlayer2() = *(const struct CoopPlayer2 *)rec;

                    CoopSync_Reset();
                    sJoinStep = COOP_JOIN_DONE;
                }
                break;
            }
        }
        else
        {
            // Player 2 takes whatever arrives, in whichever order, and answers
            // once it has settled who it is.
            if (CoopSync_HasReceived(COOP_STREAM_PLAYER2))
            {
                u16 size;
                const void *rec = CoopSync_GetReceived(COOP_STREAM_PLAYER2, &size);

                if (rec != NULL && size == sizeof(struct CoopPlayer2))
                {
                    const struct CoopPlayer2 *stored = rec;

                    if (stored->claimed)
                    {
                        // Held, not applied: adoption waits for a frame where
                        // the player is actually in control.
                        sPendingRecord = *stored;
                        sHasPendingRecord = TRUE;
                    }
                    sGotPlayer2 = TRUE;
                }
                CoopSync_Reset();
            }
            else if (CoopSync_HasReceived(COOP_STREAM_WORLD))
            {
                u16 size;
                const void *w = CoopSync_GetReceived(COOP_STREAM_WORLD, &size);

                if (w != NULL && size == sizeof(struct CoopWorldState))
                    ApplyWorldState(w);

                CoopSync_Reset();
                sGotWorld = TRUE;
            }

            if (sHasPendingRecord)
                TryAdoptPendingRecord();

            // Answer once there is nothing left to take over, and never before:
            // a pre-adoption identity sent to Player 1 overwrites the very
            // character it was about to receive.
            if (sGotPlayer2 && sGotWorld && !sHasPendingRecord
                && sJoinStep != COOP_JOIN_DONE && !CoopSync_IsSending())
            {
                GatherLocalPlayerRecord(&sOutgoingRecord);
                CoopSync_Send(COOP_STREAM_PLAYER2, &sOutgoingRecord,
                              sizeof(sOutgoingRecord));
                sJoinStep = COOP_JOIN_DONE;
            }
        }
        break;

    case COOP_STATE_LOST:
        // Hold here until the relay reports both players again, then rebuild
        // the link from scratch. Reconnecting into a half-open link is how
        // stale commands get replayed as live input.
        if (IsCoopSessionPaired())
        {
            CloseLink();
            Coop_Reset();
        }
        break;
    }

    Coop_UpdateGate();
    UpdateGuestScene();
    Coop_UpdatePendingScene();
    PublishDiagnostics();
}

// ---------------------------------------------------------------------------
// Position broadcast
//
// One command per frame, 7 u16 words of payload. We send where we are rather
// than which buttons we pressed, so each console runs its own game with every
// movement system intact. Coordinates travel in camera space (map coords plus
// MAP_OFFSET), which is what both gObjectEvents and the spawn helper use, so
// nothing has to be converted on either side.
// ---------------------------------------------------------------------------

// Local id for the partner's object event. Map-authored NPCs use small ids, so
// this sits well clear of them.

// Object-event slot the partner currently occupies, or OBJECT_EVENTS_COUNT.
static EWRAM_DATA u16 sFrameCounter = 0;

/**
 * Map the local player's avatar flags to the state id the graphics tables use.
 *
 * The flags are a bitfield that can hold several bits at once (CONTROLLABLE
 * and DASH ride alongside the real state), so order matters: check the most
 * specific first. Underwater before surfing, because underwater sets both.
 */
static u8 GetPlayerAvatarStateForCoop(void)
{
    u8 flags = gPlayerAvatar.flags;

    if (flags & PLAYER_AVATAR_FLAG_UNDERWATER)
        return PLAYER_AVATAR_STATE_UNDERWATER;
    if (flags & PLAYER_AVATAR_FLAG_SURFING)
        return PLAYER_AVATAR_STATE_SURFING;
    if (flags & PLAYER_AVATAR_FLAG_MACH_BIKE)
        return PLAYER_AVATAR_STATE_MACH_BIKE;
    if (flags & PLAYER_AVATAR_FLAG_ACRO_BIKE)
        return PLAYER_AVATAR_STATE_ACRO_BIKE;

    return PLAYER_AVATAR_STATE_NORMAL;
}

static void CoopSendPositionCB(void)
{
    struct ObjectEvent *me;

    if (gReceivedRemoteLinkPlayers != TRUE)
        return;

    // A bulk transfer owns the command slot until it finishes. One command per
    // frame is the whole budget, so position updates pause for the second or so
    // a transfer takes. That is deliberate: the alternative is interleaving,
    // which doubles the transfer time to keep a partner's walk smooth during a
    // wait they are already sitting through.
    if (CoopSync_SendChunk(gSendCmd))
        return;

    // Scenes, then gates. A scene has to arrive before the partner can reach
    // the gate it leads to, so sending them the other way round would spend the
    // gate's whole timeout waiting for a scene stuck behind it. Everything below
    // can wait a frame; a partner sat on a dark screen cannot.
    if (CoopSendScene(gSendCmd))
        return;

    if (CoopSendGate(gSendCmd))
        return;

    // Changes before positions. A missed position costs one frame of smoothness
    // and the next one corrects it; a missed flag is a gym badge that never
    // arrives.
    if (CoopSendDelta(gSendCmd))
        return;

    me = &gObjectEvents[gPlayerAvatar.objectEventId];

    gSendCmd[0] = LINKCMD_COOP_POS;
    gSendCmd[1] = gSaveBlock1Ptr->location.mapGroup
                | ((u16)gSaveBlock1Ptr->location.mapNum << 8);
    gSendCmd[2] = me->currentCoords.x;
    gSendCmd[3] = me->currentCoords.y;
    gSendCmd[4] = (me->facingDirection & 0xF)
                | ((me->currentElevation & 0xF) << 4)
                | ((GetPlayerAvatarStateForCoop() & 0xF) << 8)
                | ((gSaveBlock2Ptr->playerGender & 0x1) << 12);
    // Whether we are mid-step. The receiver uses it to decide between a walk
    // animation and standing still, which is what stops a partner who is
    // simply standing there from twitching.
    gSendCmd[5] = (me->heldMovementActive && !me->heldMovementFinished) ? 1 : 0;

    gNetMailbox.posSent++;
}

void Coop_ReceivePosition(u8 playerId, const u16 *cmd)
{
    // Our own broadcast is looped back to us by the transport, exactly as the
    // cable did. Ignore it; we are not our own partner.
    if (playerId == GetMultiplayerId())
        return;

    gCoopPeer.mapGroup = cmd[1] & 0xFF;
    gCoopPeer.mapNum = (cmd[1] >> 8) & 0xFF;
    gCoopPeer.x = cmd[2];
    gCoopPeer.y = cmd[3];
    gCoopPeer.facing = cmd[4] & 0xF;
    gCoopPeer.elevation = (cmd[4] >> 4) & 0xF;
    gCoopPeer.avatarState = (cmd[4] >> 8) & 0xF;
    gCoopPeer.gender = (cmd[4] >> 12) & 0x1;
    gCoopPeer.moving = cmd[5] & 1;
    gCoopPeer.lastSeenFrame = sFrameCounter;
    gCoopPeer.valid = TRUE;

    gNetMailbox.posRecv++;
}

static bool8 PeerIsOnOurMap(void)
{
    return gCoopPeer.valid
        && gCoopPeer.mapGroup == gSaveBlock1Ptr->location.mapGroup
        && gCoopPeer.mapNum == gSaveBlock1Ptr->location.mapNum;
}

static bool8 PeerHasGoneQuiet(void)
{
    return (u16)(sFrameCounter - gCoopPeer.lastSeenFrame) > PEER_SILENCE_FRAMES;
}

static struct ObjectEvent *GetPeerObject(void)
{
    struct ObjectEvent *obj;

    if (sPeerObjectId >= OBJECT_EVENTS_COUNT)
        return NULL;

    obj = &gObjectEvents[sPeerObjectId];

    // A map change resets every object event, so the slot we remember may now
    // be inactive or reused by an NPC. Verify rather than trust it.
    if (!obj->active || obj->localId != COOP_PEER_LOCAL_ID)
    {
        sPeerObjectId = OBJECT_EVENTS_COUNT;
        return NULL;
    }

    return obj;
}

static void DespawnPeer(void)
{
    if (GetPeerObject() != NULL)
    {
        RemoveObjectEventByLocalIdAndMap(COOP_PEER_LOCAL_ID,
                                         gSaveBlock1Ptr->location.mapNum,
                                         gSaveBlock1Ptr->location.mapGroup);
    }
    sPeerObjectId = OBJECT_EVENTS_COUNT;
}

static void SpawnPeer(void)
{
    u16 gfxId = GetRivalAvatarGraphicsIdByStateIdAndGender(gCoopPeer.avatarState,
                                                           gCoopPeer.gender);
    u8 id = SpawnSpecialObjectEventParameterized(gfxId, MOVEMENT_TYPE_NONE,
                                                 COOP_PEER_LOCAL_ID,
                                                 gCoopPeer.x, gCoopPeer.y,
                                                 gCoopPeer.elevation);

    // Returns OBJECT_EVENTS_COUNT when all 16 slots are taken. A busy route can
    // genuinely run out; try again next frame rather than treating it as fatal.
    if (id >= OBJECT_EVENTS_COUNT)
    {
        sPeerObjectId = OBJECT_EVENTS_COUNT;
        return;
    }

    sPeerObjectId = id;
    ObjectEventTurn(&gObjectEvents[id], gCoopPeer.facing);
}

void Coop_UpdatePeerSprite(void)
{
    struct ObjectEvent *peer;
    s16 dx, dy;

    sFrameCounter++;

    if (!IsCoopLinkActive() || !PeerIsOnOurMap() || PeerHasGoneQuiet())
    {
        DespawnPeer();
        return;
    }

    peer = GetPeerObject();
    if (peer == NULL)
    {
        SpawnPeer();
        return;
    }

    // Swap the sprite when they get on a bike, surf, and so on. Cheap to call
    // every frame -- it returns immediately when the id is unchanged.
    ObjectEventSetGraphicsId(peer,
        GetRivalAvatarGraphicsIdByStateIdAndGender(gCoopPeer.avatarState,
                                                   gCoopPeer.gender));

    // Let any walk already in progress finish before starting another, or the
    // sprite stutters in place instead of sliding between tiles.
    if (ObjectEventClearHeldMovementIfFinished(peer) == 0
        && peer->heldMovementActive)
        return;

    dx = (s16)gCoopPeer.x - peer->currentCoords.x;
    dy = (s16)gCoopPeer.y - peer->currentCoords.y;

    if (dx == 0 && dy == 0)
    {
        if (peer->facingDirection != gCoopPeer.facing)
            ObjectEventTurn(peer, gCoopPeer.facing);
        return;
    }

    // Exactly one tile in a cardinal direction: animate the step, so the
    // partner slides naturally and triggers grass rustle, reflections and the
    // rest of the ground effects for free.
    if ((dx == 0 && (dy == 1 || dy == -1)) || (dy == 0 && (dx == 1 || dx == -1)))
    {
        u8 dir = dx == 1 ? DIR_EAST : dx == -1 ? DIR_WEST
               : dy == 1 ? DIR_SOUTH : DIR_NORTH;

        ObjectEventSetHeldMovement(peer, GetWalkNormalMovementAction(dir));
        return;
    }

    // Anything else is a warp, a ledge hop, or us having missed frames. Snap
    // rather than trying to animate a path we never saw them take.
    MoveObjectEventToMapCoords(peer, gCoopPeer.x, gCoopPeer.y);
    ObjectEventTurn(peer, gCoopPeer.facing);
}
