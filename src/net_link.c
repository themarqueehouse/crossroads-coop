#include "global.h"
#include "link.h"
#include "net_link.h"
#include "coop.h"

// ---------------------------------------------------------------------------
// Co-op network transport. See include/net_link.h for the shape of the seam.
//
// Two behaviours of the cable transport are load-bearing and reproduced here
// exactly; getting either wrong desyncs the game in ways that look random:
//
//  1. (Superseded -- see NetEnqueueSendCmd.) The cable drops all-zero SEND
//     commands, but its receives are clocked by hardware regardless. The game writes gSendCmd
//     every frame but it is zero on most of them, and the cable code only
//     advances its queue when at least one word was nonzero (sSendNonzeroCheck
//     in EnqueueSendCmd). If we relayed the zeros, every state machine above us
//     that counts commands would run fast.
//
//  2. Receives are atomic across players. One cable queue slot held every
//     player's command for the same link frame, and DequeueRecvCmds popped the
//     whole row or nothing. So we only deliver when BOTH players have a frame
//     waiting; otherwise we deliver nothing and raise receivedNothing. This is
//     the lockstep discipline - it is what makes a socket behave like a cable.
// ---------------------------------------------------------------------------

EWRAM_DATA struct NetMailbox gNetMailbox = {0};
EWRAM_DATA bool8 gNetLinkActive = FALSE;

// Mirrors gLink.state so the status word we synthesize follows the same
// progression the game expects from the cable path.
static EWRAM_DATA u8 sNetState = 0;
static EWRAM_DATA u8 sNetQueueFull = 0;

// Commands the outbound ring had no room for, kept in the ROM rather than the
// mailbox: the mailbox is a fixed-size contract shared with the browser
// wrapper and cannot grow. Deep enough to ride out the bursts a battle turn
// produces; the ring is emptied by the relay every frame, so a backlog that
// is not drained within a frame or two does not happen.
#define NET_SEND_BACKLOG 64
static EWRAM_DATA struct NetFrame sBacklog[NET_SEND_BACKLOG];
static EWRAM_DATA u8 sBacklogHead = 0;
static EWRAM_DATA u8 sBacklogTail = 0;

// How often a command was lost outright because the ring AND the backlog were
// both full, and how deep the backlog ever got. Without the first of these,
// "some messages went missing" cannot be told apart from a fault further down
// the transport, and the backlog would just get bigger on a hunch.
EWRAM_DATA u16 gCoopDbgSendDrops = 0;

// Position updates thrown out to make room for something that mattered.
// A healthy session has a few; a steady climb means one console is
// consistently outrunning the other and the backlog never really drains.
EWRAM_DATA u16 gCoopDbgPosEvicted = 0;

// Throw away the oldest queued position update, closing the gap so everything
// else keeps the order it was queued in. FALSE if there was none to throw.
static bool8 EvictOldestPosition(void)
{
    u8 at = sBacklogTail;
    u8 found = 0xFF;

    while (at != sBacklogHead)
    {
        if (sBacklog[at].cmd[0] == LINKCMD_COOP_POS)
        {
            found = at;
            break;
        }
        at = (u8)((at + 1) % NET_SEND_BACKLOG);
    }

    if (found == 0xFF)
        return FALSE;

    while (TRUE)
    {
        u8 next = (u8)((found + 1) % NET_SEND_BACKLOG);

        if (next == sBacklogHead)
            break;

        sBacklog[found] = sBacklog[next];
        found = next;
    }

    sBacklogHead = found;
    return TRUE;
}
EWRAM_DATA u8 gCoopDbgBacklogMax = 0;
static EWRAM_DATA bool8 sNetReceivedNothing = FALSE;

static bool8 RingHasData(u8 head, u8 tail)
{
    return head != tail;
}

static u8 RingCount(u8 head, u8 tail)
{
    return (u8)((head - tail) & NET_RING_MASK);
}

void NetLink_Init(void)
{
    u32 i;
    u8 *p = (u8 *)&gNetMailbox;

    for (i = 0; i < sizeof(gNetMailbox); i++)
        p[i] = 0;

    // Written last: the host polls for the magic to locate us, and must not
    // see a half-initialized mailbox.
    gNetMailbox.version = NET_PROTOCOL_VERSION;
    gNetMailbox.magic = NET_MAILBOX_MAGIC;

    sNetState = LINK_STATE_START0;
    sNetQueueFull = QUEUE_FULL_NONE;
    sNetReceivedNothing = FALSE;
    gNetLinkActive = FALSE;
    sBacklogHead = 0;
    sBacklogTail = 0;
}

void NetLink_Reset(void)
{
    gNetMailbox.outHead = 0;
    gNetMailbox.outTail = 0;
    gNetMailbox.inHead[0] = 0;
    gNetMailbox.inHead[1] = 0;
    gNetMailbox.inTail[0] = 0;
    gNetMailbox.inTail[1] = 0;
    sNetState = LINK_STATE_START0;
    sNetQueueFull = QUEUE_FULL_NONE;
    sNetReceivedNothing = FALSE;
    // Whatever was held over belonged to the session being torn down.
    sBacklogHead = 0;
    sBacklogTail = 0;
}

u8 NetLink_GetHostStatus(void)
{
    if (gNetMailbox.magic != NET_MAILBOX_MAGIC)
        return NET_HOST_DOWN;

    return gNetMailbox.hostStatus;
}

// A plain emulator never touches the mailbox, so hostStatus stays DOWN and we
// report no co-op support. Once the wrapper has moved us off DOWN even once we
// latch gNetLinkActive, so a later dropout reads as "partner lost" rather than
// silently falling back to the cable path mid-session.
bool8 NetLink_HostSupportsCoop(void)
{
    u8 status = NetLink_GetHostStatus();

    if (status != NET_HOST_DOWN)
        gNetLinkActive = TRUE;

    return gNetLinkActive;
}

u8 NetLink_GetMultiplayerId(void)
{
    u8 id = gNetMailbox.localId;

    if (id >= NET_MAX_PLAYERS)
        return 0;

    return id;
}

u8 NetLink_GetPlayerCount(void)
{
    u8 count = gNetMailbox.playerCount;

    if (count > NET_MAX_PLAYERS)
        return NET_MAX_PLAYERS;

    return count;
}

bool8 NetLink_IsMaster(void)
{
    return NetLink_GetMultiplayerId() == 0;
}

// Whether the last enqueue attempt was turned away for want of room, so the
// caller can stall the frame instead of letting the command be overwritten.
bool8 NetLink_SendQueueWasFull(void)
{
    return sNetQueueFull == QUEUE_FULL_SEND;
}

// How many commands are waiting for room in the outbound ring.
//
// This is the signal that the two consoles have drifted apart in speed. Both
// produce and consume exactly one frame per game frame, so a backlog that
// GROWS means this console is running more game frames per second than the
// other -- and nothing in the transport slows it down. Measured over a gym
// battle, one console sat at a depth of seven all the way through while the
// other climbed 18, 25, 47, 63 and then began losing commands outright.
// Two phones will never run at exactly the same speed, so left alone this
// ends every long session in a hang.
u8 NetLink_BacklogDepth(void)
{
    return (u8)((sBacklogHead + NET_SEND_BACKLOG - sBacklogTail) % NET_SEND_BACKLOG);
}

u32 NetLink_GetSendQueueLength(void)
{
    return RingCount(gNetMailbox.outHead, gNetMailbox.outTail);
}

// The game throttles the overworld on this (OVERWORLD_RECV_QUEUE_MAX), so it
// must reflect the shallowest player ring - the one holding everyone up.
u32 NetLink_GetRecvQueueLength(void)
{
    u8 count = NetLink_GetPlayerCount();
    u32 shallowest = NET_RING_SLOTS;
    u8 i;

    if (count == 0)
        return 0;

    for (i = 0; i < count; i++)
    {
        u32 n = RingCount(gNetMailbox.inHead[i], gNetMailbox.inTail[i]);

        if (n < shallowest)
            shallowest = n;
    }

    return shallowest;
}

// Emits exactly one frame per link frame, even when the game has nothing to
// say.
//
// This deliberately does NOT reproduce EnqueueSendCmd's drop-if-all-zero rule,
// and getting that wrong deadlocked the whole session. The reasoning that led
// there was that the cable code only advances its send queue when some word is
// nonzero (sSendNonzeroCheck), so relaying zeros would make command-counting
// state machines above us run fast.
//
// That is true of the SEND queue and false of the transport. On real hardware
// the serial interrupt fires every link frame and every console receives a full
// row of commands regardless of whether anyone had anything to send -- receives
// are driven by the cable's clock, not by the send queue. Dropping zero frames
// here made a player's receives depend on its own sends, and the two sides
// deadlocked on the first exchange: the master sent LINKCMD_SEND_LINK_TYPE and
// waited, while the slave had nothing to say until it received that very
// command, so it never sent, so neither side's lockstep could advance. The
// symptom was a link that reported CONN_ESTABLISHED and then sat there.
//
// Relaying zeros is safe for exactly the reason the drop rule existed to
// prevent: ProcessRecvCmds skips any entry whose command word is 0, so an idle
// frame costs a ring slot and advances nothing else.
// Push held-over commands into the outbound ring, oldest first.
//
// Order matters: these are halves of block transfers, and a block delivered
// out of order is as broken as one delivered short. Separate from
// NetEnqueueSendCmd so a console that is holding itself back can still pay off
// what it already owes -- otherwise the backlog it is waiting on can never
// drain and the stall only ends when the safety cap fires.
void NetLink_DrainBacklog(void)
{
    u8 head = gNetMailbox.outHead;
    u8 tail = gNetMailbox.outTail;
    u8 i;

    while (sBacklogHead != sBacklogTail && RingCount(head, tail) < NET_RING_MASK)
    {
        for (i = 0; i < CMD_LENGTH; i++)
            gNetMailbox.out[head & NET_RING_MASK].cmd[i] = sBacklog[sBacklogTail].cmd[i];

        head = (u8)((head + 1) & NET_RING_MASK);
        gNetMailbox.outHead = head;
        sBacklogTail = (u8)((sBacklogTail + 1) % NET_SEND_BACKLOG);
    }
}

static void NetEnqueueSendCmd(u16 *sendCmd)
{
    u8 head = gNetMailbox.outHead;
    u8 tail = gNetMailbox.outTail;
    u8 i;

    NetLink_DrainBacklog();
    head = gNetMailbox.outHead;
    // Re-read, not just head. Reading the tail before the drain and the head
    // after it compares two different moments, and the answer is always that
    // the ring is fuller than it is.
    tail = gNetMailbox.outTail;

    // Nothing to send is not worth a backlog slot, and the transport emits a
    // frame every link frame whether or not the game had anything to say.
    //
    // That last part is why this check has to be here and not only further
    // down. An idle frame carries nothing -- ProcessRecvCmds skips any entry
    // whose command word is zero -- but queued behind a backlog it still takes
    // a slot, and the game produces one every single frame. So a backlog that
    // became non-empty once could never empty again: for every slot the drain
    // freed, the next idle frame took it back. It stayed full for the rest of
    // the session, at about a second of latency, and everything the game
    // actually wanted to say went in behind a wall of nothing -- or, once it
    // was full, nowhere at all.
    if (sendCmd[0] == 0 && sBacklogHead != sBacklogTail)
        return;

    if (sBacklogHead != sBacklogTail || RingCount(head, tail) >= NET_RING_MASK)
    {
        // No room. Hold the command rather than drop it.
        //
        // Dropping is survivable in the overworld, where a missed position
        // update is corrected by the next one, and fatal in a battle, where
        // every frame carries part of a block transfer and a block is a whole
        // message. One lost message deadlocks the fight permanently: counting
        // them during a stalled gym battle showed 94 acknowledgements sent by
        // each console and 93 received by both, and that single missing one
        // left a battler owed an acknowledgement for ever, with every
        // controller idle and nothing left to send.
        //
        // The command is copied aside rather than left in gSendCmd for the
        // caller to retry. Holding the frame back instead -- skipping the
        // game's frame until the ring drained -- deadlocks the pair: a console
        // that is not running stops consuming, so the peer's ring fills too,
        // so neither ever drains. The session simply never came up.
        u8 next = (u8)((sBacklogHead + 1) % NET_SEND_BACKLOG);

        if (next != sBacklogTail)
        {
            for (i = 0; i < CMD_LENGTH; i++)
                sBacklog[sBacklogHead].cmd[i] = sendCmd[i];

            sBacklogHead = next;
        }
        else if (sendCmd[0] != LINKCMD_COOP_POS && EvictOldestPosition())
        {
            // Full, but what is waiting in there is mostly where the player is
            // standing, and this is not. Position updates are the one thing in
            // the protocol that is safe to lose -- the next one corrects it a
            // sixtieth of a second later. Everything else is a one-off: a gate
            // announcement, a flag, a chunk of a transfer.
            //
            // Found the hard way. The two consoles do not run at exactly the
            // same speed, and the faster one's backlog fills with its own
            // position updates within a minute or two of play. After that the
            // next thing it tries to say is thrown away -- which is why the
            // joining player could announce arriving at a story gate four
            // times over and the host heard none of them, and sat at the gate
            // for ever with its partner standing right next to it.
            for (i = 0; i < CMD_LENGTH; i++)
                sBacklog[sBacklogHead].cmd[i] = sendCmd[i];

            sBacklogHead = (u8)((sBacklogHead + 1) % NET_SEND_BACKLOG);
            gCoopDbgPosEvicted++;
        }
        else
        {
            // Backlog full as well. Now it really is lost; say so.
            sNetQueueFull = QUEUE_FULL_SEND;
            gCoopDbgSendDrops++;
        }

        {
            // Signed arithmetic here read 251 for a depth of five and made the
            // watermark useless: head - tail promotes to int, and C's % keeps
            // the sign of a negative left operand.
            u8 depth = (u8)((sBacklogHead + NET_SEND_BACKLOG - sBacklogTail)
                            % NET_SEND_BACKLOG);

            if (depth > gCoopDbgBacklogMax)
                gCoopDbgBacklogMax = depth;
        }

        for (i = 0; i < CMD_LENGTH; i++)
            sendCmd[i] = 0;

        return;
    }

    for (i = 0; i < CMD_LENGTH; i++)
    {
        gNetMailbox.out[head & NET_RING_MASK].cmd[i] = sendCmd[i];
        sendCmd[i] = 0;
    }

    gNetMailbox.outHead = (u8)((head + 1) & NET_RING_MASK);
}

// Reproduces DequeueRecvCmds: all players or none.
static void NetDequeueRecvCmds(u16 (*recvCmds)[CMD_LENGTH])
{
    u8 count = NetLink_GetPlayerCount();
    bool8 allReady = TRUE;
    u8 i, j;

    if (count == 0)
        count = 1;

    for (i = 0; i < count; i++)
    {
        if (!RingHasData(gNetMailbox.inHead[i], gNetMailbox.inTail[i]))
            allReady = FALSE;
    }

    if (!allReady)
    {
        for (i = 0; i < count; i++)
        {
            for (j = 0; j < CMD_LENGTH; j++)
                recvCmds[i][j] = 0;
        }

        sNetReceivedNothing = TRUE;
        return;
    }

    for (i = 0; i < count; i++)
    {
        u8 tail = gNetMailbox.inTail[i];

        for (j = 0; j < CMD_LENGTH; j++)
            recvCmds[i][j] = gNetMailbox.in[i][tail & NET_RING_MASK].cmd[j];

        gNetMailbox.inTail[i] = (u8)((tail + 1) & NET_RING_MASK);
    }

    sNetReceivedNothing = FALSE;
}

u32 NetLinkMain1(u8 *shouldAdvanceLinkState, u16 *sendCmd, u16 (*recvCmds)[CMD_LENGTH])
{
    u8 status = NetLink_GetHostStatus();
    u8 localId;
    u8 playerCount;
    u32 retVal;

    // Backpressure is a condition of THIS frame, not a permanent state. The
    // cable transport gets away with latching it because gLink is re-zeroed
    // whenever the port is re-enabled; nothing re-zeroes this. Left latched,
    // a single ring overflow raises LINK_STAT_ERROR_QUEUE_FULL forever, and
    // TrySetLinkErrorBuffer turns that into CB2_LinkError within one frame --
    // so one burst of lag would permanently kill the session.
    sNetQueueFull = QUEUE_FULL_NONE;

    // The handshake is the host's job, not ours: it owns the socket and knows
    // when the peer has joined. We only mirror its verdict into the state
    // machine the game is watching.
    switch (sNetState)
    {
        case LINK_STATE_START0:
            NetLink_Reset();
            sNetState = LINK_STATE_START1;
            break;
        case LINK_STATE_START1:
            if (*shouldAdvanceLinkState == 1)
                sNetState = LINK_STATE_HANDSHAKE;
            break;
        case LINK_STATE_HANDSHAKE:
            if (*shouldAdvanceLinkState == 2)
            {
                sNetState = LINK_STATE_START0;
            }
            else if (status == NET_HOST_READY
                  && NetLink_GetPlayerCount() == NET_MAX_PLAYERS)
            {
                sNetState = LINK_STATE_CONN_ESTABLISHED;
            }
            break;
        case LINK_STATE_CONN_ESTABLISHED:
            NetEnqueueSendCmd(sendCmd);
            NetDequeueRecvCmds(recvCmds);
            break;
    }

    *shouldAdvanceLinkState = 0;

    localId = NetLink_GetMultiplayerId();
    playerCount = NetLink_GetPlayerCount();

    retVal = localId;
    retVal |= (u32)playerCount << LINK_STAT_PLAYER_COUNT_SHIFT;

    if (NetLink_IsMaster())
        retVal |= LINK_STAT_MASTER;

    if (sNetReceivedNothing)
        retVal |= 1 << LINK_STAT_RECEIVED_NOTHING_SHIFT;

    // Deliberately NOT reported. LINK_STAT_ERROR_QUEUE_FULL sits inside
    // LINK_STAT_ERRORS, and TrySetLinkErrorBuffer throws the game to
    // CB2_LinkError ("Communication error") on any error bit, on any frame.
    //
    // On a cable a full queue means something is genuinely broken. On a
    // network it means one moment of lag -- which on mobile data is routine
    // and entirely recoverable. Reporting it as an error turned normal jitter
    // into a session-ending error screen.
    //
    // Backpressure still does its job: GetLinkSendQueueLength reports the real
    // depth, so the overworld throttles the sender instead.

    if (sNetState == LINK_STATE_CONN_ESTABLISHED)
        retVal |= LINK_STAT_CONN_ESTABLISHED;

    // A vanished peer is never fatal in a co-op build.
    //
    // On a cable, LINK_STAT_ERROR_HARDWARE means the wire came out, and
    // CB2_LinkError's reset-and-complain is the right answer. Over a network a
    // peer going quiet means a phone locked, a tunnel re-established itself, or
    // a free-tier relay went to sleep -- all routine, all recoverable, and the
    // reset costs real unsaved progress.
    //
    // This was guarded on IsCoopLinkActive, so the error was only suppressed
    // once the handshake had finished. That left the opening and exchange phase
    // -- the longest and least reliable part of a session -- unprotected, and in
    // testing it produced exactly one "Communication error" per relay wake-up.
    // Widening the guard to the whole session still left a window each time the
    // state machine dropped back to OFF to retry.
    //
    // So: while a co-op wrapper is present, the session layer and the wrapper
    // own connection state entirely. The wrapper already shows the player
    // whether they are connected, which is the honest place for that signal; the
    // game simply waits. Revisit if link trades or battles ever ride this
    // transport, since those genuinely cannot continue without a peer -- they
    // would want a timeout of their own rather than this blunt one.

    if (localId >= MAX_LINK_PLAYERS)
        retVal |= LINK_STAT_ERROR_INVALID_ID;

    return retVal;
}
