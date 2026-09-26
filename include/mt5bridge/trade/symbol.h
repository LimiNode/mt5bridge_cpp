#pragma once

/// \file trade/symbol.h
/// \brief Declares symbol execution and sizing capability observations.

#include <mt5bridge/trade/common.h>

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
    uint32_t trade_exemode;   ///< MT5 SYMBOL_TRADE_EXECUTION_* value.
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

/// \brief Copies symbol execution and sizing capabilities.
/// \param request Symbol name and reserved-field contract.
/// \param[out] capabilities Destination for the capability snapshot.
/// \return Zero on success; non-zero for invalid input, unavailable symbol, or runtime failure.
MT5BRIDGE_EXPORT int mt5bridge_symbol_capabilities(
    const Mt5SymbolRequest *request, Mt5SymbolCapabilities *capabilities);

#if defined(__cplusplus)
static_assert(sizeof(Mt5SymbolRequest) == 16, "Mt5SymbolRequest ABI size changed");
static_assert(sizeof(Mt5SymbolCapabilities) == 168,
              "Mt5SymbolCapabilities ABI size changed");
static_assert(offsetof(Mt5SymbolCapabilities, trade_exemode) == 68,
              "Mt5SymbolCapabilities trade_exemode offset changed");
static_assert(offsetof(Mt5SymbolCapabilities, volume_min) == 112,
              "Mt5SymbolCapabilities volume_min offset changed");
static_assert(offsetof(Mt5SymbolCapabilities, known_fields) == 160,
              "Mt5SymbolCapabilities known_fields offset changed");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(Mt5SymbolRequest) == 16, "Mt5SymbolRequest ABI size changed");
_Static_assert(sizeof(Mt5SymbolCapabilities) == 168,
               "Mt5SymbolCapabilities ABI size changed");
_Static_assert(offsetof(Mt5SymbolCapabilities, trade_exemode) == 68,
               "Mt5SymbolCapabilities trade_exemode offset changed");
_Static_assert(offsetof(Mt5SymbolCapabilities, volume_min) == 112,
               "Mt5SymbolCapabilities volume_min offset changed");
_Static_assert(offsetof(Mt5SymbolCapabilities, known_fields) == 160,
               "Mt5SymbolCapabilities known_fields offset changed");
#endif

#ifdef __cplusplus
}
#endif
