#include "global.h"
#include "coop.h"
#include "fieldmap.h"
#include "palette.h"
#include "money.h"
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
#include "pokemon_storage_system.h"
#include "item.h"
#include "field_message_box.h"
#include "field_control_avatar.h"
#include "event_scripts.h"
#include "intro.h"
#include "naming_screen.h"
#include "script_pokemon_util.h"
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

// Player 2's party, kept alive in Player 1's save.
//
// The join hands the record over once and that used to be the end of it: every
// level gained, every Pokemon caught and every move learned on Player 2's side
// existed only in Player 2's RAM, and was gone the moment the session ended.
// Player 1 saves, Player 2 comes back tomorrow with the team they first
// joined with.
//
// Re-sending on a timer alone is too expensive -- the record is 632 bytes
// against a transport that carries a few hundred bytes a second, and a battle
// needs that room. So it goes only when the party has actually changed, found
// by checksumming the part that matters. Position and play time are
// deliberately NOT in the checksum: they change constantly, and position is
// already arriving every frame for the sprite (see Coop_ReceivePosition).
#define RECORD_CHECK_INTERVAL 120
static EWRAM_DATA u16 sRecordCheckTimer = 0;
static EWRAM_DATA u32 sLastSentPartyHash = 0;

static u32 HashLocalParty(void)
{
    const u8 *bytes = (const u8 *)gPlayerParty;
    u32 n = sizeof(struct Pokemon) * PARTY_SIZE;
    u32 h = 2166136261u;  // FNV-1a
    u32 i;

    h = (h ^ gPlayerPartyCount) * 16777619u;

    for (i = 0; i < n; i++)
        h = (h ^ bytes[i]) * 16777619u;

    return h;
}
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
    case COOP_DELTA_MONEY:
        // The whole amount, not the change. A delta that went missing would
        // leave the two wallets disagreeing for ever; an absolute figure is
        // corrected by the very next purchase.
        SetMoney(&gSaveBlock1Ptr->money, ((u32)id << 16) | value);
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
//
// Was ten seconds, which is not generous at all once you watch somebody play:
// an NPC with three boxes of dialogue takes longer than that to read, and the
// scene was then dropped on both consoles. Nothing breaks when it is -- the
// trigger is left unfired and runs again next time they step on it -- but
// "walk onto the trigger and nothing happens" is not a thing a player can make
// sense of. Half a minute covers a conversation; the player who got there
// first is looking at "Waiting for your partner", not a blank screen, so the
// wait explains itself.
#define SCENE_GATE_TIMEOUT_FRAMES 1800 // 30 seconds

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

// ---------------------------------------------------------------------------
// Travelling together
//
// Fly, Teleport, Dig and an Escape Rope move one player across the map in a
// way walking never does. Left alone, one player ends up in Lilycove and the
// other in Petalburg, and from then on every gym door and every story scene
// refuses, because they all need both players present. Getting back together
// means a long walk that nobody enjoys twice.
//
// So the traveller takes their partner along. What crosses the link is not a
// destination -- the four moves work out where they are going at four
// different points, and some of them not until the screen has already faded
// -- but a far simpler thing: "I am travelling, come with me." The partner
// remembers that, waits until it can see the traveller standing on a map that
// is not its own, and warps to them.
//
// Deciding on arrival rather than departure is what makes one mechanism cover
// all four moves, and it is also what makes it safe: the partner moves when
// the traveller has finished moving, not into a map that is still loading.
// It also means a cancelled trip costs nothing -- back out of the fly map and
// the request simply expires, because the traveller never turns up anywhere
// their partner is not.
// How deep the transport's send backlog may get before position broadcasts
// stop being queued. Eight is a seventh of the backlog: deep enough that an
// ordinary hiccup does not interrupt the partner's walk, shallow enough that
// there is always room for the things that cannot be re-sent.
#define NET_POS_SKIP_DEPTH  8

#define FOLLOW_SENDS        6     // repeats, so one dropped frame is not the end of it
#define FOLLOW_WAIT_FRAMES  900   // give up if they never arrive anywhere

static EWRAM_DATA u8 sFollowSendsLeft = 0;
static EWRAM_DATA u8 sFollowSeq = 0;
static EWRAM_DATA u8 sUsedPeerFollowSeq = 0;
static EWRAM_DATA bool8 sFollowPending = FALSE;
static EWRAM_DATA u16 sFollowWaited = 0;

// Whether this pending follow is the one that places Player 2 on their very
// first session, as opposed to a partner flying somewhere.
//
// The deadline above is right for a flight: the trip is over, and if the
// partner spent fifteen seconds in a menu rather than following, the moment
// has passed and dragging them across the region later would be worse than
// not. It is wrong for the first placement, where Player 2 has nowhere else
// to be -- their town is a guess made before the link said anything. Letting
// that expire left the two in different regions for the whole session, and
// all it took was Player 1 lingering in a menu.
static EWRAM_DATA bool8 sFollowIsFirstPlacement = FALSE;

void Coop_FollowMe(void)
{
    if (!IsCoopLinkActive())
        return;

    // A sequence number, so the repeats are recognised as one request rather
    // than four separate trips.
    sFollowSeq++;
    if (sFollowSeq == 0)
        sFollowSeq = 1;
    sFollowSendsLeft = FOLLOW_SENDS;
}

static bool8 CoopSendFollow(u16 *sendCmd)
{
    if (sFollowSendsLeft == 0)
        return FALSE;

    sendCmd[0] = LINKCMD_COOP_FOLLOW;
    sendCmd[1] = sFollowSeq;
    sFollowSendsLeft--;

    return TRUE;
}

void Coop_ReceiveFollow(u8 playerId, const u16 *cmd)
{
    u8 seq = cmd[1] & 0xFF;

    if (playerId == GetMultiplayerId())
        return;

    // Same trip, repeated. Taking it twice would re-arm the wait after we had
    // already arrived and send us chasing them again.
    if (seq == sUsedPeerFollowSeq)
        return;

    sUsedPeerFollowSeq = seq;
    sFollowPending = TRUE;
    sFollowWaited = 0;
    sFollowIsFirstPlacement = FALSE;
}

// Go to them, once they are somewhere to go to.
static void Coop_UpdateFollow(void)
{
    if (!sFollowPending)
        return;

    if (!sFollowIsFirstPlacement && ++sFollowWaited > FOLLOW_WAIT_FRAMES)
    {
        sFollowPending = FALSE;
        return;
    }

    // Not until this console is its own master. Warping out from under a
    // script, a battle or a menu is how a save gets corrupted, and the
    // traveller is not going anywhere.
    if (ArePlayerFieldControlsLocked() || ScriptContext_IsEnabled()
        || Coop_IsSuspendedForBattle() || gPaletteFade.active)
        return;

    if (!gCoopPeer.valid)
        return;

    // Still on the way. Their position keeps arriving throughout the trip, so
    // "same map as us" means they have not left yet rather than that they have
    // arrived next to us.
    if (gCoopPeer.mapGroup == gSaveBlock1Ptr->location.mapGroup
        && gCoopPeer.mapNum == gSaveBlock1Ptr->location.mapNum)
    {
        // Unless this was the first-session placement, in which case being on
        // their map IS the goal and it is already met.
        //
        // That one deliberately has no deadline -- Player 2 has nowhere else
        // to be -- so without this it stayed armed for the whole session,
        // waiting for the two to be apart. The next time the partner walked
        // through a door, hours later, it fired: Player 2 was yanked through
        // after them by a request made when the game started.
        if (sFollowIsFirstPlacement)
        {
            sFollowPending = FALSE;
            sFollowIsFirstPlacement = FALSE;
        }
        return;
    }

    sFollowPending = FALSE;
    sFollowIsFirstPlacement = FALSE;

    // Out of camera space, the same conversion the saved record needs.
    SetWarpDestination(gCoopPeer.mapGroup, gCoopPeer.mapNum, WARP_ID_NONE,
                       gCoopPeer.x - MAP_OFFSET, gCoopPeer.y - MAP_OFFSET);
    DoWarp();
    ResetInitialPlayerAvatarState();
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
    gCoopDbgGateSent++;

    return TRUE;
}

void Coop_ReceiveGate(u8 playerId, const u16 *cmd)
{
    gCoopDbgGateRecv++;

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

// Repeats, and a sequence number so the repeats are recognised as one scene.
//
// This went out exactly once, alone among the things this protocol sends --
// gates repeat four times, "come with me" six, a scene's own page turns four.
// One lost frame and the partner simply never played the scene: the trigger
// had already fired on this console, so it was not coming again, and the two
// players walked away from the same spot having seen different things. It
// showed up as about one run in five of the mirrored-scene check, which is
// also roughly how often it would have happened to somebody playing.
#define SCENE_SENDS 4
static EWRAM_DATA u8 sSceneSendsLeft = 0;
static EWRAM_DATA u16 sSceneSendSeq = 0;
static EWRAM_DATA u16 sUsedPeerSceneSeq = 0;

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

// Pacing a mirrored scene.
//
// The console that triggered the scene counts its own presses; the other one
// counts how many of them it has used. A count rather than a bare "advance"
// because the transport is allowed to lose a frame: a missed pulse would hang
// the follower on a message box for ever, while a missed count catches up by
// itself on the next one.
//
// It is the driver's count that travels, not a delta, for the same reason.
static EWRAM_DATA u16 sSceneAdvanceSent = 0;    // driver: presses so far
static EWRAM_DATA u16 sSceneAdvanceHeard = 0;   // follower: the driver's count
static EWRAM_DATA u16 sSceneAdvanceUsed = 0;    // follower: how many consumed
static EWRAM_DATA u8 sSceneAdvanceSendsLeft = 0;
static EWRAM_DATA u16 sSceneAdvanceWaited = 0;

// Repeats, so one dropped frame does not strand the follower mid-sentence.
// Cheap: a scene is a few dozen presses, not a per-frame broadcast.
#define SCENE_ADVANCE_SENDS 4

// How long the follower waits on a silent driver before taking its own
// buttons back. Ten seconds.
#define SCENE_ADVANCE_WAIT_FRAMES 600

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
    sSceneSendsLeft = SCENE_SENDS;
    sSceneSendSeq++;
    if (sSceneSendSeq == 0)
        sSceneSendSeq = 1;
    sSceneSendLocalId = gSpecialVar_LastTalked;
    sInHostScene = TRUE;
    return TRUE;
}

// Emit a queued scene. Returns TRUE if it wrote a command.
static bool8 CoopSendScene(u16 *sendCmd)
{
    u32 off;

    if (sSceneSendPtr == NULL || sSceneSendsLeft == 0)
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
    sendCmd[6] = sSceneSendSeq;

    if (--sSceneSendsLeft == 0)
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

    // One of the repeats of a scene we have already taken. Accepting it again
    // would replay a scene that may well be running right now.
    if (cmd[6] == sUsedPeerSceneSeq)
        return;

    off = cmd[2] | ((u32)cmd[3] << 16);

    // A script pointer arriving over a wire gets checked before it is jumped
    // to. Everything below trusts it completely -- it goes straight into the
    // script interpreter -- so a corrupt value here is not a wrong scene, it is
    // arbitrary bytes run as bytecode.
    if (off >= 0x02000000)
        return;

    sUsedPeerSceneSeq = cmd[6];
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

    // Start level with the driver rather than from zero. The driver's count
    // runs for the whole session, not per scene, so "how many presses have
    // happened since this scene began" is the difference from whatever it
    // stood at the moment this console joined the scene. Zeroing instead
    // would make every press the driver had ever made look like a pending
    // advance, and the follower would skip the scene outright.
    sSceneAdvanceUsed = sSceneAdvanceHeard;
    sSceneAdvanceWaited = 0;

    ScriptContext_SetupScript(sPendingScene);
    sPendingScene = NULL;
    sInGuestScene = TRUE;
}

bool8 Coop_IsSceneGuest(void)
{
    return sInGuestScene;
}

// --- pacing -----------------------------------------------------------------

bool8 Coop_SceneFollowerWaits(void)
{
    return sInGuestScene && IsCoopLinkActive();
}

// The follower's text prints instantly, because it has nothing to pace. See
// IsPlayerTextSpeedInstant.
bool8 Coop_SceneTextIsInstant(void)
{
    return Coop_SceneFollowerWaits();
}

// The driver pressed A. Count it and start telling the partner.
void Coop_SceneAdvanced(void)
{
    if (!sInHostScene || !IsCoopLinkActive())
        return;

    sSceneAdvanceSent++;
    sSceneAdvanceSendsLeft = SCENE_ADVANCE_SENDS;
}

// The follower: has the driver moved on yet?
bool8 Coop_SceneTakeAdvance(void)
{
    // u16 subtraction, so this still reads correctly across the wrap.
    if ((u16)(sSceneAdvanceHeard - sSceneAdvanceUsed) != 0)
    {
        sSceneAdvanceUsed++;
        sSceneAdvanceWaited = 0;
        return TRUE;
    }

    // Nothing has come for a long time. Hand the buttons back.
    //
    // The repeats above make a lost advance unlikely rather than impossible,
    // and the cost of one is a console frozen on a message box with no way out
    // -- the script cannot end, so nothing clears the state that is waiting on
    // it. Ten seconds of silence is already a broken link or a partner who has
    // put their phone down; letting this player read on by themselves is a
    // worse scene than intended and a far better outcome than a locked game.
    if (sSceneAdvanceWaited < SCENE_ADVANCE_WAIT_FRAMES)
    {
        sSceneAdvanceWaited++;
        return FALSE;
    }

    if (JOY_NEW(A_BUTTON) || JOY_NEW(B_BUTTON))
    {
        sSceneAdvanceWaited = 0;
        return TRUE;
    }

    return FALSE;
}

static bool8 CoopSendSceneAdvance(u16 *sendCmd)
{
    if (sSceneAdvanceSendsLeft == 0)
        return FALSE;

    sendCmd[0] = LINKCMD_COOP_ADVANCE;
    sendCmd[1] = sSceneAdvanceSent;
    sSceneAdvanceSendsLeft--;

    return TRUE;
}

void Coop_ReceiveSceneAdvance(u8 playerId, const u16 *cmd)
{
    if (playerId == GetMultiplayerId())
        return;

    sSceneAdvanceHeard = cmd[1];
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
    sSceneSendsLeft = 0;
    // Not the sequence numbers: they are what tells a repeat from a new
    // scene, and zeroing them across a reconnect would make the next scene
    // look like one already taken.
    sPendingScene = NULL;
    sPendingSceneFrames = 0;
    sPendingSceneLocalId = 0;
    sSceneSendLocalId = 0;
    sInGuestScene = FALSE;
    sInHostScene = FALSE;
    // Not the driver's own count: it is this console's, it is monotonic, and
    // the partner baselines against whatever it last heard. Resetting it would
    // make the next scene's first press look like a step backwards.
    sSceneAdvanceSendsLeft = 0;
    sSceneAdvanceWaited = 0;
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


// ---------------------------------------------------------------------------
// Pokemon storage.
//
// Every write to a box slot is broadcast as it happens. See include/coop.h for
// why the boxes have to be shared at all, and why a joining player is sent only
// the slots that have something in them.
// ---------------------------------------------------------------------------

// What one box change looks like on the wire. Four bytes of where, then the
// Pokemon itself -- 84 in all, which the chunked transfer moves in about a
// seventh of a second.
struct CoopBoxOp
{
    u8 boxId;
    u8 position;
    bool8 clear;        // TRUE: empty the slot, and ignore the mon below
    u8 padding;
    struct BoxPokemon mon;
};

// A short queue, because the transfer layer carries one thing at a time and a
// player releasing several Pokemon in a row would otherwise lose all but the
// first. Four is enough for any burst a person can produce by hand; a fuller
// queue drops the oldest rather than the newest, since the newest is the one
// still on screen.
#define COOP_BOX_QUEUE_SLOTS 4

static EWRAM_DATA struct CoopBoxOp sBoxQueue[COOP_BOX_QUEUE_SLOTS] = {0};
static EWRAM_DATA u8 sBoxQueueHead = 0;
static EWRAM_DATA u8 sBoxQueueTail = 0;
static EWRAM_DATA struct CoopBoxOp sBoxOutgoing = {0};
static EWRAM_DATA u16 sBoxJoinCursor = 0;
static EWRAM_DATA bool8 sBoxJoinRunning = FALSE;
static EWRAM_DATA bool8 sApplyingBoxOp = FALSE;

#define BOX_SLOT_TOTAL (TOTAL_BOXES_COUNT * IN_BOX_COUNT)

static void EnqueueBoxOp(const struct CoopBoxOp *op)
{
    u8 next = (sBoxQueueHead + 1) % COOP_BOX_QUEUE_SLOTS;

    if (next == sBoxQueueTail)
        sBoxQueueTail = (sBoxQueueTail + 1) % COOP_BOX_QUEUE_SLOTS;

    sBoxQueue[sBoxQueueHead] = *op;
    sBoxQueueHead = next;
}

void Coop_QueueBoxWrite(u8 boxId, u8 position, const struct BoxPokemon *mon)
{
    struct CoopBoxOp op;

    // Not while applying the partner's own change, or the two consoles would
    // bounce the same deposit back and forth for ever.
    if (!IsCoopLinkActive() || sApplyingBoxOp)
        return;

    op.boxId = boxId;
    op.position = position;
    op.clear = FALSE;
    op.padding = 0;
    op.mon = *mon;
    EnqueueBoxOp(&op);
}

void Coop_QueueBoxClear(u8 boxId, u8 position)
{
    struct CoopBoxOp op;

    if (!IsCoopLinkActive() || sApplyingBoxOp)
        return;

    op.boxId = boxId;
    op.position = position;
    op.clear = TRUE;
    op.padding = 0;
    EnqueueBoxOp(&op);
}

void Coop_ApplyBoxOp(const void *data)
{
    const struct CoopBoxOp *op = data;

    if (op->boxId >= TOTAL_BOXES_COUNT || op->position >= IN_BOX_COUNT)
        return;

    sApplyingBoxOp = TRUE;

    if (op->clear)
        ZeroBoxMonAt(op->boxId, op->position);
    else
        SetBoxMonAt(op->boxId, op->position, (struct BoxPokemon *)&op->mon);

    sApplyingBoxOp = FALSE;
}

void Coop_BeginBoxJoinSync(void)
{
    sBoxJoinCursor = 0;
    sBoxJoinRunning = TRUE;
}

void Coop_UpdateBoxSync(void)
{
    if (!IsCoopLinkActive())
        return;

    // Arrivals first, and handled HERE rather than in the join handshake.
    //
    // Box changes travel in both directions for the whole session, while the
    // join handshake runs on one console and only while joining -- so a branch
    // added there was never reached by a deposit made after the game had
    // settled, which is every deposit.
    //
    // ClearReceived rather than Reset: this console is very likely sending a
    // box change of its own, and a full reset would abandon it.
    if (CoopSync_HasReceived(COOP_STREAM_BOXMON))
    {
        u16 size;
        const void *b = CoopSync_GetReceived(COOP_STREAM_BOXMON, &size);

        if (b != NULL && size == sizeof(struct CoopBoxOp))
            Coop_ApplyBoxOp(b);

        CoopSync_ClearReceived();
    }

    // Walk the boxes, enqueuing what is actually in them. Paced by queue space
    // rather than sent as one block: the storage is 34 KB and this transport
    // moves about 800 bytes a second, so a full copy would hold a joining
    // player for three quarters of a minute. Occupied slots alone cost a second
    // or two for a normal playthrough, and nothing at all for a new game.
    while (sBoxJoinRunning
        && ((sBoxQueueHead + 1) % COOP_BOX_QUEUE_SLOTS) != sBoxQueueTail)
    {
        u8 boxId, position;
        struct BoxPokemon *mon;

        if (sBoxJoinCursor >= BOX_SLOT_TOTAL)
        {
            sBoxJoinRunning = FALSE;
            break;
        }

        boxId = sBoxJoinCursor / IN_BOX_COUNT;
        position = sBoxJoinCursor % IN_BOX_COUNT;
        sBoxJoinCursor++;

        mon = GetBoxedMonPtr(boxId, position);
        if (mon != NULL && GetBoxMonData(mon, MON_DATA_SPECIES, NULL) != SPECIES_NONE)
        {
            struct CoopBoxOp op;
            op.boxId = boxId;
            op.position = position;
            op.clear = FALSE;
            op.padding = 0;
            op.mon = *mon;
            EnqueueBoxOp(&op);
        }
    }

    // One at a time: the transfer layer carries a single stream, and the join
    // sync shares it with ordinary deposits.
    if (sBoxQueueHead != sBoxQueueTail && !CoopSync_IsSending())
    {
        sBoxOutgoing = sBoxQueue[sBoxQueueTail];
        sBoxQueueTail = (sBoxQueueTail + 1) % COOP_BOX_QUEUE_SLOTS;
        CoopSync_Send(COOP_STREAM_BOXMON, &sBoxOutgoing, sizeof(sBoxOutgoing));
    }
}

static void ResetBoxSync(void)
{
    sBoxQueueHead = 0;
    sBoxQueueTail = 0;
    sBoxJoinCursor = 0;
    sBoxJoinRunning = FALSE;
    sApplyingBoxOp = FALSE;
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

    out->money = GetMoney(&gSaveBlock1Ptr->money);
    out->playerRegion = gSaveBlock2Ptr->playerRegion;
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

    SetMoney(&gSaveBlock1Ptr->money, in->money);

    // Adopt Player 1's region, and with it the presentation that hangs off it.
    //
    // isFrlg and isFrlgInt are not in the save block -- SetInitialGame derives
    // them from playerRegion, and until something calls it they keep whatever
    // the title screen left behind. The skin is read fresh on the next map
    // load, which the warp onto Player 1's map provides a moment after this.
    if (in->playerRegion == REGION_KANTO || in->playerRegion == REGION_HOENN)
    {
        gSaveBlock2Ptr->playerRegion = in->playerRegion;
        SetInitialGame();
    }
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

// Whether this console is the joining player in a co-op session.
//
// Known before the game starts, because the wrapper writes the slot into the
// mailbox at boot -- which is what lets Player 2's console skip the main menu
// and the whole Birch intro rather than making somebody sit through it, and a
// save file exist, for a character that is going to be replaced over the link
// anyway.
// How far Player 2's intro-free entry got. 1 = the menu task released it,
// 2 = the new-game callback started, 3 = it finished and the overworld has it.
// A value that stops at 2 is a hang inside the callback, which from outside
// looks identical to the menu never releasing -- the last frame just stays on
// screen either way.
EWRAM_DATA u8 gCoopDbgJoinEntry = 0;

// (menu type << 4) | action, as the main menu computed it. Which branch the
// dispatch took is not visible from the screen: a menu that holds and a menu
// that dispatched to the wrong action look the same from outside.
EWRAM_DATA u8 gCoopDbgMenuAction = 0;

// Gate announcements written, and gate announcements taken off the wire. A
// gate that never opens is either one console not telling or the other not
// hearing, and from the gate state alone those look identical.
EWRAM_DATA u16 gCoopDbgGateSent = 0;
EWRAM_DATA u16 gCoopDbgGateRecv = 0;

// Where the running script is, mirrored out of IWRAM once a frame so the test
// rig can read it and resolve it to a name. A console stuck inside a script
// reports SCRIPT_BUSY and a black screen and nothing else, and that describes
// every script in the game equally well.
EWRAM_DATA u32 gCoopDbgScriptPtr = 0;

// CONTEXT_RUNNING / CONTEXT_WAITING / CONTEXT_SHUTDOWN, mirrored for the same
// reason. WAITING means a menu or a prompt has the script.
EWRAM_DATA u8 gCoopDbgScriptStatus = 0;

// Silence every outgoing sync for a moment.
//
// Wiping a save is thousands of flag, var, Pokedex, bag and storage writes,
// and every one of them now has a hook on it. Player 2 does that wipe with the
// link already up -- their console has no save of its own, so starting means
// initialising one from nothing while connected -- and the result is thousands
// of pointless queue operations announcing a world that is about to be thrown
// away and replaced by Player 1's.
//
// Pointless is the charitable reading; it also wedged the console outright.
void Coop_SuppressSync(bool8 suppress)
{
    sApplyingRemote = suppress;
    sApplyingBoxOp = suppress;
}

bool8 Coop_IsJoiningPlayer(void)
{
    return gNetLinkActive && !NetLink_IsMaster();
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
// The partner's follower Pokemon. Defined down with the peer sprite it hangs
// off, used up here by the session update and the send callback.
static bool8 CoopSendFollowerMon(u16 *sendCmd);
static void CheckOurFollower(void);
static void ResetPeerFollower(void);
static void Coop_UpdateFirstRun(void);

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
    ResetPeerFollower();
    ResetBoxSync();
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

    gCoopDbgScriptPtr = (u32)ScriptContext_GetScriptPtr();
    gCoopDbgScriptStatus = ScriptContext_GetStatus();

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

                    // Player 1 owns the save, so Player 1 owns the boxes: send
                    // them over once the rest of the join is settled. Started
                    // here rather than earlier because the transfer layer
                    // carries one stream at a time and the join's own transfers
                    // come first.
                    Coop_BeginBoxJoinSync();
                }
                break;

            case COOP_JOIN_DONE:
                // Player 2 re-sends its record whenever its party changes.
                // Taking those is what makes the progress stick: this copy is
                // the one that goes into the save.
                //
                // Position is left alone. It is maintained every frame from
                // the live broadcast, so the copy in an arriving record is
                // older than what is already here.
                if (CoopSync_HasReceived(COOP_STREAM_PLAYER2))
                {
                    u16 size;
                    const void *rec = CoopSync_GetReceived(COOP_STREAM_PLAYER2, &size);

                    if (rec != NULL && size == sizeof(struct CoopPlayer2))
                    {
                        struct CoopPlayer2 *dst = GetCoopPlayer2();
                        struct Coords16 pos = dst->pos;
                        struct WarpData location = dst->location;

                        *dst = *(const struct CoopPlayer2 *)rec;
                        dst->pos = pos;
                        dst->location = location;
                    }

                    CoopSync_ClearReceived();
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
                        // Player 1's save remembers who this player is, so
                        // there is nothing to ask them.
                        Coop_CancelFirstRun();
                    }
                    else
                    {
                        // Nobody has played as Player 2 in this save before,
                        // so there is no character and no saved spot to go
                        // back to -- but there is still a partner standing
                        // somewhere, and Player 2 should be standing next to
                        // them.
                        //
                        // The same wait the travel-together warp uses, for the
                        // same reason: it goes as soon as this console is its
                        // own master, and it reads the live position broadcast
                        // rather than anything stored, so it lands where
                        // Player 1 actually is. Without it a first session put
                        // Player 2 in Littleroot and left them there while
                        // Player 1 started the game in Pallet Town.
                        sFollowPending = TRUE;
                        sFollowWaited = 0;
                        sFollowIsFirstPlacement = TRUE;

                        // And nobody has played as Player 2 before, so this
                        // player still has to say who they are and pick a
                        // starter. Armed here rather than when their console
                        // started, because until this arrives there is no way
                        // to tell a first session from a returning one.
                        Coop_ArmFirstRun();
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
                sLastSentPartyHash = HashLocalParty();
                sJoinStep = COOP_JOIN_DONE;
            }
            else if (sJoinStep == COOP_JOIN_DONE && !CoopSync_IsSending()
                     && !sSuspendedForBattle)
            {
                // Checked on a timer rather than every frame: hashing six
                // Pokemon is cheap but not free, and nothing changes a party
                // faster than this notices.
                if (++sRecordCheckTimer >= RECORD_CHECK_INTERVAL)
                {
                    u32 hash = HashLocalParty();

                    sRecordCheckTimer = 0;
                    if (hash != sLastSentPartyHash)
                    {
                        GatherLocalPlayerRecord(&sOutgoingRecord);
                        CoopSync_Send(COOP_STREAM_PLAYER2, &sOutgoingRecord,
                                      sizeof(sOutgoingRecord));
                        sLastSentPartyHash = hash;
                    }
                }
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
    CheckOurFollower();
    Coop_UpdateFirstRun();
    Coop_UpdateFollow();
    Coop_UpdateBoxSync();
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

    // Ahead of the gate, and for the same reason the scene is: a partner
    // sitting on a message box waiting to be let through is a partner looking
    // at a screen that has stopped.
    if (CoopSendSceneAdvance(gSendCmd))
        return;

    if (CoopSendGate(gSendCmd))
        return;

    // "Come with me" is small, rare and time-critical -- the partner cannot
    // act on it until the traveller has arrived, so a late one strands them.
    if (CoopSendFollow(gSendCmd))
        return;

    if (CoopSendFollowerMon(gSendCmd))
        return;

    // Changes before positions. A missed position costs one frame of smoothness
    // and the next one corrects it; a missed flag is a gym badge that never
    // arrives.
    if (CoopSendDelta(gSendCmd))
        return;

    // Not while the queue is backed up.
    //
    // The two consoles do not run at the same speed -- two phones never will --
    // and the faster one queues more frames per second than the transport
    // carries away. Positions are what fills that gap, sixty a second of them,
    // and left alone they pack the backlog solid. Everything the session says
    // after that has to fight for a slot, and some of it loses.
    //
    // Skipping them here is free in a way that dropping them later is not:
    // the next update is a sixtieth of a second behind and carries the same
    // information. The partner's sprite keeps the position it had until the
    // queue drains, which it does within a few frames.
    if (NetLink_BacklogDepth() >= NET_POS_SKIP_DEPTH)
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

    // Player 1 owns the save, so Player 2's saved whereabouts have to come
    // from here. They arrive every frame anyway for the sprite; writing them
    // through costs nothing and means the record in the save is never more
    // than a frame stale, without the heavy record being re-sent just because
    // somebody took a step.
    if (NetLink_IsMaster())
    {
        struct CoopPlayer2 *rec = GetCoopPlayer2();

        if (rec->claimed)
        {
            rec->location.mapGroup = gCoopPeer.mapGroup;
            rec->location.mapNum = gCoopPeer.mapNum;
            // Back out of camera space. The broadcast carries map coords
            // plus MAP_OFFSET, because that is what gObjectEvents and the
            // spawn helper want; gSaveBlock1Ptr->pos, which this mirrors,
            // does not. Writing it through raw would resume Player 2 seven
            // tiles from where they stood, every session.
            rec->pos.x = gCoopPeer.x - MAP_OFFSET;
            rec->pos.y = gCoopPeer.y - MAP_OFFSET;
        }
    }

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

// ---------------------------------------------------------------------------
// The partner's follower Pokemon.
//
// Mirrored the same way the partner themselves is: a map object spawned under
// a reserved local id, told where to be. What is NOT mirrored is its position.
//
// A follower does not need one. It occupies the tile its trainer just left, so
// it can be driven entirely from the partner's own movement, which is already
// arriving sixty times a second. Broadcasting a second set of coordinates
// would double the busiest message in the protocol to say something both
// consoles can already work out -- and the send queue is the scarce thing
// here, as a session's worth of dropped gym badges established.
//
// So only the species travels, and only when it changes.
// ---------------------------------------------------------------------------

static EWRAM_DATA u8 sPeerFollowerObjectId = 0;

// The tile the partner's follower is walking to: the one the partner was
// standing on before their most recent step.
//
// Held rather than recomputed, and this is the whole subtlety. "Where were
// they before this frame" is the partner's current tile for every frame they
// are standing still, so a follower driven off that walks onto its trainer
// and stands inside them -- which looks, from the outside, exactly like a
// follower that is working, because one sprite is drawn over the other.
static EWRAM_DATA s16 sPeerFollowerToX = 0;
static EWRAM_DATA s16 sPeerFollowerToY = 0;
static EWRAM_DATA u16 sPeerFollowerSpecies = SPECIES_NONE;
static EWRAM_DATA u8 sPeerFollowerFlags = 0;

// What we last told them about ours, so a change can be noticed.
static EWRAM_DATA u16 sFollowerSentSpecies = SPECIES_NONE;
static EWRAM_DATA u8 sFollowerSentFlags = 0;
static EWRAM_DATA u8 sFollowerSendsLeft = 0;
static EWRAM_DATA u8 sFollowerCheckTimer = 0;

#define COOP_FOLLOWER_SHINY   (1 << 0)
#define COOP_FOLLOWER_FEMALE  (1 << 1)

// Repeats, because this is sent on change rather than every frame: a lost one
// would leave the partner looking at the wrong Pokemon until the next swap.
#define FOLLOWER_SENDS 4

// Cheap, but not worth doing sixty times a second to answer a question whose
// answer changes when the party leader does.
#define FOLLOWER_CHECK_INTERVAL 30

static struct ObjectEvent *GetPeerFollowerObject(void)
{
    struct ObjectEvent *obj;

    if (sPeerFollowerObjectId >= OBJECT_EVENTS_COUNT)
        return NULL;

    obj = &gObjectEvents[sPeerFollowerObjectId];

    // Same check the partner's own object needs: a map change resets every
    // object event, so the slot we remember may be inactive or reused.
    if (!obj->active || obj->localId != COOP_PEER_FOLLOWER_LOCAL_ID)
    {
        sPeerFollowerObjectId = OBJECT_EVENTS_COUNT;
        return NULL;
    }

    return obj;
}

static void DespawnPeerFollower(void)
{
    if (GetPeerFollowerObject() != NULL)
    {
        RemoveObjectEventByLocalIdAndMap(COOP_PEER_FOLLOWER_LOCAL_ID,
                                         gSaveBlock1Ptr->location.mapNum,
                                         gSaveBlock1Ptr->location.mapGroup);
    }
    sPeerFollowerObjectId = OBJECT_EVENTS_COUNT;
}

// Whether the partner's follower can be drawn on the map we are both standing
// on. The same rules the game applies to our own: a Pokemon with no overworld
// sprite cannot be drawn at all, and one whose sprite is bigger than a tile
// does not fit indoors.
static bool8 PeerFollowerFits(void)
{
    const struct ObjectEventGraphicsInfo *info;

    if (sPeerFollowerSpecies == SPECIES_NONE)
        return FALSE;

    info = SpeciesToGraphicsInfo(sPeerFollowerSpecies,
                                 (sPeerFollowerFlags & COOP_FOLLOWER_SHINY) != 0,
                                 (sPeerFollowerFlags & COOP_FOLLOWER_FEMALE) != 0);
    if (info == NULL)
        return FALSE;

    if (gMapHeader.mapType == MAP_TYPE_INDOOR && info->oam->size > ST_OAM_SIZE_2)
        return FALSE;

    return TRUE;
}

static void SpawnPeerFollower(const struct ObjectEvent *peer)
{
    u16 gfxId = GetGraphicsIdForMon(sPeerFollowerSpecies,
                                    (sPeerFollowerFlags & COOP_FOLLOWER_SHINY) != 0,
                                    (sPeerFollowerFlags & COOP_FOLLOWER_FEMALE) != 0);
    // On top of the partner, which is where the game puts a follower that has
    // just appeared too. Their next step pushes it into the tile behind them.
    u8 id = SpawnSpecialObjectEventParameterized(gfxId, MOVEMENT_TYPE_NONE,
                                                 COOP_PEER_FOLLOWER_LOCAL_ID,
                                                 peer->currentCoords.x,
                                                 peer->currentCoords.y,
                                                 peer->currentElevation);

    sPeerFollowerObjectId = id < OBJECT_EVENTS_COUNT ? id : OBJECT_EVENTS_COUNT;
}

// Walk the partner's follower into the tile they have just left.
//
// Called with the tile the partner occupied before this frame's step. A
// follower is always one tile behind its trainer, so that tile is exactly
// where this one belongs, and stepping it there rather than placing it gets
// the walk animation, the grass rustle and the reflections for nothing.
static void StepPeerFollowerTo(void)
{
    struct ObjectEvent *mon = GetPeerFollowerObject();
    s16 toX = sPeerFollowerToX;
    s16 toY = sPeerFollowerToY;
    s16 dx, dy;

    if (mon == NULL)
        return;

    if (ObjectEventClearHeldMovementIfFinished(mon) == 0 && mon->heldMovementActive)
        return;

    dx = toX - mon->currentCoords.x;
    dy = toY - mon->currentCoords.y;

    if (dx == 0 && dy == 0)
        return;

    if ((dx == 0 && (dy == 1 || dy == -1)) || (dy == 0 && (dx == 1 || dx == -1)))
    {
        u8 dir = dx == 1 ? DIR_EAST : dx == -1 ? DIR_WEST
               : dy == 1 ? DIR_SOUTH : DIR_NORTH;

        ObjectEventSetHeldMovement(mon, GetWalkNormalMovementAction(dir));
        return;
    }

    // Further than a step: the partner warped, hopped a ledge, or we missed
    // frames. Snap, for the same reason their own sprite does.
    MoveObjectEventToMapCoords(mon, toX, toY);
}

static void UpdatePeerFollower(struct ObjectEvent *peer)
{
    if (!PeerFollowerFits())
    {
        DespawnPeerFollower();
        return;
    }

    if (GetPeerFollowerObject() == NULL)
    {
        SpawnPeerFollower(peer);
        // Under them until they move, which is where the game puts a newly
        // spawned follower of its own too.
        sPeerFollowerToX = peer->currentCoords.x;
        sPeerFollowerToY = peer->currentCoords.y;
        return;
    }

    // Swap the sprite if they have changed who is at the front of their party.
    ObjectEventSetGraphicsId(GetPeerFollowerObject(),
        GetGraphicsIdForMon(sPeerFollowerSpecies,
                            (sPeerFollowerFlags & COOP_FOLLOWER_SHINY) != 0,
                            (sPeerFollowerFlags & COOP_FOLLOWER_FEMALE) != 0));

    StepPeerFollowerTo();
}

// Tell them about ours, when it changes.
static bool8 CoopSendFollowerMon(u16 *sendCmd)
{
    if (sFollowerSendsLeft == 0)
        return FALSE;

    sendCmd[0] = LINKCMD_COOP_FOLLOWER;
    sendCmd[1] = sFollowerSentSpecies;
    sendCmd[2] = sFollowerSentFlags;
    sFollowerSendsLeft--;

    return TRUE;
}

static void CheckOurFollower(void)
{
    u32 species = SPECIES_NONE;
    bool32 shiny = FALSE;
    bool32 female = FALSE;
    u8 flags = 0;

    if (++sFollowerCheckTimer < FOLLOWER_CHECK_INTERVAL)
        return;

    sFollowerCheckTimer = 0;

    // The same question the game asks before drawing our own, so the partner
    // is told about a follower exactly when there is one to see.
    if (!GetFollowerInfo(&species, &shiny, &female))
        species = SPECIES_NONE;

    if (shiny)
        flags |= COOP_FOLLOWER_SHINY;
    if (female)
        flags |= COOP_FOLLOWER_FEMALE;

    if ((u16)species == sFollowerSentSpecies && flags == sFollowerSentFlags)
        return;

    sFollowerSentSpecies = species;
    sFollowerSentFlags = flags;
    sFollowerSendsLeft = FOLLOWER_SENDS;
}

void Coop_ReceiveFollowerMon(u8 playerId, const u16 *cmd)
{
    if (playerId == GetMultiplayerId())
        return;

    sPeerFollowerSpecies = cmd[1];
    sPeerFollowerFlags = cmd[2] & 0xFF;
}

static void ResetPeerFollower(void)
{
    sPeerFollowerSpecies = SPECIES_NONE;
    sPeerFollowerFlags = 0;
    // Forget what we told them, so a reconnect says it again rather than
    // leaving the partner with whatever the last session left on screen.
    sFollowerSentSpecies = SPECIES_NONE;
    sFollowerSentFlags = 0;
    sFollowerSendsLeft = 0;
    sFollowerCheckTimer = 0;
}

void Coop_UpdatePeerSprite(void)
{
    struct ObjectEvent *peer;
    s16 dx, dy, wasX, wasY;

    sFrameCounter++;

    if (!IsCoopLinkActive() || !PeerIsOnOurMap() || PeerHasGoneQuiet())
    {
        DespawnPeerFollower();
        DespawnPeer();
        return;
    }

    peer = GetPeerObject();
    if (peer == NULL)
    {
        SpawnPeer();
        return;
    }

    // Where they are before anything this frame moves them. This is the tile
    // their follower walks into if they step.
    wasX = peer->currentCoords.x;
    wasY = peer->currentCoords.y;

    // Swap the sprite when they get on a bike, surf, and so on. Cheap to call
    // every frame -- it returns immediately when the id is unchanged.
    ObjectEventSetGraphicsId(peer,
        GetRivalAvatarGraphicsIdByStateIdAndGender(gCoopPeer.avatarState,
                                                   gCoopPeer.gender));

    // Let any walk already in progress finish before starting another, or the
    // sprite stutters in place instead of sliding between tiles.
    if (ObjectEventClearHeldMovementIfFinished(peer) == 0
        && peer->heldMovementActive)
    {
        // Their follower is on its own clock -- it is a step behind by
        // definition, so it is usually still walking when they have stopped,
        // and would stand frozen half a tile out if this returned first.
        UpdatePeerFollower(peer);
        return;
    }

    dx = (s16)gCoopPeer.x - peer->currentCoords.x;
    dy = (s16)gCoopPeer.y - peer->currentCoords.y;

    if (dx == 0 && dy == 0)
    {
        if (peer->facingDirection != gCoopPeer.facing)
            ObjectEventTurn(peer, gCoopPeer.facing);
        // Target deliberately untouched: they are standing still, so the tile
        // behind them has not changed.
        UpdatePeerFollower(peer);
        return;
    }

    // Exactly one tile in a cardinal direction: animate the step, so the
    // partner slides naturally and triggers grass rustle, reflections and the
    // rest of the ground effects for free.
    if ((dx == 0 && (dy == 1 || dy == -1)) || (dy == 0 && (dx == 1 || dx == -1)))
    {
        u8 dir = dx == 1 ? DIR_EAST : dx == -1 ? DIR_WEST
               : dy == 1 ? DIR_SOUTH : DIR_NORTH;

        // They are leaving this tile, so it is the one their follower wants.
        sPeerFollowerToX = wasX;
        sPeerFollowerToY = wasY;

        ObjectEventSetHeldMovement(peer, GetWalkNormalMovementAction(dir));
        UpdatePeerFollower(peer);
        return;
    }

    // Anything else is a warp, a ledge hop, or us having missed frames. Snap
    // rather than trying to animate a path we never saw them take.
    MoveObjectEventToMapCoords(peer, gCoopPeer.x, gCoopPeer.y);
    ObjectEventTurn(peer, gCoopPeer.facing);
    // Nothing was walked, so there is no trail to follow. Put it with them and
    // let the next real step pull it into place behind.
    sPeerFollowerToX = gCoopPeer.x;
    sPeerFollowerToY = gCoopPeer.y;
    UpdatePeerFollower(peer);
}

// ---------------------------------------------------------------------------
// First run: who you are, and which Pokemon you start with.
//
// The opening asks both of those, and a co-op game does not have an opening.
// See data/scripts/coop.inc for the script itself.
//
// Armed per console, never shared. "Have I been set up" is not a question the
// two players answer together -- a flag would be, because flags are Player 1's
// and are copied wholesale to Player 2 on join, so Player 1 finishing their
// setup would have told Player 2 it was already done.
// ---------------------------------------------------------------------------

static EWRAM_DATA bool8 sFirstRunPending = FALSE;

// Set while the first-run script is actually on screen.
//
// Separate from the pending flag, which is cleared the moment the script
// starts. Anything outside wanting to know "is this console in the opening
// questions" -- the test rig, mainly -- cannot tell that from SCRIPT_BUSY,
// which is equally true of a signpost.
static EWRAM_DATA bool8 sFirstRunRunning = FALSE;

void Coop_ArmFirstRun(void)
{
    sFirstRunPending = TRUE;
}

void Coop_CancelFirstRun(void)
{
    sFirstRunPending = FALSE;
}

bool8 Coop_FirstRunIsRunning(void)
{
    return sFirstRunRunning;
}

bool8 Coop_FirstRunIsPending(void)
{
    return sFirstRunPending;
}

static void Coop_UpdateFirstRun(void)
{
    if (!sFirstRunPending)
        return;

    // Not before the session is up. Player 2's own setup is skipped entirely
    // when Player 1's save already remembers them, and that is only known
    // once the join has delivered the stored record.
    if (!IsCoopLinkActive())
        return;

    // The same wait every other deferred script here uses: nothing else
    // running, the player in control and standing still.
    if (ArePlayerFieldControlsLocked() || ScriptContext_IsEnabled()
        || gPaletteFade.active || !IsPlayerStandingStill())
        return;

    sFirstRunPending = FALSE;
    sFirstRunRunning = TRUE;
    ScriptContext_SetupScript(CoopEventScript_FirstRun);
}

// Set while the naming screen has the console.
//
// The script-status mirror cannot cover this one: it is published from the
// overworld, and the naming screen replaces the overworld, so the mirror
// simply freezes at whatever it last said. A test driving these prompts has
// to know when the keyboard is up and when it has gone, and "the value
// stopped changing" is not something it can wait on.
static EWRAM_DATA bool8 sFirstRunNaming = FALSE;

bool8 Coop_FirstRunNamingScreenIsUp(void)
{
    return sFirstRunNaming;
}

static void CB2_CoopFirstRunNamed(void)
{
    sFirstRunNaming = FALSE;
    SetMainCallback2(CB2_ReturnToFieldContinueScript);
}

void CoopDoPlayerNamingScreen(void)
{
    sFirstRunNaming = TRUE;
    DoNamingScreen(NAMING_SCREEN_PLAYER, gSaveBlock2Ptr->playerName,
                   gSaveBlock2Ptr->playerGender, 0, 0,
                   CB2_CoopFirstRunNamed);
}

void Coop_SetGenderMale(struct ScriptContext *ctx)
{
    gSaveBlock2Ptr->playerGender = MALE;
}

void Coop_SetGenderFemale(struct ScriptContext *ctx)
{
    gSaveBlock2Ptr->playerGender = FEMALE;
}

// The chosen starter, from VAR_TEMP_E.
//
// Level 5 and no held item, like every starter the game hands out. Given here
// rather than with the script's own givemon so the species can come from a
// variable -- givemon takes a constant, and nine regions of three would
// otherwise be twenty-seven copies of the same four lines.
void Coop_GiveChosenStarter(struct ScriptContext *ctx)
{
    u16 species = VarGet(VAR_TEMP_E);

    if (species == SPECIES_NONE)
        return;

    ScriptGiveMon(species, 5, ITEM_NONE);

    // Which of the three it was, for the parts of the story that ask what you
    // started with -- the rival's team, mostly.
    //
    // Approximate on purpose, and only Player 1 writes it. The var holds an
    // index into one region's three, which is a question with no answer once
    // the choice spans nine regions; the slot is the nearest honest reading
    // of it. And it is a shared var, so if both players wrote it the second
    // to choose would silently overwrite the first.
    if (NetLink_IsMaster() || !gNetLinkActive)
        VarSet(VAR_STARTER_MON, VarGet(VAR_TEMP_F));
}

// Setup is over on this console.
//
// The gender and the name are already in SaveBlock2 and the starter is in the
// party, and all three travel to the partner by the ordinary routes: the
// record for Player 2, the position broadcast for the sprite.
void Coop_FirstRunDone(struct ScriptContext *ctx)
{
    sFirstRunPending = FALSE;
    sFirstRunRunning = FALSE;
    sFirstRunNaming = FALSE;

    // Nothing is written to a flag to remember this. A player who quits in
    // the middle of being asked is asked again from the top next session,
    // which is the right outcome and costs nothing to arrange: the arming is
    // driven by the two things that are already persistent -- a brand-new
    // save on Player 1's side, and an unclaimed record on Player 2's.
}
