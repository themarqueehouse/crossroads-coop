#ifndef GUARD_COOP_SYNC_H
#define GUARD_COOP_SYNC_H

#include "global.h"
#include "link.h"

// Bulk transfer for co-op, on top of the one-command-per-frame link.
//
// Why not SendBlock: the game's block layer receives into
// gBlockRecvBuffer[MAX_RFU_PLAYERS][BLOCK_BUFFER_SIZE / 2], and
// BLOCK_BUFFER_SIZE is 0x100 -- 256 bytes. Player 2's save record alone is 632,
// and the shared state that follows it is larger still. Growing that buffer
// would cost EWRAM for five link slots when co-op only ever has two, on a build
// already at 86% of EWRAM, and it would put our traffic in the same buffers a
// trade uses.
//
// So this is its own thing: a sequence of ordinary link commands, each carrying
// a chunk index and twelve bytes. At one command per frame that is 720 bytes a
// second -- Player 2's record in about a second, the whole shared state in
// about four. Slow for a running game, which is why this is only used at the
// moments where a short wait is honest: joining a session, and saving.
//
// The design is deliberately dumb. There is no windowing, no retransmission and
// no checksum, because the transport underneath already guarantees ordered,
// lossless, lockstep delivery -- it is a ring buffer the relay drains in order,
// not a datagram socket. If that ever stops being true, this needs a rethink
// rather than a patch.

// Payload bytes per command.
//
// A link command is CMD_LENGTH (8) words. Three carry the header -- the command
// id, the stream-and-chunk word, and the total size -- leaving five words, so
// TEN bytes, not twelve.
//
// Getting this wrong was not a short read; it was an out-of-bounds one. Writing
// a sixth payload word ran off the end of gSendCmd, and reading it ran off the
// end of gRecvCmds[i] into gRecvCmds[i + 1][0] -- the NEXT player's command id.
// So every chunk's last two bytes were quietly replaced by whatever the peer
// happened to be sending, usually LINKCMD_COOP_POS. Two bytes in every twelve,
// corrupted, in every transfer. It surfaced as a bag quantity of 0x3333.
#define COOP_CHUNK_BYTES 10

STATIC_ASSERT(COOP_CHUNK_BYTES / 2 + 3 <= CMD_LENGTH, CoopChunkOverrunsCommand);

// Which payload a transfer carries. Sent in the high nibble of the sequence
// word, so there is room for 15 of these and 4095 chunks each (49 KB).
enum CoopStream
{
    COOP_STREAM_NONE,
    // Player 2's character: identity, party, position. Player 1 holds it in
    // the save; this is how it gets handed over in each direction.
    COOP_STREAM_PLAYER2,
    // Shared progression: flags (badges and story), and the Pokedex.
    COOP_STREAM_WORLD,
};

/** Begin sending `size` bytes from `src` as `stream`. */
void CoopSync_Send(enum CoopStream stream, const void *src, u16 size);

/** True while a send is in progress. */
bool8 CoopSync_IsSending(void);

/**
 * Called from the link callback to emit the next chunk. Returns TRUE if it
 * wrote a command, so the caller knows not to send anything else this frame.
 */
bool8 CoopSync_SendChunk(u16 *sendCmd);

/** Feed a received chunk in. Called from ProcessRecvCmds. */
void CoopSync_ReceiveChunk(const u16 *cmd);

/**
 * True once `stream` has arrived complete. Clears on the next transfer of the
 * same stream, so a caller must act on it before starting another.
 */
bool8 CoopSync_HasReceived(enum CoopStream stream);

/** The received bytes for `stream`, or NULL if it has not arrived. */
const void *CoopSync_GetReceived(enum CoopStream stream, u16 *sizeOut);

/** Forget any in-flight or completed transfer. Called when a session ends. */
void CoopSync_Reset(void);

#endif // GUARD_COOP_SYNC_H
