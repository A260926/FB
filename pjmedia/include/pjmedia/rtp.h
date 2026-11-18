/* 
 * Copyright (C) 2008-2026 Teluu Inc. (http://www.teluu.com)
 * Copyright (C) 2003-2008 Benny Prijono <benny@prijono.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA 
 */
#ifndef __PJMEDIA_RTP_H__
#define __PJMEDIA_RTP_H__


/**
 * @file rtp.h
 * @brief RTP packet and RTP session declarations.
 */
#include <pjmedia/types.h>
#include <pj/assert.h>


PJ_BEGIN_DECL


/**
 * @defgroup PJMED_RTP RTP Session and Encapsulation (RFC 3550)
 * @ingroup PJMEDIA_SESSION
 * @brief RTP format and session management
 * @{
 *
 * The RTP module is designed to be dependent only to PJLIB, it does not depend
 * on any other parts of PJMEDIA library. The RTP module does not even depend
 * on any transports (sockets), to promote even more use, such as in DSP
 * development (where transport may be handled by different processor).
 *
 * An RTCP implementation is available, in separate module. Please see 
 * @ref PJMED_RTCP.
 *
 * The functions that are provided by this module:
 *  - creating RTP header for each outgoing packet.
 *  - decoding RTP packet into RTP header and payload.
 *  - provide simple RTP session management (sequence number, etc.)
 *
 * The RTP module does not use any dynamic memory at all.
 *
 * \section P1 How to Use the RTP Module
 * 
 * First application must call #pjmedia_rtp_session_init() to initialize the RTP 
 * session.
 *
 * When application wants to send RTP packet, it needs to call 
 * #pjmedia_rtp_encode_rtp() to build the RTP header. Note that this WILL NOT
 * build the complete RTP packet, but instead only the header. Application can
 * then either concatenate the header with the payload, or send the two
 * fragments (the header and the payload) using scatter-gather transport API
 * (e.g. \a sendv()).
 *
 * When application receives an RTP packet, first it should call
 * #pjmedia_rtp_decode_rtp to decode RTP header and payload, then it should call
 * #pjmedia_rtp_session_update to check whether we can process the RTP payload,
 * and to let the RTP session updates its internal status. The decode function
 * is guaranteed to point the payload to the correct position regardless of
 * any options present in the RTP packet.
 *
 */

#ifdef _MSC_VER
#   pragma warning(disable:4214)    // bit field types other than int
#endif


/**
 * RTP packet header. Note that all RTP functions here will work with this
 * header in network byte order.
 */
#pragma pack(1)
struct pjmedia_rtp_hdr
{
#if defined(PJ_IS_BIG_ENDIAN) && (PJ_IS_BIG_ENDIAN!=0)
    pj_uint16_t v:2;            /**< packet type/version            */
    pj_uint16_t p:1;            /**< padding flag                   */
    pj_uint16_t x:1;            /**< extension flag                 */
    pj_uint16_t cc:4;           /**< CSRC count                     */
    pj_uint16_t m:1;            /**< marker bit                     */
    pj_uint16_t pt:7;           /**< payload type                   */
#else
    pj_uint16_t cc:4;           /**< CSRC count                     */
    pj_uint16_t x:1;            /**< header extension flag          */ 
    pj_uint16_t p:1;            /**< padding flag                   */
    pj_uint16_t v:2;            /**< packet type/version            */
    pj_uint16_t pt:7;           /**< payload type                   */
    pj_uint16_t m:1;            /**< marker bit                     */
#endif
    pj_uint16_t seq;            /**< sequence number                */
    pj_uint32_t ts;             /**< timestamp                      */
    pj_uint32_t ssrc;           /**< synchronization source         */
};
#pragma pack()

/**
 * Following the RTP header are a number of additional headers, which
 * can be used for purposes such as redundancy.
 * This structure contains the first 8-bit of an additional header.
 */
#pragma pack(1)
struct pjmedia_rtp_add_hdr_short
{
#if defined(PJ_IS_BIG_ENDIAN) && (PJ_IS_BIG_ENDIAN!=0)
    pj_uint8_t f:1;            /**< F bit (0 means last, 1 has more) */
    pj_uint8_t pt:7;           /**< payload type                     */
#else
    pj_uint8_t pt:7;           /**< payload type                     */
    pj_uint8_t f:1;            /**< F bit (0 means last, 1 has more) */
#endif
};
#pragma pack()

/**
 * @see pjmedia_rtp_hdr
 */
typedef struct pjmedia_rtp_hdr pjmedia_rtp_hdr;

/* RTP additional header. */
typedef pj_uint32_t pjmedia_rtp_add_hdr;

/**
 * @see pjmedia_rtp_add_hdr_short
 */
typedef struct pjmedia_rtp_add_hdr_short pjmedia_rtp_add_hdr_short;

/**
 * RTP extension header.
 */
struct pjmedia_rtp_ext_hdr
{
    pj_uint16_t profile_data;   /**< Profile data.          */
    pj_uint16_t length;         /**< Length.                */
};

/**
 * @see pjmedia_rtp_ext_hdr
 */
typedef struct pjmedia_rtp_ext_hdr pjmedia_rtp_ext_hdr;

/**
 * This will contain the RTP header decode output.
 */
struct pjmedia_rtp_dec_hdr
{
    /* RTP extension header output decode */
    pjmedia_rtp_ext_hdr *ext_hdr;
    pj_uint32_t *ext;
    unsigned ext_len;
};

/**
 * @see pjmedia_rtp_dec_hdr
 */
typedef struct pjmedia_rtp_dec_hdr pjmedia_rtp_dec_hdr;

#pragma pack(1)

/**
 * Declaration for DTMF telephony-events (RFC2833).
 */
struct pjmedia_rtp_dtmf_event
{
    pj_uint8_t  event;      /**< Event type ID.     */
    pj_uint8_t  e_vol;      /**< Event volume.      */
    pj_uint16_t duration;   /**< Event duration.    */
};

/**
 * Mask for the E ("End") bit of telephony-events payload.
 *
 * @see pjmedia_rtp_dtmf_event
 */
#define PJMEDIA_RTP_DTMF_EVENT_END_MASK     0x80

/**
 * Mask for the Volume field of telephony-events payload.
 *
 * @see pjmedia_rtp_dtmf_event
 */
#define PJMEDIA_RTP_DTMF_EVENT_VOLUME_MASK  0x3F

/**
 * @see pjmedia_rtp_dtmf_event
 */
typedef struct pjmedia_rtp_dtmf_event pjmedia_rtp_dtmf_event;

PJ_STATIC_ASSERT(sizeof(pjmedia_rtp_dtmf_event) == 4,
                 pjmedia_rtp_dtmf_event_must_be_4_bytes);

#pragma pack()


/**
 * A generic sequence number management, used by both RTP and RTCP.
 */
struct pjmedia_rtp_seq_session
{
    pj_uint16_t     max_seq;        /**< Highest sequence number heard      */
    pj_uint32_t     cycles;         /**< Shifted count of seq number cycles */
    pj_uint32_t     base_seq;       /**< Base seq number                    */
    pj_uint32_t     bad_seq;        /**< Last 'bad' seq number + 1          */
    pj_uint32_t     probation;      /**< Sequ. packets till source is valid */
};

/**
 * @see pjmedia_rtp_seq_session
 */
typedef struct pjmedia_rtp_seq_session pjmedia_rtp_seq_session;


/**
 * RTP session descriptor.
 */
struct pjmedia_rtp_session
{
    pjmedia_rtp_hdr         out_hdr;    /**< Saved hdr for outgoing pkts.   */
    pjmedia_rtp_seq_session seq_ctrl;   /**< Sequence number management.    */
    pj_uint16_t             out_pt;     /**< Default outgoing payload type. */
    pj_uint32_t             out_extseq; /**< Outgoing extended seq #.       */
    pj_bool_t               has_peer_ssrc;/**< Has peer SSRC?               */
    pj_uint32_t             peer_ssrc;  /**< Peer SSRC.                     */
    pj_uint32_t             received;   /**< Number of received packets.    */
};

/**
 * @see pjmedia_rtp_session
 */
typedef struct pjmedia_rtp_session pjmedia_rtp_session;


/**
 * This structure is used to receive additional information about the
 * state of incoming RTP packet.
 */
struct pjmedia_rtp_status
{
    union {
        struct flag {
            int bad:1;      /**< General flag to indicate that sequence is
                                 bad, and application should not process
                                 this packet. More information will be given
                                 in other flags.                            */
            int badpt:1;    /**< Bad payload type.                          */
            int badssrc:1;  /**< Bad SSRC                                   */
            int dup:1;      /**< Indicates duplicate packet                 */
            int outorder:1; /**< Indicates out of order packet              */
            int probation:1;/**< Indicates that session is in probation
                                 until more packets are received.           */
            int restart:1;  /**< Indicates that sequence number has made
                                 a large jump, and internal base sequence
                                 number has been adjusted.                  */
        } flag;             /**< Status flags.                              */

        pj_uint16_t value;  /**< Status value, to conveniently address all
                                 flags.                                     */

    } status;               /**< Status information union.                  */

    pj_uint16_t diff;       /**< Sequence number difference from previous
                                 packet. Normally the value should be 1.    
                                 Value greater than one may indicate packet
                                 loss. If packet with lower sequence is
                                 received, the value will be set to zero.
                                 If base sequence has been restarted, the
                                 value will be one.                         */
};


/**
 * RTP session settings.
 */
typedef struct pjmedia_rtp_session_setting
{
    pj_uint8_t       flags;         /**< Bitmask flags to specify whether such
                                         field is set. Bitmask contents are:
                                         (bit #0 is LSB)
                                         bit #0: default payload type
                                         bit #1: sender SSRC
                                         bit #2: sequence
                                         bit #3: timestamp
                                         bit #4: peer SSRC                  */
    int              default_pt;    /**< Default payload type.              */
    pj_uint32_t      sender_ssrc;   /**< Sender SSRC.                       */
    pj_uint32_t      peer_ssrc;     /**< Peer SSRC.                         */
    pj_uint16_t      seq;           /**< Sequence.                          */
    pj_uint32_t      ts;            /**< Timestamp.                         */
} pjmedia_rtp_session_setting;


/**
 * @see pjmedia_rtp_status
 */
typedef struct pjmedia_rtp_status pjmedia_rtp_status;


/**
 * This function will initialize the RTP session according to given parameters.
 *
 * @param ses           The session.
 * @param default_pt    Default payload type.
 * @param sender_ssrc   SSRC used for outgoing packets, in host byte order.
 *
 * @return              PJ_SUCCESS if successfull.
 */
PJ_DECL(pj_status_t) pjmedia_rtp_session_init( pjmedia_rtp_session *ses,
                                               int default_pt, 
                                               pj_uint32_t sender_ssrc );

/**
 * This function will initialize the RTP session according to given parameters
 * defined in RTP session settings.
 *
 * @param ses           The session.
 * @param settings      RTP session settings.
 *
 * @return              PJ_SUCCESS if successfull.
 */
PJ_DECL(pj_status_t) pjmedia_rtp_session_init2( 
                                    pjmedia_rtp_session *ses,
                                    pjmedia_rtp_session_setting settings);


/**
 * Create the RTP header based on arguments and current state of the RTP
 * session.
 *
 * @param ses           The session.
 * @param pt            Payload type.
 * @param m             Marker flag.
 * @param payload_len   Payload length in bytes.
 * @param ts_len        Timestamp length.
 * @param rtphdr        Upon return will point to RTP packet header.
 * @param hdrlen        Upon return will indicate the size of RTP packet header
 *
 * @return              PJ_SUCCESS if successfull.
 */
PJ_DECL(pj_status_t) pjmedia_rtp_encode_rtp( pjmedia_rtp_session *ses, 
                                             int pt, int m,
                                             int payload_len, int ts_len,
                                             const void **rtphdr, 
                                             int *hdrlen );

/**
 * This function decodes incoming packet into RTP header and payload.
 * The decode function is guaranteed to point the payload to the correct 
 * position regardless of any options present in the RTP packet.
 *
 * Note that this function does not modify the returned RTP header to
 * host byte order.
 *
 * @param ses           The session.
 * @param pkt           The received RTP packet.
 * @param pkt_len       The length of the packet.
 * @param hdr           Upon return will point to the location of the RTP 
 *                      header inside the packet. Note that the RTP header
 *                      will be given back as is, meaning that the fields
 *                      will be in network byte order.
 * @param payload       Upon return will point to the location of the
 *                      payload inside the packet.
 * @param payloadlen    Upon return will indicate the size of the payload.
 *
 * @return              PJ_SUCCESS if successfull.
 */
PJ_DECL(pj_status_t) pjmedia_rtp_decode_rtp( pjmedia_rtp_session *ses, 
                                             const void *pkt, int pkt_len,
                                             const pjmedia_rtp_hdr **hdr,
                                             const void **payload,
                                             unsigned *payloadlen);


/**
 * This function decodes incoming packet into RTP header and payload.
 * The decode function is guaranteed to point the payload to the correct
 * position regardless of any options present in the RTP packet.
 *
 * Note that this function does not modify the returned RTP header to
 * host byte order.
 *
 * @param ses           The session.
 * @param pkt           The received RTP packet.
 * @param pkt_len       The length of the packet.
 * @param hdr           Upon return will point to the location of the RTP
 *                      header inside the packet. Note that the RTP header
 *                      will be given back as is, meaning that the fields
 *                      will be in network byte order.
 * @param dec_hdr       Upon return will point to the location of the 
 *                      additional RTP header inside the packet, if any.
 * @param payload       Upon return will point to the location of the
 *                      payload inside the packet.
 * @param payloadlen    Upon return will indicate the size of the payload.
 *
 * @return              PJ_SUCCESS if successfull.
 */
PJ_DECL(pj_status_t) pjmedia_rtp_decode_rtp2(
                                    pjmedia_rtp_session *ses,
                                    const void *pkt, int pkt_len,
                                    const pjmedia_rtp_hdr **hdr,
                                    pjmedia_rtp_dec_hdr *dec_hdr,
                                    const void **payload,
                                    unsigned *payloadlen);

/**
 * Call this function everytime an RTP packet is received to check whether 
 * the packet can be received and to let the RTP session performs its internal
 * calculations.
 *
 * @param ses       The session.
 * @param hdr       The RTP header of the incoming packet. The header must
 *                  be given with fields in network byte order.
 * @param seq_st    Optional structure to receive the status of the RTP packet
 *                  processing.
 */
PJ_DECL(void) pjmedia_rtp_session_update( pjmedia_rtp_session *ses, 
                                          const pjmedia_rtp_hdr *hdr,
                                          pjmedia_rtp_status *seq_st);


/**
 * Call this function everytime an RTP packet is received to check whether 
 * the packet can be received and to let the RTP session performs its internal
 * calculations.
 *
 * @param ses       The session.
 * @param hdr       The RTP header of the incoming packet. The header must
 *                  be given with fields in network byte order.
 * @param seq_st    Optional structure to receive the status of the RTP packet
 *                  processing.
 * @param check_pt  Flag to indicate whether payload type needs to be validate.
 *
 * @see pjmedia_rtp_session_update()
 */
PJ_DECL(void) pjmedia_rtp_session_update2(pjmedia_rtp_session *ses, 
                                          const pjmedia_rtp_hdr *hdr,
                                          pjmedia_rtp_status *seq_st,
                                          pj_bool_t check_pt);


/**
 * @defgroup PJMED_RTP_SPLICE RTP Source Splice
 * @ingroup PJMED_RTP
 * @brief Timed continuation between two RTP sources.
 * @{
 *
 * The splice object accepts complete RTP packets that are already in
 * memory and a caller-driven microsecond clock.  It does not use sockets
 * or threads.  Packets from a current source are queued in logical slots;
 * another source can be armed as a candidate and atomically takes over at
 * a caller selected slot.
 *
 * All functions that return an int return a POSIX errno value: zero on
 * success, and one of EINVAL, ENOENT, ENOBUFS, ETIMEDOUT, EBADMSG, EAGAIN,
 * ENODATA or ENOMEM on failure.
 */

/**
 * Splice configuration.
 */
typedef struct pjmedia_rtp_splice_cfg
{
    pj_uint32_t init_ssrc;   /**< SSRC of the initial media source.        */
    pj_uint32_t out_ssrc;    /**< SSRC written to output packets.          */
    pj_uint32_t pt;          /**< Payload type, 0..127.                    */
    pj_uint32_t clock_rate;  /**< Clock rate in Hz, greater than zero.     */
    pj_uint32_t frame_ticks; /**< Timestamp ticks per packet, > 0.         */
    pj_int64_t  delay_us;    /**< Playout delay in microseconds, >= 0.     */
    pj_uint32_t window;      /**< Slot window, 4..512.                     */
    pj_uint16_t out_seq;     /**< Sequence number of output slot zero.     */
    pj_uint32_t out_ts;      /**< Timestamp of output slot zero.           */
} pjmedia_rtp_splice_cfg;

/**
 * Opaque splice object.
 */
typedef struct pjmedia_rtp_splice pjmedia_rtp_splice;

/**
 * Allocate a splice object.
 *
 * @param obj   Upon return, points to the new splice object.
 * @param cfg   Splice configuration.
 *
 * @return      Zero on success, EINVAL for an invalid configuration, or
 *              ENOMEM when allocation fails.
 */
PJ_DECL(int) pjmedia_rtp_splice_alloc(pjmedia_rtp_splice **obj,
                                      const pjmedia_rtp_splice_cfg *cfg);

/**
 * Destroy a splice object and release every packet it still holds.
 *
 * @param obj   The splice object.
 */
PJ_DECL(void) pjmedia_rtp_splice_destroy(pjmedia_rtp_splice *obj);

/**
 * Submit a complete RTP packet.  The packet bytes are copied, so the caller
 * may reuse or free its buffer immediately.  The first packet accepted for a
 * given source and slot wins; later packets for the same slot are ignored.
 *
 * The first valid packet of the initial source establishes slot zero using
 * its sequence number, timestamp and arrival time.
 *
 * @param obj   The splice object.
 * @param pkt   Complete RTP packet.
 * @param len   Packet length in bytes.
 * @param now   Current time in microseconds, non-negative and monotonic.
 *
 * @return      Zero on success, ENOENT when the slot is too old or the SSRC
 *              is unknown, ENOBUFS when the slot is beyond the window,
 *              ETIMEDOUT when the slot deadline has passed, EBADMSG for an
 *              invalid packet, or EINVAL for invalid arguments.
 */
PJ_DECL(int) pjmedia_rtp_splice_push(pjmedia_rtp_splice *obj,
                                     const void *pkt, pj_size_t len,
                                     pj_uint64_t now);

/**
 * Pop the packet for the current head slot once its deadline has arrived.
 *
 * Before a stream starts, or before the head slot deadline, EAGAIN is
 * returned and nothing is advanced.  Otherwise exactly one slot is
 * consumed: zero is returned when a packet was available and ENODATA when
 * the slot was empty.  Both outcomes advance the head slot.
 *
 * On success, *pkt points to an independently allocated buffer owned by the
 * caller, who must free() it, and *len receives its size.  The buffer keeps
 * the complete original packet and only the SSRC, sequence number and
 * timestamp are rewritten with the configured output identity.
 *
 * @param obj       The splice object.
 * @param now       Current time in microseconds.
 * @param pkt       Upon success, receives the output packet buffer.
 * @param len       Upon success, receives the output packet length.
 *
 * @return          Zero on success, EAGAIN when not due yet, ENODATA when a
 *                  due slot was empty, or EINVAL for invalid arguments.
 */
PJ_DECL(int) pjmedia_rtp_splice_pop(pjmedia_rtp_splice *obj, pj_uint64_t now,
                                    void **pkt, pj_size_t *len);

/**
 * Arm a candidate source that may take over at logical slot @a cut.
 *
 * The object must already have an anchor, @a cut must be at least the head
 * slot, cut + 2 must be inside the current window, and @a ssrc must differ
 * from the current source.  The first valid candidate packet maps to
 * @a cut and establishes the candidate's sequence/timestamp coordinates.
 * Slots cut, cut + 1 and cut + 2 must all be received for the candidate to
 * be ready.
 *
 * Arming replaces a previous candidate.  A failed call leaves the previous
 * candidate untouched.
 *
 * @param obj   The splice object.
 * @param ssrc  Candidate SSRC.
 * @param cut   Logical slot where the candidate may take over.
 *
 * @return      Zero on success or EINVAL for invalid arguments.
 */
PJ_DECL(int) pjmedia_rtp_splice_arm(pjmedia_rtp_splice *obj,
                                    pj_uint32_t ssrc, pj_uint64_t cut);

/**
 * Cancel the current candidate without disturbing packets already queued
 * for the current source.
 *
 * @param obj   The splice object.
 */
PJ_DECL(void) pjmedia_rtp_splice_cancel(pjmedia_rtp_splice *obj);

/**
 * Reset the splice object: release all queued packets and any candidate,
 * clear the clock and head slot, and restore the configured initial source
 * and output origin.
 *
 * @param obj   The splice object.
 */
PJ_DECL(void) pjmedia_rtp_splice_reset(pjmedia_rtp_splice *obj);

/**
 * Get the current head slot number.
 *
 * @param obj   The splice object.
 *
 * @return      The head slot, or zero when the object is NULL.
 */
PJ_DECL(pj_uint64_t) pjmedia_rtp_splice_head(const pjmedia_rtp_splice *obj);

/**
 * Get the SSRC of the currently active source.
 *
 * @param obj   The splice object.
 *
 * @return      The active SSRC; before the first packet this is the
 *              configured initial SSRC.
 */
PJ_DECL(pj_uint32_t) pjmedia_rtp_splice_active(
                                        const pjmedia_rtp_splice *obj);

/**
 * @}
 */

/*
 * INTERNAL:
 */

/** 
 * Internal function for creating sequence number control, shared by RTCP 
 * implementation. 
 *
 * @param seq_ctrl  The sequence control instance.
 * @param seq       Sequence number to initialize.
 */
void pjmedia_rtp_seq_init(pjmedia_rtp_seq_session *seq_ctrl, 
                          pj_uint16_t seq);


/** 
 * Internal function update sequence control, shared by RTCP implementation.
 *
 * @param seq_ctrl      The sequence control instance.
 * @param seq           Sequence number to update.
 * @param seq_status    Optional structure to receive additional information 
 *                      about the packet.
 */
void pjmedia_rtp_seq_update( pjmedia_rtp_seq_session *seq_ctrl, 
                             pj_uint16_t seq,
                             pjmedia_rtp_status *seq_status);

/**
 * @}
 */

PJ_END_DECL


#endif  /* __PJMEDIA_RTP_H__ */
