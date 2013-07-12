#include "serial_framing_protocol.h"

#include <assert.h>
#include <stdio.h>

//////////////////////////////////////////////////////////////////////////////

#ifdef AVR
#include <util/crc16.h>
#else
/* Stolen from avr-libc's docs */
static uint16_t _crc_ccitt_update (uint16_t crc, uint8_t octet) {
  octet ^= crc & 0xff;
  octet ^= octet << 4;
  return ((((uint16_t)octet << 8) | ((crc >> 8) & 0xff)) ^ (uint8_t)(octet >> 4) ^ ((uint16_t)octet << 3));
}
#endif

//////////////////////////////////////////////////////////////////////////////

static int sfpIsReservedOctet (uint8_t octet);
static SFPseq sfpNextSeq (SFPseq seq);

static void sfpBufferedWrite (uint8_t octet, void *ctx);
static void sfpFlushWriteBuffer (SFPcontext *ctx);

static void sfpWriteFrameWithSeq (SFPcontext *ctx, SFPseq seq, SFPpacket *packet);
static void sfpWriteControlFrame (SFPcontext *ctx, SFPheader header);
static void sfpWriteUserFrame (SFPcontext *ctx, SFPpacket *packet);
static void sfpWriteNoCRC (SFPcontext *ctx, uint8_t octet);
static void sfpWrite (SFPcontext *ctx, uint8_t octet);
static int sfpIsTransmitterLockable (SFPcontext *ctx);

static void sfpBufferOctet (SFPcontext *ctx, uint8_t octet);
static void sfpHandleNAK (SFPcontext *ctx, SFPseq seq);
static void sfpSendNAK (SFPcontext *ctx);
static void sfpHandleControlFrame (SFPcontext *ctx);
static void sfpTryDeliverUserFrame (SFPcontext *ctx);
static void sfpResetReceiver (SFPcontext *ctx);
static void sfpTryDeliverFrame (SFPcontext *ctx);

//////////////////////////////////////////////////////////////////////////////

void sfpInit (SFPcontext *ctx) {
#ifdef SFP_DEBUG
  ctx->debugName[0] = '\0';
#endif

  ////////////////////////////////////////////////////////////////////////////

  ctx->rx.seq = SFP_INITIAL_SEQ;

  sfpResetReceiver(ctx);

  sfpSetDeliverCallback(ctx, NULL, NULL);

  ////////////////////////////////////////////////////////////////////////////
  
  ctx->tx.seq = SFP_INITIAL_SEQ;
  ctx->tx.crc = SFP_CRC_PRESET;

  ctx->tx.writebufn = 0;

  sfpSetWriteCallback(ctx, SFP_WRITE_ONE, NULL, NULL);
  sfpSetLockCallback(ctx, NULL, NULL);
  sfpSetUnlockCallback(ctx, NULL, NULL);

  RINGBUF_INIT(ctx->tx.history);
}

#ifdef SFP_DEBUG
void sfpSetDebugName (SFPcontext *ctx, const char *name) {
  assert(strlen(name) < SFP_MAX_DEBUG_NAME_SIZE);
  strcpy(ctx->debugName, name);
}
#endif

void sfpSetDeliverCallback (SFPcontext *ctx, SFPdeliverfun cbfun, void *userdata) {
  ctx->rx.deliver = cbfun;
  ctx->rx.deliverData = userdata;
}

void sfpSetWriteCallback (SFPcontext *ctx, SFPwritetype type,
    void *cbfun, void *userdata) {
  /* How this works: if the user wants to use SFP_WRITE_MULTIPLE, we still use
   * our write1 pointer, we just point it to our own function which buffers
   * the octets privately. sfpFlushWriteBuffer() then calls the user-provided
   * writen() function. If the user wants to use SFP_WRITE_ONE, then the
   * sfpFlushWriteBuffer() call is just a no-op. */
  switch (type) {
    case SFP_WRITE_ONE:
      ctx->tx.write1 = (SFPwrite1fun)cbfun;
      ctx->tx.write1Data = userdata;
      ctx->tx.writen = NULL;
      ctx->tx.writenData = NULL;
      break;
    case SFP_WRITE_MULTIPLE:
      ctx->tx.write1 = sfpBufferedWrite;
      ctx->tx.write1Data = ctx;
      ctx->tx.writen = (SFPwritenfun)cbfun;
      ctx->tx.writenData = userdata;
      break;
    default:
      assert(0);
  }
}

void sfpSetLockCallback (SFPcontext *ctx, SFPlockfun cbfun, void *userdata) {
  ctx->tx.lock = cbfun;
  ctx->tx.lockData = userdata;
}

void sfpSetUnlockCallback (SFPcontext *ctx, SFPunlockfun cbfun, void *userdata) {
  ctx->tx.unlock = cbfun;
  ctx->tx.unlockData = userdata;
}

/* Entry point for receiver. */
void sfpDeliverOctet (SFPcontext *ctx, uint8_t octet) {
  if (SFP_FLAG == octet) {
    if (SFP_FRAME_STATE_RECEIVING == ctx->rx.frameState) {
      sfpTryDeliverFrame(ctx);
    }
    /* If we receive a FLAG while in FRAME_STATE_NEW, this means we have
     * received back-to-back FLAG octets. This is a heartbeat/keepalive, and we
     * simply ignore them. */
    sfpResetReceiver(ctx);
  }
  else if (SFP_ESC == octet) {
    ctx->rx.escapeState = SFP_ESCAPE_STATE_ESCAPING;
  }
  else {
    /* All other, non-control octets. */

    if (SFP_ESCAPE_STATE_ESCAPING == ctx->rx.escapeState) {
      octet ^= SFP_ESC_FLIP_BIT;
      ctx->rx.escapeState = SFP_ESCAPE_STATE_NORMAL;
    }

#if 0
#ifdef SFP_DEBUG
    fprintf(stderr, "(sfp) DEBUG(%s): received data octet<0x%02x> CRC<0x%04x>\n", ctx->debugName,
        octet, ctx->rx.crc);
#endif
#endif


    if (SFP_FRAME_STATE_NEW == ctx->rx.frameState) {
      /* We are receiving the header. */

      ctx->rx.crc = _crc_ccitt_update(ctx->rx.crc, octet);
      ctx->rx.header = octet;
      ctx->rx.frameState = SFP_FRAME_STATE_RECEIVING;
    }
    else {
      /* We are receiving the payload. Since the CRC will be indistinguishable
       * from the rest of the payload until we receive the terminating FLAG
       * octet, we put the CRC calculation on a delay of SFP_CRC_SIZE octets. */

      if (SFP_CRC_SIZE <= ctx->rx.packet.len) {
        ctx->rx.crc = _crc_ccitt_update(ctx->rx.crc,
            ctx->rx.packet.buf[ctx->rx.packet.len - SFP_CRC_SIZE]);
      }

      sfpBufferOctet(ctx, octet);
    }
  }
}

/* Entry point for transmitter. */
void sfpWritePacket (SFPcontext *ctx, SFPpacket *packet) {
  if (sfpIsTransmitterLockable(ctx)) {
    ctx->tx.lock(ctx->tx.lockData);
  }

  RINGBUF_PUSH_BACK(ctx->tx.history, *packet);
  sfpWriteUserFrame(ctx, packet);

  if (sfpIsTransmitterLockable(ctx)) {
    ctx->tx.unlock(ctx->tx.unlockData);
  }
}

//////////////////////////////////////////////////////////////////////////////

static SFPseq sfpNextSeq (SFPseq seq) {
  return (seq + 1) & (SFP_SEQ_RANGE - 1);
}

static void sfpResetReceiver (SFPcontext *ctx) {
  ctx->rx.crc = SFP_CRC_PRESET;
  ctx->rx.escapeState = SFP_ESCAPE_STATE_NORMAL;
  ctx->rx.frameState = SFP_FRAME_STATE_NEW;
  ctx->rx.packet.len = 0;
}

static void sfpTryDeliverFrame (SFPcontext *ctx) {
  if (SFP_CRC_SIZE > ctx->rx.packet.len) {
#ifdef SFP_DEBUG
    fprintf(stderr, "(sfp) DEBUG(%s): RX<0x%02x \"", ctx->debugName, ctx->rx.header);
    fwrite(ctx->rx.packet.buf, 1, ctx->rx.packet.len, stderr);
    fprintf(stderr, "\">\n\tEXPECTED<0x%02x (payload) 0x%04x>\n",
        ctx->rx.seq, ctx->rx.crc);
    fflush(stderr);
#endif

#ifdef SFP_WARN
    fprintf(stderr, "(sfp) WARNING: short frame received, sending NAK.\n");
#endif
    sfpSendNAK(ctx);
    return;
  }

  uint8_t *pcrc = &ctx->rx.packet.buf[ctx->rx.packet.len - SFP_CRC_SIZE];
  SFPcrc crc = sfpByteSwapCRC(*(SFPcrc *)pcrc);
  ctx->rx.packet.len -= SFP_CRC_SIZE;

#ifdef SFP_DEBUG
  fprintf(stderr, "(sfp) DEBUG(%s): RX<0x%02x \"", ctx->debugName, ctx->rx.header);
  fwrite(ctx->rx.packet.buf, 1, ctx->rx.packet.len, stderr);
  fprintf(stderr, "\" 0x%04x>\n\tEXPECTED<0x%02x (payload) 0x%04x>\n",
      crc, ctx->rx.seq, ctx->rx.crc);
  fflush(stderr);
#endif

  if (0 == ctx->rx.packet.len) {
#ifdef SFP_DEBUG
    fprintf(stderr, "(sfp) DEBUG(%s): received control frame\n", ctx->debugName);
#endif

    if (crc != ctx->rx.crc) {
#ifdef SFP_WARN
      fprintf(stderr, "(sfp) WARNING: CRC mismatch, ignoring.\n");
#endif
      return;
    }

    sfpHandleControlFrame(ctx);
  }
  else {
#ifdef SFP_DEBUG
    fprintf(stderr, "(sfp) DEBUG(%s): received user frame\n", ctx->debugName);
#endif

    if (crc != ctx->rx.crc) {
#ifdef SFP_WARN
      fprintf(stderr, "(sfp) WARNING: CRC mismatch, sending NAK.\n");
#endif
      sfpSendNAK(ctx);
      return;
    }

    sfpTryDeliverUserFrame(ctx);
  }
}

static void sfpTryDeliverUserFrame (SFPcontext *ctx) {
  if ((ctx->rx.header & (SFP_SEQ_RANGE - 1)) == ctx->rx.seq) {
    /* Good frame received and accepted--deliver it.p */
    ctx->rx.deliver(&ctx->rx.packet, ctx->rx.deliverData);
    ctx->rx.seq = sfpNextSeq(ctx->rx.seq);
  }
  else {
#ifdef SFP_WARN
    fprintf(stderr, "(sfp) WARNING: out-of-order frame received, sending NAK.\n");
#endif
    sfpSendNAK(ctx);
  }
}

static void sfpHandleControlFrame (SFPcontext *ctx) {
  if (SFP_NAK_BIT & ctx->rx.header) {
    SFPseq seq = ctx->rx.header & (SFP_SEQ_RANGE - 1);

    if (seq == ctx->tx.seq) {
      /* The remote is telling us it expects the current sequence number, but
       * received something different. This is fine, and probably just means
       * that it received a frame that had to be retransmitted multiple
       * times. This is unlikely to even happen on a USB line, since the
       * bandwidth-delay product is so low. */
#ifdef SFP_DEBUG
      fprintf(stderr, "(sfp) DEBUG(%s): received NAK<%d> for current SEQ. Ignoring.\n",
          ctx->debugName, seq);
#endif
    }
    else {
#ifdef SFP_WARN
      fprintf(stderr, "(sfp) WARNING: current SEQ<%d>, remote host NAK'ed SEQ<%d>.\n",
          ctx->tx.seq, seq);
#endif
      sfpHandleNAK(ctx, seq);
    }
  }
  else {
#ifdef SFP_WARN
    fprintf(stderr, "(sfp) WARNING: unknown or corrupt control frame received, ignoring: 0x%x.\n",
        ctx->rx.header);
#endif
  }
}

static void sfpSendNAK (SFPcontext *ctx) {
  /* XXX The receiver must lock the transmitter before sending anything! */

  if (sfpIsTransmitterLockable(ctx)) {
    ctx->tx.lock(ctx->tx.lockData);
  }

  sfpWriteControlFrame(ctx, ctx->rx.seq | SFP_NAK_BIT);

  if (sfpIsTransmitterLockable(ctx)) {
    ctx->tx.unlock(ctx->tx.unlockData);
  }
}

static void sfpHandleNAK (SFPcontext *ctx, SFPseq seq) {

  /* XXX The receiver must lock the transmitter before sending anything! */

  if (sfpIsTransmitterLockable(ctx)) {
    ctx->tx.lock(ctx->tx.lockData);
  }

  /* The number of frames we'll have to drop from our history ring buffer in
   * order to fast-forward to the remote's current sequence number. */
  unsigned fastforward = seq
    - (ctx->tx.seq - RINGBUF_SIZE(ctx->tx.history));

  fastforward &= (SFP_SEQ_RANGE - 1);

#ifdef SFP_DEBUG
  fprintf(stderr, "(sfp) DEBUG(%s): received NAK<%d> (current SEQ<%d>). History size<%d>, fastforward<%d>.\n",
      ctx->debugName, seq, ctx->tx.seq, RINGBUF_SIZE(ctx->tx.history), fastforward);
  fprintf(stderr, "(sfp) DEBUG(%s): r' - (r - s) == %d - (%d - %d) == %d\n",
      ctx->debugName, seq, ctx->tx.seq, RINGBUF_SIZE(ctx->tx.history),
      fastforward);
#endif


  if (RINGBUF_SIZE(ctx->tx.history) > fastforward) {
    for (unsigned i = 0; i < fastforward; ++i) {
      RINGBUF_POP_FRONT(ctx->tx.history);
    }
  }
  else {
    fprintf(stderr, "(sfp) ERROR: %d outgoing frame(s) lost by history buffer underrun.\n"
        "\tTry adjusting SFP_CONFIG_HISTORY_CAPACITY.\n", SFP_SEQ_RANGE - fastforward);

    /* Even if we lost frames, the show still has to go on. Resynchronize, and
     * send what frames we have available in our history. */
  }

  /* Synchronize our remote sequence number with the NAK. */
  ctx->tx.seq = seq;

  size_t reTxCount = RINGBUF_SIZE(ctx->tx.history);

  for (size_t i = 0; i < reTxCount; ++i) {
#ifdef SFP_DEBUG
    fprintf(stderr, "(sfp) DEBUG(%s): retransmitting frame with SEQ<%d>\n",
        ctx->debugName, ctx->tx.seq);
#endif
    sfpWriteUserFrame(ctx, &RINGBUF_AT(ctx->tx.history, i));
  }

  if (sfpIsTransmitterLockable(ctx)) {
    ctx->tx.unlock(ctx->tx.unlockData);
  }
}

static void sfpBufferOctet (SFPcontext *ctx, uint8_t octet) {
  if (SFP_CONFIG_MAX_PACKET_SIZE <= ctx->rx.packet.len) {
    fprintf(stderr, "(sfp) ERROR: incoming frame(s) lost by frame buffer overrun.\n"
        "\tTry increasing SFP_CONFIG_MAX_PACKET_SIZE.\n"
        "\tThis could also be caused by a corrupt FLAG octet.\n");

    /* Until I have a better idea, just going to pretend we didn't receive
     * anything at all, and just go on with life. If this was caused by a
     * corrupt FLAG octet, then our forthcoming NAK should resynchronize
     * everything. */
    sfpResetReceiver(ctx);
  }
  else {
    /* Finally, the magic happens. */
    ctx->rx.packet.buf[ctx->rx.packet.len++] = octet;
  }
}

static int sfpIsTransmitterLockable (SFPcontext *ctx) {
  return ctx->tx.lock && ctx->tx.unlock;
}

static int sfpIsReservedOctet (uint8_t octet) {
  switch (octet) {
    case SFP_ESC:
      /* fall-through */
    case SFP_FLAG:
      return 1;
    default:
      return 0;
  }
}

/* Wrapper around ctx->write1, updating the rolling CRC and escaping
 * reserved octets as necessary. */
static void sfpWrite (SFPcontext *ctx, uint8_t octet) {
  ctx->tx.crc = _crc_ccitt_update(ctx->tx.crc, octet);

#if 0
#ifdef SFP_DEBUG
  fprintf(stderr, "(sfp) DEBUG(%s): writing data octet<0x%02x> CRC<0x%04x>\n", ctx->debugName,
      octet, ctx->tx.crc);
#endif
#endif

  sfpWriteNoCRC(ctx, octet);
}

static void sfpWriteNoCRC (SFPcontext *ctx, uint8_t octet) {
  if (sfpIsReservedOctet(octet)) {
    octet ^= SFP_ESC_FLIP_BIT;
    ctx->tx.write1(SFP_ESC, ctx->tx.write1Data);
  }
  ctx->tx.write1(octet, ctx->tx.write1Data);
}

static void sfpWriteUserFrame (SFPcontext *ctx, SFPpacket *packet) {
  sfpWriteFrameWithSeq(ctx, ctx->tx.seq, packet);
  ctx->tx.seq = sfpNextSeq(ctx->tx.seq);
}

static void sfpWriteControlFrame (SFPcontext *ctx, SFPheader header) {
  sfpWriteFrameWithSeq(ctx, header, NULL);
}

/* Provided separately from sfpWriteUserFrame so that the receiver can
 * use it to send NAKs. */
static void sfpWriteFrameWithSeq (SFPcontext *ctx, SFPseq seq, SFPpacket *packet) {
  ctx->tx.crc = SFP_CRC_PRESET;

  /* Begin frame. */
  ctx->tx.write1(SFP_FLAG, ctx->tx.write1Data);

  sfpWrite(ctx, seq);

  if (packet) {
    for (size_t i = 0; i < packet->len; ++i) {
      sfpWrite(ctx, packet->buf[i]);
    }
  }

  SFPcrc crc = sfpByteSwapCRC(ctx->tx.crc);
  uint8_t *pcrc = (uint8_t *)&crc;

  for (size_t i = 0; i < sizeof(crc); ++i) {
    /* At first glance, this might seem bizarre. The "NoCRC" bit simply means
     * that the transmitter's rolling CRC will not be updated by the octet we
     * pass. We don't need to CRC the CRC itself. */
    sfpWriteNoCRC(ctx, pcrc[i]);
  }

  /* End frame. */
  ctx->tx.write1(SFP_FLAG, ctx->tx.write1Data);

  sfpFlushWriteBuffer(ctx);
}

static void sfpFlushWriteBuffer (SFPcontext *ctx) {
  if (ctx->tx.writen) {
    ctx->tx.writen(ctx->tx.writebuf, ctx->tx.writebufn, ctx->tx.writenData);
    ctx->tx.writebufn = 0;
  }
  else {
    assert(!ctx->tx.writebufn);
  }
}

static void sfpBufferedWrite (uint8_t octet, void *data) {
  SFPcontext *ctx = (SFPcontext *)data;

  /* If we're in this function, that means we're using SFP_WRITE_MULTIPLE,
   * so the writen function had better exist. */
  assert(ctx->tx.writen);

  if (ctx->tx.writebufn >= SFP_CONFIG_WRITEBUF_SIZE) {
    sfpFlushWriteBuffer(ctx);
  }

  ctx->tx.writebuf[ctx->tx.writebufn++] = octet;
}