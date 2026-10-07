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
static EWRAM_DATA bool8 sSentPlayer2Record = FALSE;
// Staging for an outgoing record. The transfer reads its source across
// many frames, so it cannot point at a caller's stack.
static EWRAM_DATA struct CoopPlayer2 sOutgoingRecord = {0};
// Player 2 only: the stored character handed back by Player 1, waiting to be
// taken over. Held rather than applied immediately because adopting an identity
// and party needs a safe moment, not whichever frame the last chunk landed on.
static EWRAM_DATA struct CoopPlayer2 sPendingRecord = {0};
static EWRAM_DATA bool8 sHasPendingRecord = FALSE;
static EWRAM_DATA bool8 sHandledPlayer2Record = FALSE;
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
    sSentPlayer2Record = FALSE;
    sHandledPlayer2Record = FALSE;
    sHasPendingRecord = FALSE;
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

        // The character hand-over. Strictly ordered, and the ordering is the
        // whole point.
        //
        // Both sides sending on connect looked symmetric and was wrong twice
        // over. Player 2 reporting its CURRENT identity would land at Player 1
        // and overwrite the stored character before it could be handed back --
        // destroying the partner's save on every join. And two transfers of the
        // same stream in flight at once interleave in CoopSync's single receive
        // buffer, because chunk 0 from either one restarts it.
        //
        // So it is a request and a response. Player 1 speaks first, always;
        // Player 2 answers only when it has something to say. One transfer in
        // flight, ever.
        if (NetLink_IsMaster())
        {
            if (!sSentPlayer2Record)
            {
                CoopSync_Send(COOP_STREAM_PLAYER2, GetCoopPlayer2(),
                              sizeof(struct CoopPlayer2));
                sSentPlayer2Record = TRUE;
            }
            else if (CoopSync_HasReceived(COOP_STREAM_PLAYER2))
            {
                // Player 2 answering with who they are. Store it.
                u16 size;
                const void *rec = CoopSync_GetReceived(COOP_STREAM_PLAYER2, &size);

                if (rec != NULL && size == sizeof(struct CoopPlayer2))
                    *GetCoopPlayer2() = *(const struct CoopPlayer2 *)rec;

                CoopSync_Reset();
            }
        }
        else if (sHasPendingRecord)
        {
            TryAdoptPendingRecord();
        }
        else if (!sHandledPlayer2Record && CoopSync_HasReceived(COOP_STREAM_PLAYER2))
        {
            u16 size;
            const void *rec = CoopSync_GetReceived(COOP_STREAM_PLAYER2, &size);

            if (rec != NULL && size == sizeof(struct CoopPlayer2))
            {
                const struct CoopPlayer2 *stored = rec;

                sHandledPlayer2Record = TRUE;
                CoopSync_Reset();

                if (stored->claimed)
                {
                    // A character is waiting for us. Held rather than applied
                    // here: TryAdoptPendingRecord waits for a frame where the
                    // player is actually in control.
                    sPendingRecord = *stored;
                    sHasPendingRecord = TRUE;
                }
                else
                {
                    // Nobody has ever joined this save. We are the first, so we
                    // claim the slot with whoever we currently are.
                    GatherLocalPlayerRecord(&sOutgoingRecord);
                    CoopSync_Send(COOP_STREAM_PLAYER2, &sOutgoingRecord,
                                  sizeof(sOutgoingRecord));
                }
            }
            else
            {
                CoopSync_Reset();
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
