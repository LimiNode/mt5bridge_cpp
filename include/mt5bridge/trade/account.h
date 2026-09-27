#pragma once

/// \file trade/account.h
/// \brief Declares account identity and capability observations.

#include <mt5bridge/trade/common.h>

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

/// \brief Copies the current account identity and capabilities.
/// \param[out] info Destination for the account snapshot.
/// \return Zero on success; non-zero when the runtime is unavailable or data cannot be read.
MT5BRIDGE_EXPORT int mt5bridge_account_info(Mt5AccountInfo *info);

#if defined(__cplusplus)
static_assert(sizeof(Mt5AccountInfo) == 192, "Mt5AccountInfo ABI size changed");
static_assert(offsetof(Mt5AccountInfo, login) == 144, "Mt5AccountInfo login offset changed");
static_assert(offsetof(Mt5AccountInfo, known_fields) == 184,
              "Mt5AccountInfo known_fields offset changed");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(Mt5AccountInfo) == 192, "Mt5AccountInfo ABI size changed");
_Static_assert(offsetof(Mt5AccountInfo, login) == 144, "Mt5AccountInfo login offset changed");
_Static_assert(offsetof(Mt5AccountInfo, known_fields) == 184,
               "Mt5AccountInfo known_fields offset changed");
#endif

#ifdef __cplusplus
}
#endif
