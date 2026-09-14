#pragma once

/// \file trade.h
/// \brief Declares the ABI-typed Stage 1 trade observation contracts.

#include <stddef.h>
#include <stdint.h>

#include "abi.h"

/// \def MT5BRIDGE_TRADE_API_VERSION
/// \brief Identifies the additive typed trade-observation surface.
#define MT5BRIDGE_TRADE_API_VERSION 1u

/// \def MT5BRIDGE_TEXT_CAPACITY
/// \brief Maximum UTF-8 bytes stored in fixed-size observation text fields.
#define MT5BRIDGE_TEXT_CAPACITY 128u

/// \def MT5BRIDGE_ACCOUNT_KNOWN_SERVER
/// \brief Bit identifying a present account server field.
#define MT5BRIDGE_ACCOUNT_KNOWN_SERVER (UINT64_C(1) << 0)
/// \def MT5BRIDGE_ACCOUNT_KNOWN_CURRENCY
/// \brief Bit identifying a present account currency field.
#define MT5BRIDGE_ACCOUNT_KNOWN_CURRENCY (UINT64_C(1) << 1)
/// \def MT5BRIDGE_ACCOUNT_KNOWN_LOGIN
/// \brief Bit identifying a present account login field.
#define MT5BRIDGE_ACCOUNT_KNOWN_LOGIN (UINT64_C(1) << 2)
/// \def MT5BRIDGE_ACCOUNT_KNOWN_MARGIN_MODE
/// \brief Bit identifying a present margin mode field.
#define MT5BRIDGE_ACCOUNT_KNOWN_MARGIN_MODE (UINT64_C(1) << 3)
/// \def MT5BRIDGE_ACCOUNT_KNOWN_TRADE_MODE
/// \brief Bit identifying a present account trade mode field.
#define MT5BRIDGE_ACCOUNT_KNOWN_TRADE_MODE (UINT64_C(1) << 4)
/// \def MT5BRIDGE_ACCOUNT_KNOWN_LEVERAGE
/// \brief Bit identifying a present leverage field.
#define MT5BRIDGE_ACCOUNT_KNOWN_LEVERAGE (UINT64_C(1) << 5)
/// \def MT5BRIDGE_ACCOUNT_KNOWN_TRADE_ALLOWED
/// \brief Bit identifying a present account trade permission field.
#define MT5BRIDGE_ACCOUNT_KNOWN_TRADE_ALLOWED (UINT64_C(1) << 6)
/// \def MT5BRIDGE_ACCOUNT_KNOWN_TRADE_EXPERT
/// \brief Bit identifying a present expert/API permission field.
#define MT5BRIDGE_ACCOUNT_KNOWN_TRADE_EXPERT (UINT64_C(1) << 7)
/// \def MT5BRIDGE_ACCOUNT_KNOWN_FIFO_CLOSE
/// \brief Bit identifying a present FIFO-close policy field.
#define MT5BRIDGE_ACCOUNT_KNOWN_FIFO_CLOSE (UINT64_C(1) << 8)
/// \def MT5BRIDGE_ACCOUNT_KNOWN_HEDGE_ALLOWED
/// \brief Bit identifying a directly reported hedge permission field.
#define MT5BRIDGE_ACCOUNT_KNOWN_HEDGE_ALLOWED (UINT64_C(1) << 9)
/// \def MT5BRIDGE_ACCOUNT_KNOWN_BALANCE
/// \brief Bit identifying a present balance field.
#define MT5BRIDGE_ACCOUNT_KNOWN_BALANCE (UINT64_C(1) << 10)
/// \def MT5BRIDGE_ACCOUNT_KNOWN_EQUITY
/// \brief Bit identifying a present equity field.
#define MT5BRIDGE_ACCOUNT_KNOWN_EQUITY (UINT64_C(1) << 11)

/// \def MT5BRIDGE_SYMBOL_KNOWN_TRADE_MODE
/// \brief Bit identifying a present symbol trade mode field.
#define MT5BRIDGE_SYMBOL_KNOWN_TRADE_MODE (UINT64_C(1) << 0)
/// \def MT5BRIDGE_SYMBOL_KNOWN_ORDER_MODE
/// \brief Bit identifying a present symbol order mode field.
#define MT5BRIDGE_SYMBOL_KNOWN_ORDER_MODE (UINT64_C(1) << 1)
/// \def MT5BRIDGE_SYMBOL_KNOWN_TRADE_EXEMODE
/// \brief Bit identifying a present symbol execution mode field.
#define MT5BRIDGE_SYMBOL_KNOWN_TRADE_EXEMODE (UINT64_C(1) << 2)
/// \def MT5BRIDGE_SYMBOL_KNOWN_FILLING_MODE
/// \brief Bit identifying a present symbol filling mode field.
#define MT5BRIDGE_SYMBOL_KNOWN_FILLING_MODE (UINT64_C(1) << 3)
/// \def MT5BRIDGE_SYMBOL_KNOWN_EXPIRATION_MODE
/// \brief Bit identifying a present symbol expiration mode field.
#define MT5BRIDGE_SYMBOL_KNOWN_EXPIRATION_MODE (UINT64_C(1) << 4)
/// \def MT5BRIDGE_SYMBOL_KNOWN_ORDER_GTC_MODE
/// \brief Bit identifying a present GTC policy field.
#define MT5BRIDGE_SYMBOL_KNOWN_ORDER_GTC_MODE (UINT64_C(1) << 5)
/// \def MT5BRIDGE_SYMBOL_KNOWN_STOPS_LEVEL
/// \brief Bit identifying a present stops-level field.
#define MT5BRIDGE_SYMBOL_KNOWN_STOPS_LEVEL (UINT64_C(1) << 6)
/// \def MT5BRIDGE_SYMBOL_KNOWN_FREEZE_LEVEL
/// \brief Bit identifying a present freeze-level field.
#define MT5BRIDGE_SYMBOL_KNOWN_FREEZE_LEVEL (UINT64_C(1) << 7)
/// \def MT5BRIDGE_SYMBOL_KNOWN_CLOSEBY
/// \brief Bit identifying a derived CLOSEBY capability.
#define MT5BRIDGE_SYMBOL_KNOWN_CLOSEBY (UINT64_C(1) << 8)
/// \def MT5BRIDGE_SYMBOL_KNOWN_VISIBLE
/// \brief Bit identifying a present visibility field.
#define MT5BRIDGE_SYMBOL_KNOWN_VISIBLE (UINT64_C(1) << 9)
/// \def MT5BRIDGE_SYMBOL_KNOWN_SELECTED
/// \brief Bit identifying a present Market Watch selection field.
#define MT5BRIDGE_SYMBOL_KNOWN_SELECTED (UINT64_C(1) << 10)
/// \def MT5BRIDGE_SYMBOL_KNOWN_VOLUME_LIMITS
/// \brief Bit identifying all symbol volume limit fields.
#define MT5BRIDGE_SYMBOL_KNOWN_VOLUME_LIMITS (UINT64_C(1) << 11)
/// \def MT5BRIDGE_SYMBOL_KNOWN_TICK_SIZE
/// \brief Bit identifying a present trade tick size field.
#define MT5BRIDGE_SYMBOL_KNOWN_TICK_SIZE (UINT64_C(1) << 12)
/// \def MT5BRIDGE_SYMBOL_KNOWN_POINT
/// \brief Bit identifying a present point field.
#define MT5BRIDGE_SYMBOL_KNOWN_POINT (UINT64_C(1) << 13)

#ifdef __cplusplus
extern "C" {
#endif

/// \struct Mt5AccountInfo
/// \brief Snapshot of account identity, permissions, and margin state.
///
/// Text fields are UTF-8 and are always null-terminated when the call succeeds.
/// A field is usable only when its bit is set in known_fields; zero can be a
/// valid MT5 value and is never used to represent an absent field.
typedef struct Mt5AccountInfo {
    char server[MT5BRIDGE_TEXT_CAPACITY]; ///< Immutable terminal server identity.
    char currency[16];                    ///< Account currency code.
    uint64_t login;                       ///< Account login number.
    int32_t margin_mode;                  ///< MT5 ACCOUNT_MARGIN_MODE_* value.
    int32_t trade_mode;                   ///< MT5 ACCOUNT_TRADE_MODE_* value.
    int32_t leverage;                     ///< Account leverage, or zero when unavailable.
    uint8_t trade_allowed;                ///< Non-zero when trading is allowed.
    uint8_t trade_expert;                 ///< Non-zero when expert/API trading is allowed.
    uint8_t fifo_close;                   ///< Non-zero when FIFO close is enforced.
    uint8_t hedge_allowed;                ///< Direct hedge permission, when known.
    double balance;                       ///< Current account balance.
    double equity;                        ///< Current account equity.
    uint64_t known_fields;                ///< MT5BRIDGE_ACCOUNT_KNOWN_* mask.
} Mt5AccountInfo;

/// \struct Mt5SymbolRequest
/// \brief Identifies one symbol for a capability snapshot.
typedef struct Mt5SymbolRequest {
    const char *symbol_utf8; ///< Borrowed null-terminated UTF-8 symbol name.
    uint32_t reserved;       ///< Reserved; must be zero.
} Mt5SymbolRequest;

/// \struct Mt5SymbolCapabilities
/// \brief Snapshot of symbol execution, order, and sizing capabilities.
///
/// Consumers must check known_fields before making a policy decision. The
/// bridge reports missing fields as unknown rather than guessing from zero.
typedef struct Mt5SymbolCapabilities {
    char symbol[64];          ///< Canonical symbol name returned by MT5.
    uint32_t trade_mode;      ///< MT5 SYMBOL_TRADE_MODE_* value.
    uint32_t trade_exemode;    ///< MT5 SYMBOL_TRADE_EXECUTION_* value.
    uint32_t order_mode;      ///< MT5 SYMBOL_ORDER_* bit mask.
    uint32_t filling_mode;    ///< MT5 SYMBOL_FILLING_* bit mask.
    uint32_t expiration_mode; ///< MT5 SYMBOL_EXPIRATION_* bit mask.
    uint32_t order_gtc_mode;  ///< MT5 SYMBOL_ORDERTIME_GTC mode.
    int32_t trade_stops_level;  ///< Minimum stops distance in points.
    int32_t trade_freeze_level; ///< Freeze distance in points.
    uint32_t closeby_allowed;   ///< Non-zero when SYMBOL_ORDER_CLOSEBY is supported.
    uint32_t visible;           ///< Non-zero when the symbol is visible in Market Watch.
    uint32_t selected;          ///< Non-zero when the symbol is selected.
    double volume_min;          ///< Minimum order volume.
    double volume_max;          ///< Maximum order volume.
    double volume_step;         ///< Volume increment.
    double volume_limit;        ///< Aggregate directional volume limit.
    double trade_tick_size;     ///< Minimum price increment.
    double point;               ///< Symbol point size.
    uint64_t known_fields;      ///< MT5BRIDGE_SYMBOL_KNOWN_* mask.
} Mt5SymbolCapabilities;

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

/// \brief Returns the version of the additive typed trade-observation API.
/// \return MT5BRIDGE_TRADE_API_VERSION for this runtime.
MT5BRIDGE_EXPORT uint32_t mt5bridge_trade_api_version(void);

/// \brief Copies the current account identity and capabilities.
/// \param[out] info Destination for the account snapshot.
/// \return Zero on success; non-zero when the runtime is unavailable or data cannot be read.
MT5BRIDGE_EXPORT int mt5bridge_account_info(Mt5AccountInfo *info);

/// \brief Copies symbol execution and sizing capabilities.
/// \param request Symbol name and reserved-field contract.
/// \param[out] capabilities Destination for the capability snapshot.
/// \return Zero on success; non-zero for invalid input, unavailable symbol, or runtime failure.
MT5BRIDGE_EXPORT int mt5bridge_symbol_capabilities(
    const Mt5SymbolRequest *request, Mt5SymbolCapabilities *capabilities);

/// \brief Runs the advisory MetaTrader order_check operation.
/// \param request Plain-C request fields; no order is submitted.
/// \param[out] result Destination for the raw advisory result.
/// \return Zero when a result was returned, even when its retcode rejects the request.
/// \note This call has no durable dispatch or restart guarantee and is not order_send.
MT5BRIDGE_EXPORT int mt5bridge_order_check(const Mt5OrderCheckRequest *request,
                                           Mt5OrderCheckResult *result);

#if defined(__cplusplus)
static_assert(sizeof(Mt5AccountInfo) == 192, "Mt5AccountInfo ABI size changed");
static_assert(sizeof(Mt5SymbolRequest) == 16, "Mt5SymbolRequest ABI size changed");
static_assert(sizeof(Mt5SymbolCapabilities) == 168,
              "Mt5SymbolCapabilities ABI size changed");
static_assert(sizeof(Mt5OrderCheckRequest) == 128,
              "Mt5OrderCheckRequest ABI size changed");
static_assert(sizeof(Mt5OrderCheckResult) == 192,
              "Mt5OrderCheckResult ABI size changed");
static_assert(offsetof(Mt5AccountInfo, login) == 144, "Mt5AccountInfo login offset changed");
static_assert(offsetof(Mt5AccountInfo, known_fields) == 184,
              "Mt5AccountInfo known_fields offset changed");
static_assert(offsetof(Mt5SymbolCapabilities, trade_exemode) == 68,
              "Mt5SymbolCapabilities trade_exemode offset changed");
static_assert(offsetof(Mt5SymbolCapabilities, volume_min) == 112,
              "Mt5SymbolCapabilities volume_min offset changed");
static_assert(offsetof(Mt5SymbolCapabilities, known_fields) == 160,
              "Mt5SymbolCapabilities known_fields offset changed");
static_assert(offsetof(Mt5OrderCheckRequest, action) == 96,
              "Mt5OrderCheckRequest action offset changed");
static_assert(offsetof(Mt5OrderCheckResult, balance) == 8,
              "Mt5OrderCheckResult balance offset changed");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(Mt5AccountInfo) == 192, "Mt5AccountInfo ABI size changed");
_Static_assert(sizeof(Mt5SymbolRequest) == 16, "Mt5SymbolRequest ABI size changed");
_Static_assert(sizeof(Mt5SymbolCapabilities) == 168,
               "Mt5SymbolCapabilities ABI size changed");
_Static_assert(sizeof(Mt5OrderCheckRequest) == 128,
               "Mt5OrderCheckRequest ABI size changed");
_Static_assert(sizeof(Mt5OrderCheckResult) == 192,
               "Mt5OrderCheckResult ABI size changed");
_Static_assert(offsetof(Mt5AccountInfo, login) == 144, "Mt5AccountInfo login offset changed");
_Static_assert(offsetof(Mt5AccountInfo, known_fields) == 184,
               "Mt5AccountInfo known_fields offset changed");
_Static_assert(offsetof(Mt5SymbolCapabilities, trade_exemode) == 68,
               "Mt5SymbolCapabilities trade_exemode offset changed");
_Static_assert(offsetof(Mt5SymbolCapabilities, volume_min) == 112,
               "Mt5SymbolCapabilities volume_min offset changed");
_Static_assert(offsetof(Mt5SymbolCapabilities, known_fields) == 160,
               "Mt5SymbolCapabilities known_fields offset changed");
_Static_assert(offsetof(Mt5OrderCheckRequest, action) == 96,
               "Mt5OrderCheckRequest action offset changed");
_Static_assert(offsetof(Mt5OrderCheckResult, balance) == 8,
               "Mt5OrderCheckResult balance offset changed");
#endif

#ifdef __cplusplus
}
#endif
