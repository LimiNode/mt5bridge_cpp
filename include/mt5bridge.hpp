#pragma once

/// \file mt5bridge.hpp
/// \brief Provides the primary public C++ entry point for mt5bridge.

#if !defined(_WIN32)
#error "mt5bridge is only supported on Windows"
#endif

// Keep the umbrella limited to the public surface.  Python, NumPy and
// MetaTrader5 remain implementation details of the runtime DLL.
#include <mt5bridge/abi.h>
#include <mt5bridge/data.h>
#include <mt5bridge/trade.h>
#include <mt5bridge/reconciliation.hpp>
#include <mt5bridge/reconciliation_worker.hpp>
#include <mt5bridge/client.hpp>
