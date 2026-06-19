/* ============================================================================
 * pgforge — wire protocol
 *
 * Binary framing over WebSocket (8-byte header + payload), big-endian:
 *   [opcode:1][flags:1][message_id:2][payload_length:4][payload:N]
 *
 * The opcode set is deliberately small and GENERIC: a handful of system ops
 * plus the schema-driven data-layer ops (DB_*). The table is named in the JSON
 * payload, not encoded as a distinct opcode — that is the core difference from
 * a per-entity backend.
 * ============================================================================ */
#ifndef PGF_PROTOCOL_H
#define PGF_PROTOCOL_H

#include <stdint.h>

#define PGF_HEADER_SIZE      8
#define PGF_MAX_PAYLOAD      (1024 * 1024)   /* 1MB */

/* Flag bits (high bits of the flags byte) */
#define PGF_FLAG_RESPONSE    0x80   /* message is a response to a request */
#define PGF_FLAG_ERROR       0x40   /* response carries an error */

/* 8-byte header followed by a variable payload. Multi-byte fields big-endian. */
typedef struct {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t message_id;       /* network byte order on the wire */
    uint32_t payload_length;   /* network byte order on the wire */
    uint8_t  payload[];
} __attribute__((packed)) pgf_message_t;

/* ---- System / connectivity (0x01-0x0F) — no auth ---- */
#define OP_PING              0x01   /* -> PONG */
#define OP_ECHO              0x02   /* -> same payload back */
#define OP_SERVER_INFO       0x03   /* -> JSON {name, version} */

/* ---- Auth & sessions (0x10-0x1F) ---- */
#define OP_LOGIN             0x10
#define OP_LOGOUT            0x11
#define OP_VERIFY_SESSION    0x12

/* ---- Realtime pub/sub (0x20-0x2F) ---- */
#define OP_SUBSCRIBE         0x20   /* { token, table, key?:{column,value} } */
#define OP_UNSUBSCRIBE       0x21   /* { token, table } */
#define OP_CHANGE            0x22   /* server push: { table, op, row } */

/* ---- Generic schema-driven data layer (0xD0-0xD7) ----
 * Payload is JSON; "table" names the target, resolved via the schema catalog. */
#define OP_DB_LIST           0xD0   /* { table, select, where, order, limit, offset } */
#define OP_DB_GET            0xD1   /* { table, id } */
#define OP_DB_CREATE         0xD2   /* { table, values } */
#define OP_DB_UPDATE         0xD3   /* { table, id, values } */
#define OP_DB_DELETE         0xD4   /* { table, id } */
#define OP_DB_QUERY          0xD5   /* richer read: LIST + { embed:[...], count:"exact" } */
#define OP_DB_RPC            0xD6   /* { fn, args } */
#define OP_DB_SCHEMA         0xD7   /* -> catalog JSON for the admin UI */

#endif /* PGF_PROTOCOL_H */
