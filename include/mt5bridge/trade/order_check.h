#pragma once

/// \file trade/order_check.h
/// \brief Declares the advisory order-check request and result ABI.

#include <mt5bridge/trade/common.h>

#ifdef __cplusplus
extern "C" {
#endif

/// \struct Mt5OrderCheckRequest
/// \brief Complete plain-C request passed to the advisory order_check call.
///
/// Zero-valued optional numeric fields are forwarded as supplied. A null symbol
/// or comment omits that key from the Python request.
typedef struct Mt5OrderCheckRequest {
    const char *symbol_utf8; ///< Borrowed symbol name, or NULL for order-only actions.
    const char *comment_utf8; ///< Borrowed order comment, or NULL.
    uint64_t magic;          ///< Expert/magic identifier.
    uint64_t order;          ///< Existing order ticket for cancel/modify actions.
    uint64_t position;       ///< Position ticket for close/modify actions.
    uint64_t position_by;    ///< Opposite position ticket for CloseBy.
    double volume;           ///< Requested volume in lots.
    double price;            ///< Requested price.
    double stoplimit;        ///< Stop-limit trigger price.
    double sl;               ///< Stop-loss price.
    double tp;               ///< Take-profit price.
    int64_t expiration;      ///< Unix timestamp in seconds, or zero.
    uint32_t action;         ///< MT5 TRADE_ACTION_* value.
    uint32_t type;           ///< MT5 ORDER_TYPE_* value.
    uint32_t type_filling;   ///< MT5 ORDER_FILLING_* value.
    uint32_t type_time;      ///< MT5 ORDER_TIME_* value.
    uint32_t deviation;      ///< Maximum price deviation in points.
    uint32_t reserved[3];    ///< Reserved; must be zero.
} Mt5OrderCheckRequest;

/// \struct Mt5OrderCheckResult
/// \brief Raw advisory result returned by MetaTrader's order_check call.
///
/// This mirrors MqlTradeCheckResult. retcode_external belongs to the future
/// side-effecting order_send result and is intentionally absent here.
/// Every documented field is required in the backend record; a missing field
/// is reported as an error instead of being silently returned as zero/default.
typedef struct Mt5OrderCheckResult {
    uint32_t retcode;         ///< MT5 TRADE_RETCODE_* value.
    double balance;           ///< Projected balance.
    double equity;            ///< Projected equity.
    double profit;            ///< Projected profit.
    double margin;            ///< Projected margin.
    double margin_free;       ///< Projected free margin.
    double margin_level;      ///< Projected margin level.
    char comment[MT5BRIDGE_TEXT_CAPACITY]; ///< Server diagnostic comment.
    uint32_t reserved[2];     ///< Reserved; must be zero.
} Mt5OrderCheckResult;

/// \brief Runs the advisory MetaTrader order_check operation.
/// \param request Plain-C request fields; no order is submitted.
/// \param[out] result Destination for the raw advisory result.
/// \return Zero when a result was returned, even when its retcode rejects the request.
/// \note This call has no durable dispatch or restart guarantee and is not order_send.
MT5BRIDGE_EXPORT int mt5bridge_order_check(const Mt5OrderCheckRequest *request,
                                           Mt5OrderCheckResult *result);

#if defined(__cplusplus)
static_assert(sizeof(Mt5OrderCheckRequest) == 128,
              "Mt5OrderCheckRequest ABI size changed");
static_assert(sizeof(Mt5OrderCheckResult) == 192,
              "Mt5OrderCheckResult ABI size changed");
static_assert(offsetof(Mt5OrderCheckRequest, action) == 96,
              "Mt5OrderCheckRequest action offset changed");
static_assert(offsetof(Mt5OrderCheckResult, balance) == 8,
              "Mt5OrderCheckResult balance offset changed");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(Mt5OrderCheckRequest) == 128,
               "Mt5OrderCheckRequest ABI size changed");
_Static_assert(sizeof(Mt5OrderCheckResult) == 192,
               "Mt5OrderCheckResult ABI size changed");
_Static_assert(offsetof(Mt5OrderCheckRequest, action) == 96,
               "Mt5OrderCheckRequest action offset changed");
_Static_assert(offsetof(Mt5OrderCheckResult, balance) == 8,
               "Mt5OrderCheckResult balance offset changed");
#endif

#ifdef __cplusplus
}
#endif
