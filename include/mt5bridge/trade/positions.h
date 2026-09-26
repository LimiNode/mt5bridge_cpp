#pragma once

/// \file trade/positions.h
/// \brief Declares active-position observation contracts.

#include <mt5bridge/trade/common.h>

/// \name Position snapshot known-field masks
/// \brief Bits distinguish an absent MT5 field from a valid zero value.
/// \{
#define MT5BRIDGE_POSITION_KNOWN_TICKET (UINT64_C(1) << 0)
#define MT5BRIDGE_POSITION_KNOWN_IDENTIFIER (UINT64_C(1) << 1)
#define MT5BRIDGE_POSITION_KNOWN_MAGIC (UINT64_C(1) << 2)
#define MT5BRIDGE_POSITION_KNOWN_TYPE (UINT64_C(1) << 3)
#define MT5BRIDGE_POSITION_KNOWN_REASON (UINT64_C(1) << 4)
#define MT5BRIDGE_POSITION_KNOWN_VOLUME (UINT64_C(1) << 5)
#define MT5BRIDGE_POSITION_KNOWN_PRICE_OPEN (UINT64_C(1) << 6)
#define MT5BRIDGE_POSITION_KNOWN_PRICE_CURRENT (UINT64_C(1) << 7)
#define MT5BRIDGE_POSITION_KNOWN_SL (UINT64_C(1) << 8)
#define MT5BRIDGE_POSITION_KNOWN_TP (UINT64_C(1) << 9)
#define MT5BRIDGE_POSITION_KNOWN_PROFIT (UINT64_C(1) << 10)
#define MT5BRIDGE_POSITION_KNOWN_SWAP (UINT64_C(1) << 11)
#define MT5BRIDGE_POSITION_KNOWN_TIME (UINT64_C(1) << 12)
#define MT5BRIDGE_POSITION_KNOWN_TIME_UPDATE (UINT64_C(1) << 13)
#define MT5BRIDGE_POSITION_KNOWN_SYMBOL (UINT64_C(1) << 14)
#define MT5BRIDGE_POSITION_KNOWN_COMMENT (UINT64_C(1) << 15)
#define MT5BRIDGE_POSITION_KNOWN_EXTERNAL_ID (UINT64_C(1) << 16)
/// \}

#ifdef __cplusplus
extern "C" {
#endif

/// \struct Mt5PositionsRequest
/// \brief Filters an active-position snapshot.
typedef struct Mt5PositionsRequest {
    const char *symbol_utf8; ///< Optional symbol selector; exclusive with group/ticket.
    const char *group_utf8;  ///< Optional MT5 group selector; exclusive with symbol/ticket.
    uint64_t ticket;         ///< Optional position ticket selector; exclusive with symbol/group.
    uint64_t identifier;     ///< Optional POSITION_IDENTIFIER, applied as a local post-filter.
    uint32_t reserved;       ///< Reserved; must be zero.
} Mt5PositionsRequest;

/// \struct Mt5PositionSnapshot
/// \brief Lossless typed evidence copied from an MT5 position record.
typedef struct Mt5PositionSnapshot {
    uint64_t ticket;             ///< POSITION_TICKET.
    uint64_t identifier;         ///< POSITION_IDENTIFIER.
    uint64_t magic;              ///< POSITION_MAGIC.
    uint32_t type;               ///< POSITION_TYPE_*.
    uint32_t reason;             ///< POSITION_REASON_*.
    double volume;               ///< POSITION_VOLUME.
    double price_open;           ///< POSITION_PRICE_OPEN.
    double price_current;        ///< POSITION_PRICE_CURRENT.
    double sl;                   ///< POSITION_SL.
    double tp;                   ///< POSITION_TP.
    double profit;               ///< POSITION_PROFIT.
    double swap;                 ///< POSITION_SWAP.
    int64_t time_msc;            ///< POSITION_TIME converted to milliseconds.
    int64_t time_update_msc;     ///< POSITION_TIME_UPDATE converted to milliseconds.
    char symbol[64];             ///< UTF-8 symbol.
    char comment[MT5BRIDGE_TEXT_CAPACITY]; ///< UTF-8 comment.
    char external_id[MT5BRIDGE_TEXT_CAPACITY]; ///< Broker/exchange external id.
    uint64_t known_fields;       ///< Bits identifying fields present in MT5.
    uint32_t reserved[2];        ///< Reserved; must be zero.
} Mt5PositionSnapshot;

/// \struct Mt5PositionBuffer
/// \brief Opaque DLL-owned active-position collection.
typedef struct Mt5PositionBuffer Mt5PositionBuffer;

/// \brief Returns an active-position snapshot.
MT5BRIDGE_EXPORT int mt5bridge_query_positions(const Mt5PositionsRequest *request,
                                               Mt5PositionBuffer **result);
/// \brief Returns the immutable data stored in a position buffer.
MT5BRIDGE_EXPORT const Mt5PositionSnapshot *mt5bridge_position_buffer_data(
    const Mt5PositionBuffer *buffer);
/// \brief Returns the number of positions stored in a buffer.
MT5BRIDGE_EXPORT size_t mt5bridge_position_buffer_size(const Mt5PositionBuffer *buffer);
/// \brief Releases a position buffer allocated by mt5_bridge.dll.
MT5BRIDGE_EXPORT void mt5bridge_position_buffer_free(Mt5PositionBuffer *buffer);

#if defined(__cplusplus)
static_assert(sizeof(Mt5PositionsRequest) == 40, "Mt5PositionsRequest ABI size changed");
static_assert(sizeof(Mt5PositionSnapshot) == 440, "Mt5PositionSnapshot ABI size changed");
static_assert(offsetof(Mt5PositionSnapshot, identifier) == 8,
              "Mt5PositionSnapshot identifier offset changed");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(Mt5PositionsRequest) == 40, "Mt5PositionsRequest ABI size changed");
_Static_assert(sizeof(Mt5PositionSnapshot) == 440, "Mt5PositionSnapshot ABI size changed");
#endif

#ifdef __cplusplus
}
#endif
