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

struct CoopPlayer2 *GetCoopPlayer2(void)
{
    return &gSaveBlock1Ptr->coopPlayer2;
}

// Snapshot the shared world: badges and story flags, and the Pokedex.
static void GatherWorldState(struct CoopWorldState *out)
{
    u16 i;

    for (i = 0; i < NUM_FLAG_BYTES; i++)
        out->flags[i] = gSaveBlock1Ptr->flags[i];

    for (i = 0; i < NUM_DEX_FLAG_BYTES; i++)
    {
        out->dexSeen[i] = gSaveBlock1Ptr->dexSeen[i];
        out->dexCaught[i] = gSaveBlock1Ptr->dexCaught[i];
    }
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

    for (i = 0; i < NUM_DEX_FLAG_BYTES; i++)
    {
        gSaveBlock1Ptr->dexSeen[i] |= in->dexSeen[i];
        gSaveBlock1Ptr->dexCaught[i] |= in->dexCaught[i];
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

bool8 IsCoopSessionPaired(void)
{
    return gNetLinkActive && NetLink_GetHostStatus() == NET_HOST_READY
        && NetLink_GetPlayerCount() == NET_MAX_PLAYERS;
}

u8 GetCoopState(void)
{
    return sCoopState;
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
    CoopSync_Reset();
}

static void CoopSendPositionCB(void);
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

    gNetMailbox.coopState = sCoopState;
    gNetMailbox.linkFlags = flags;
    gNetMailbox.joinStep = sJoinStep;
    gNetMailbox.peerMap = gCoopPeer.mapGroup | ((u16)gCoopPeer.mapNum << 8);
    gNetMailbox.peerX = gCoopPeer.x;
    gNetMailbox.peerY = gCoopPeer.y;
    gNetMailbox.selfMap = gSaveBlock1Ptr->location.mapGroup
                        | ((u16)gSaveBlock1Ptr->location.mapNum << 8);
    gNetMailbox.peerObjectId = sPeerObjectId;
}

void Coop_Update(void)
{
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
#define COOP_PEER_LOCAL_ID 0xF0

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
