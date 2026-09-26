#pragma once

/// \file trade/common.h
/// \brief Declares constants shared by the plain-C trade ABI.

#include <stddef.h>
#include <stdint.h>

#include <mt5bridge/abi.h>

/// \def MT5BRIDGE_TRADE_API_VERSION
/// \brief Identifies the additive typed trade surface.
#define MT5BRIDGE_TRADE_API_VERSION 2u

/// \def MT5BRIDGE_TEXT_CAPACITY
/// \brief Maximum UTF-8 bytes stored in fixed-size trade text fields.
#define MT5BRIDGE_TEXT_CAPACITY 128u

#ifdef __cplusplus
extern "C" {
#endif

/// \brief Returns the version of the additive typed trade API.
/// \return MT5BRIDGE_TRADE_API_VERSION for this runtime.
MT5BRIDGE_EXPORT uint32_t mt5bridge_trade_api_version(void);

#ifdef __cplusplus
}
#endif
