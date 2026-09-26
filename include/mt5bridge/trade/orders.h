#pragma once

/// \file trade/orders.h
/// \brief Declares active and historical order observation contracts.

#include <mt5bridge/trade/common.h>

/// \name Order snapshot known-field masks
/// \brief Bits distinguish an absent MT5 field from a valid zero value.
/// \{
#define MT5BRIDGE_ORDER_KNOWN_TICKET (UINT64_C(1) << 0)
#define MT5BRIDGE_ORDER_KNOWN_POSITION_ID (UINT64_C(1) << 1)
#define MT5BRIDGE_ORDER_KNOWN_POSITION_BY_ID (UINT64_C(1) << 2)
#define MT5BRIDGE_ORDER_KNOWN_MAGIC (UINT64_C(1) << 3)
#define MT5BRIDGE_ORDER_KNOWN_TYPE (UINT64_C(1) << 4)
#define MT5BRIDGE_ORDER_KNOWN_STATE (UINT64_C(1) << 5)
#define MT5BRIDGE_ORDER_KNOWN_REASON (UINT64_C(1) << 6)
#define MT5BRIDGE_ORDER_KNOWN_TYPE_TIME (UINT64_C(1) << 7)
#define MT5BRIDGE_ORDER_KNOWN_TYPE_FILLING (UINT64_C(1) << 8)
#define MT5BRIDGE_ORDER_KNOWN_VOLUME_INITIAL (UINT64_C(1) << 9)
#define MT5BRIDGE_ORDER_KNOWN_VOLUME_CURRENT (UINT64_C(1) << 10)
#define MT5BRIDGE_ORDER_KNOWN_PRICE_OPEN (UINT64_C(1) << 11)
#define MT5BRIDGE_ORDER_KNOWN_PRICE_CURRENT (UINT64_C(1) << 12)
#define MT5BRIDGE_ORDER_KNOWN_PRICE_STOPLIMIT (UINT64_C(1) << 13)
#define MT5BRIDGE_ORDER_KNOWN_SL (UINT64_C(1) << 14)
#define MT5BRIDGE_ORDER_KNOWN_TP (UINT64_C(1) << 15)
#define MT5BRIDGE_ORDER_KNOWN_TIME_SETUP (UINT64_C(1) << 16)
#define MT5BRIDGE_ORDER_KNOWN_TIME_DONE (UINT64_C(1) << 17)
#define MT5BRIDGE_ORDER_KNOWN_TIME_EXPIRATION (UINT64_C(1) << 18)
#define MT5BRIDGE_ORDER_KNOWN_SYMBOL (UINT64_C(1) << 19)
#define MT5BRIDGE_ORDER_KNOWN_COMMENT (UINT64_C(1) << 20)
#define MT5BRIDGE_ORDER_KNOWN_EXTERNAL_ID (UINT64_C(1) << 21)
/// \}

#ifdef __cplusplus
extern "C" {
#endif

/// \struct Mt5OrdersRequest
/// \brief Filters an active-order snapshot.
typedef struct Mt5OrdersRequest {
    const char *symbol_utf8; ///< Optional symbol selector; exclusive with group/ticket.
    const char *group_utf8;  ///< Optional MT5 group selector; exclusive with symbol/ticket.
    uint64_t ticket;         ///< Optional order ticket selector; exclusive with symbol/group.
    uint32_t reserved;       ///< Reserved; must be zero.
} Mt5OrdersRequest;

/// \struct Mt5HistoryOrdersRequest
/// \brief Bounds history orders by time and optional local identity filters.
typedef struct Mt5HistoryOrdersRequest {
    int64_t from_msc;        ///< Inclusive UTC range start in milliseconds.
    int64_t to_msc;          ///< Inclusive UTC range end in milliseconds.
    const char *group_utf8;  ///< Optional MT5 group selector sent to the server.
    uint64_t order_ticket;   ///< Optional ORDER_TICKET local post-filter; zero means all.
    uint64_t position_id;    ///< Optional POSITION_IDENTIFIER local post-filter; zero means all.
    uint32_t reserved;       ///< Reserved; must be zero.
} Mt5HistoryOrdersRequest;

/// \struct Mt5OrderSnapshot
/// \brief Lossless typed evidence copied from an MT5 order record.
typedef struct Mt5OrderSnapshot {
    uint64_t ticket;             ///< ORDER_TICKET.
    uint64_t position_id;        ///< ORDER_POSITION_ID.
    uint64_t position_by_id;     ///< ORDER_POSITION_BY_ID.
    uint64_t magic;              ///< ORDER_MAGIC.
    uint32_t type;               ///< ORDER_TYPE_*.
    uint32_t state;              ///< ORDER_STATE_*.
    uint32_t reason;             ///< ORDER_REASON_*.
    uint32_t type_time;          ///< ORDER_TYPE_TIME_*.
    uint32_t type_filling;       ///< ORDER_TYPE_FILLING_*.
    double volume_initial;       ///< ORDER_VOLUME_INITIAL.
    double volume_current;       ///< ORDER_VOLUME_CURRENT.
    double price_open;           ///< ORDER_PRICE_OPEN.
    double price_current;        ///< ORDER_PRICE_CURRENT.
    double price_stoplimit;      ///< ORDER_PRICE_STOPLIMIT.
    double sl;                   ///< ORDER_SL.
    double tp;                   ///< ORDER_TP.
    int64_t time_setup_msc;      ///< ORDER_TIME_SETUP converted to milliseconds.
    int64_t time_done_msc;       ///< ORDER_TIME_DONE converted to milliseconds.
    int64_t time_expiration_msc; ///< ORDER_TIME_EXPIRATION converted to milliseconds.
    char symbol[64];             ///< UTF-8 symbol.
    char comment[MT5BRIDGE_TEXT_CAPACITY]; ///< UTF-8 comment.
    char external_id[MT5BRIDGE_TEXT_CAPACITY]; ///< Broker/exchange external id.
    uint64_t known_fields;       ///< Bits identifying fields present in MT5.
    uint32_t reserved[2];        ///< Reserved; must be zero.
} Mt5OrderSnapshot;

/// \typedef Mt5HistoryOrderSnapshot
/// \brief History-order evidence with the same shape as an order snapshot.
typedef Mt5OrderSnapshot Mt5HistoryOrderSnapshot;

/// \struct Mt5OrderBuffer
/// \brief Opaque DLL-owned active-order collection.
typedef struct Mt5OrderBuffer Mt5OrderBuffer;

/// \struct Mt5HistoryOrderBuffer
/// \brief Opaque DLL-owned historical-order collection.
typedef struct Mt5HistoryOrderBuffer Mt5HistoryOrderBuffer;

/// \brief Returns an active-order snapshot.
MT5BRIDGE_EXPORT int mt5bridge_query_orders(const Mt5OrdersRequest *request,
                                            Mt5OrderBuffer **result);
/// \brief Returns the immutable data stored in an active-order buffer.
MT5BRIDGE_EXPORT const Mt5OrderSnapshot *mt5bridge_order_buffer_data(
    const Mt5OrderBuffer *buffer);
/// \brief Returns the number of orders stored in an active-order buffer.
MT5BRIDGE_EXPORT size_t mt5bridge_order_buffer_size(const Mt5OrderBuffer *buffer);
/// \brief Releases an active-order buffer allocated by mt5_bridge.dll.
MT5BRIDGE_EXPORT void mt5bridge_order_buffer_free(Mt5OrderBuffer *buffer);

/// \brief Returns a bounded history-order snapshot.
MT5BRIDGE_EXPORT int mt5bridge_query_history_orders(const Mt5HistoryOrdersRequest *request,
                                                    Mt5HistoryOrderBuffer **result);
/// \brief Returns the immutable data stored in a history-order buffer.
MT5BRIDGE_EXPORT const Mt5HistoryOrderSnapshot *mt5bridge_history_order_buffer_data(
    const Mt5HistoryOrderBuffer *buffer);
/// \brief Returns the number of orders stored in a history-order buffer.
MT5BRIDGE_EXPORT size_t mt5bridge_history_order_buffer_size(
    const Mt5HistoryOrderBuffer *buffer);
/// \brief Releases a history-order buffer allocated by mt5_bridge.dll.
MT5BRIDGE_EXPORT void mt5bridge_history_order_buffer_free(Mt5HistoryOrderBuffer *buffer);

#if defined(__cplusplus)
static_assert(sizeof(Mt5OrdersRequest) == 32, "Mt5OrdersRequest ABI size changed");
static_assert(sizeof(Mt5HistoryOrdersRequest) == 48,
              "Mt5HistoryOrdersRequest ABI size changed");
static_assert(sizeof(Mt5OrderSnapshot) == 472, "Mt5OrderSnapshot ABI size changed");
static_assert(offsetof(Mt5HistoryOrdersRequest, order_ticket) == 24,
              "Mt5HistoryOrdersRequest order_ticket offset changed");
static_assert(offsetof(Mt5OrderSnapshot, position_id) == 8,
              "Mt5OrderSnapshot position_id offset changed");
static_assert(offsetof(Mt5OrderSnapshot, time_setup_msc) == 112,
              "Mt5OrderSnapshot time_setup_msc offset changed");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(Mt5OrdersRequest) == 32, "Mt5OrdersRequest ABI size changed");
_Static_assert(sizeof(Mt5HistoryOrdersRequest) == 48,
               "Mt5HistoryOrdersRequest ABI size changed");
_Static_assert(sizeof(Mt5OrderSnapshot) == 472, "Mt5OrderSnapshot ABI size changed");
_Static_assert(offsetof(Mt5HistoryOrdersRequest, order_ticket) == 24,
               "Mt5HistoryOrdersRequest order_ticket offset changed");
#endif

#ifdef __cplusplus
}
#endif
