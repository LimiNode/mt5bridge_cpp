#pragma once

/// \file trade/deals.h
/// \brief Declares historical-deal observation contracts.

#include <mt5bridge/trade/common.h>

/// \name Deal snapshot known-field masks
/// \brief Bits distinguish an absent MT5 field from a valid zero value.
/// \{
#define MT5BRIDGE_DEAL_KNOWN_TICKET (UINT64_C(1) << 0)
#define MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET (UINT64_C(1) << 1)
#define MT5BRIDGE_DEAL_KNOWN_POSITION_ID (UINT64_C(1) << 2)
#define MT5BRIDGE_DEAL_KNOWN_MAGIC (UINT64_C(1) << 3)
#define MT5BRIDGE_DEAL_KNOWN_TYPE (UINT64_C(1) << 4)
#define MT5BRIDGE_DEAL_KNOWN_ENTRY (UINT64_C(1) << 5)
#define MT5BRIDGE_DEAL_KNOWN_REASON (UINT64_C(1) << 6)
#define MT5BRIDGE_DEAL_KNOWN_VOLUME (UINT64_C(1) << 7)
#define MT5BRIDGE_DEAL_KNOWN_PRICE (UINT64_C(1) << 8)
#define MT5BRIDGE_DEAL_KNOWN_PROFIT (UINT64_C(1) << 9)
#define MT5BRIDGE_DEAL_KNOWN_COMMISSION (UINT64_C(1) << 10)
#define MT5BRIDGE_DEAL_KNOWN_SWAP (UINT64_C(1) << 11)
#define MT5BRIDGE_DEAL_KNOWN_FEE (UINT64_C(1) << 12)
#define MT5BRIDGE_DEAL_KNOWN_TIME (UINT64_C(1) << 13)
#define MT5BRIDGE_DEAL_KNOWN_SYMBOL (UINT64_C(1) << 14)
#define MT5BRIDGE_DEAL_KNOWN_COMMENT (UINT64_C(1) << 15)
#define MT5BRIDGE_DEAL_KNOWN_EXTERNAL_ID (UINT64_C(1) << 16)
/// \}

#ifdef __cplusplus
extern "C" {
#endif

/// \struct Mt5HistoryDealsRequest
/// \brief Bounds history deals by time and local deal/order/position filters.
typedef struct Mt5HistoryDealsRequest {
    int64_t from_msc;        ///< Inclusive UTC range start in milliseconds.
    int64_t to_msc;          ///< Inclusive UTC range end in milliseconds.
    const char *group_utf8;  ///< Optional MT5 group selector sent to the server.
    uint64_t deal_ticket;    ///< Optional DEAL_TICKET local post-filter; zero means all.
    uint64_t order_ticket;   ///< Optional DEAL_ORDER local post-filter; zero means all.
    uint64_t position_id;    ///< Optional DEAL_POSITION_ID local post-filter; zero means all.
    uint32_t reserved;       ///< Reserved; must be zero.
} Mt5HistoryDealsRequest;

/// \struct Mt5DealSnapshot
/// \brief Lossless typed evidence copied from an MT5 deal record.
typedef struct Mt5DealSnapshot {
    uint64_t ticket;             ///< DEAL_TICKET.
    uint64_t order_ticket;       ///< DEAL_ORDER.
    uint64_t position_id;        ///< DEAL_POSITION_ID.
    uint64_t magic;              ///< DEAL_MAGIC.
    uint32_t type;               ///< DEAL_TYPE_*.
    uint32_t entry;              ///< DEAL_ENTRY_*.
    uint32_t reason;             ///< DEAL_REASON_*.
    double volume;               ///< DEAL_VOLUME.
    double price;                ///< DEAL_PRICE.
    double profit;               ///< DEAL_PROFIT.
    double commission;           ///< DEAL_COMMISSION.
    double swap;                 ///< DEAL_SWAP.
    double fee;                  ///< DEAL_FEE.
    int64_t time_msc;            ///< DEAL_TIME converted to milliseconds.
    char symbol[64];             ///< UTF-8 symbol.
    char comment[MT5BRIDGE_TEXT_CAPACITY]; ///< UTF-8 comment.
    char external_id[MT5BRIDGE_TEXT_CAPACITY]; ///< Broker/exchange external id.
    uint64_t known_fields;       ///< Bits identifying fields present in MT5.
    uint32_t reserved[2];        ///< Reserved; must be zero.
} Mt5DealSnapshot;

/// \struct Mt5DealBuffer
/// \brief Opaque DLL-owned historical-deal collection.
typedef struct Mt5DealBuffer Mt5DealBuffer;

/// \brief Returns a bounded history-deal snapshot.
MT5BRIDGE_EXPORT int mt5bridge_query_history_deals(const Mt5HistoryDealsRequest *request,
                                                   Mt5DealBuffer **result);
/// \brief Returns the immutable data stored in a deal buffer.
MT5BRIDGE_EXPORT const Mt5DealSnapshot *mt5bridge_deal_buffer_data(const Mt5DealBuffer *buffer);
/// \brief Returns the number of deals stored in a buffer.
MT5BRIDGE_EXPORT size_t mt5bridge_deal_buffer_size(const Mt5DealBuffer *buffer);
/// \brief Releases a deal buffer allocated by mt5_bridge.dll.
MT5BRIDGE_EXPORT void mt5bridge_deal_buffer_free(Mt5DealBuffer *buffer);

#if defined(__cplusplus)
static_assert(sizeof(Mt5HistoryDealsRequest) == 56,
              "Mt5HistoryDealsRequest ABI size changed");
static_assert(sizeof(Mt5DealSnapshot) == 440, "Mt5DealSnapshot ABI size changed");
static_assert(offsetof(Mt5HistoryDealsRequest, deal_ticket) == 24,
              "Mt5HistoryDealsRequest deal_ticket offset changed");
static_assert(offsetof(Mt5HistoryDealsRequest, order_ticket) == 32,
              "Mt5HistoryDealsRequest order_ticket offset changed");
static_assert(offsetof(Mt5HistoryDealsRequest, position_id) == 40,
              "Mt5HistoryDealsRequest position_id offset changed");
static_assert(offsetof(Mt5DealSnapshot, position_id) == 16,
              "Mt5DealSnapshot position_id offset changed");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(Mt5HistoryDealsRequest) == 56,
               "Mt5HistoryDealsRequest ABI size changed");
_Static_assert(sizeof(Mt5DealSnapshot) == 440, "Mt5DealSnapshot ABI size changed");
_Static_assert(offsetof(Mt5HistoryDealsRequest, deal_ticket) == 24,
               "Mt5HistoryDealsRequest deal_ticket offset changed");
_Static_assert(offsetof(Mt5HistoryDealsRequest, order_ticket) == 32,
               "Mt5HistoryDealsRequest order_ticket offset changed");
_Static_assert(offsetof(Mt5HistoryDealsRequest, position_id) == 40,
               "Mt5HistoryDealsRequest position_id offset changed");
#endif

#ifdef __cplusplus
}
#endif
