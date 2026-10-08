#include "global.h"
#include "coop_sync.h"
#include "coop.h"
#include "link.h"

// See include/coop_sync.h for why this exists rather than using SendBlock.

// The largest payload any stream carries. Sized to struct CoopPlayer2 with room
// to spare; the static assert below is what actually keeps it honest.
// Sized by the largest stream, which is the shared world. The static asserts
// below are what actually keep this honest; they have already caught one
// overflow when the bag was added.
#define COOP_SYNC_BUFFER_SIZE 4096

STATIC_ASSERT(sizeof(struct CoopPlayer2) <= COOP_SYNC_BUFFER_SIZE, CoopSyncBufferTooSmallForPlayer2);
STATIC_ASSERT(sizeof(struct CoopWorldState) <= COOP_SYNC_BUFFER_SIZE, CoopSyncBufferTooSmallForWorld);

// Outgoing. src points at the caller's data and is read chunk by chunk rather
// than copied, which is safe because every sender here is a long-lived save
// structure, not a stack temporary.
static EWRAM_DATA const u8 *sSendSrc = NULL;
static EWRAM_DATA u16 sSendSize = 0;
static EWRAM_DATA u16 sSendPos = 0;
static EWRAM_DATA u8 sSendStream = COOP_STREAM_NONE;

// Incoming. One buffer, because a session never has two transfers in flight --
// the state machine starts one and waits for it.
static EWRAM_DATA u8 sRecvBuf[COOP_SYNC_BUFFER_SIZE] = {0};
static EWRAM_DATA u16 sRecvSize = 0;
static EWRAM_DATA u8 sRecvStream = COOP_STREAM_NONE;
static EWRAM_DATA bool8 sRecvComplete = FALSE;

static u16 ChunkCount(u16 size)
{
    return (size + COOP_CHUNK_BYTES - 1) / COOP_CHUNK_BYTES;
}

void CoopSync_Send(enum CoopStream stream, const void *src, u16 size)
{
    sSendSrc = src;
    sSendSize = size;
    sSendPos = 0;
    sSendStream = stream;
}

bool8 CoopSync_IsSending(void)
{
    return sSendStream != COOP_STREAM_NONE && sSendPos < sSendSize;
}

bool8 CoopSync_SendChunk(u16 *sendCmd)
{
    u16 index, i;
    u8 bytes[COOP_CHUNK_BYTES];

    if (!CoopSync_IsSending())
        return FALSE;

    index = sSendPos / COOP_CHUNK_BYTES;

    // Zero the tail so the final, partial chunk does not leak whatever happened
    // to follow the payload in memory.
    for (i = 0; i < COOP_CHUNK_BYTES; i++)
        bytes[i] = (sSendPos + i < sSendSize) ? sSendSrc[sSendPos + i] : 0;

    sendCmd[0] = LINKCMD_COOP_BULK;
    // Stream in the high nibble, chunk index in the low 12 bits.
    sendCmd[1] = ((u16)sSendStream << 12) | (index & 0xFFF);
    // Total size rides on every chunk rather than only the first. One command
    // is one link frame, and a receiver that joined late or lost the first
    // frame would otherwise have no way to know how much to expect.
    sendCmd[2] = sSendSize;
    for (i = 0; i < (COOP_CHUNK_BYTES / 2); i++)
        sendCmd[3 + i] = bytes[i * 2] | ((u16)bytes[i * 2 + 1] << 8);

    sSendPos += COOP_CHUNK_BYTES;

    if (sSendPos >= sSendSize)
        sSendStream = COOP_STREAM_NONE;

    return TRUE;
}

void CoopSync_ReceiveChunk(const u16 *cmd)
{
    u8 stream = (cmd[1] >> 12) & 0xF;
    u16 index = cmd[1] & 0xFFF;
    u16 size = cmd[2];
    u16 offset = index * COOP_CHUNK_BYTES;
    u16 i;

    if (stream == COOP_STREAM_NONE || size == 0 || size > COOP_SYNC_BUFFER_SIZE)
        return;

    // A chunk index past the payload is nonsense; drop it rather than writing
    // outside the buffer.
    if (offset >= size)
        return;

    // Chunk 0 starts a transfer. Anything else arriving for a stream we are not
    // mid-receive on is a leftover from an abandoned one -- ignore it, or it
    // would be stitched into the next transfer at the wrong offset.
    if (index == 0)
    {
        sRecvStream = stream;
        sRecvSize = size;
        sRecvComplete = FALSE;
    }
    else if (sRecvStream != stream || sRecvComplete)
    {
        return;
    }

    for (i = 0; i < COOP_CHUNK_BYTES && offset + i < size; i++)
        sRecvBuf[offset + i] = cmd[3 + (i / 2)] >> ((i & 1) ? 8 : 0);

    if (index + 1 >= ChunkCount(size))
        sRecvComplete = TRUE;
}

bool8 CoopSync_HasReceived(enum CoopStream stream)
{
    return sRecvComplete && sRecvStream == stream;
}

const void *CoopSync_GetReceived(enum CoopStream stream, u16 *sizeOut)
{
    if (!CoopSync_HasReceived(stream))
        return NULL;

    if (sizeOut != NULL)
        *sizeOut = sRecvSize;

    return sRecvBuf;
}

void CoopSync_ClearReceived(void)
{
    sRecvSize = 0;
    sRecvStream = COOP_STREAM_NONE;
    sRecvComplete = FALSE;
}

void CoopSync_Reset(void)
{
    sSendSrc = NULL;
    sSendSize = 0;
    sSendPos = 0;
    sSendStream = COOP_STREAM_NONE;
    sRecvSize = 0;
    sRecvStream = COOP_STREAM_NONE;
    sRecvComplete = FALSE;
}
