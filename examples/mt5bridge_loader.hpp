#pragma once

/// \file mt5bridge_loader.hpp
/// \brief Preserves the historical example loader name as a client alias.

// Compatibility include for older examples. Production consumers should use
// <mt5bridge.hpp> directly.
#include <mt5bridge.hpp>

/// \brief Compatibility alias for the production mt5bridge C++ client.
using Mt5BridgeApi = mt5bridge::Client;
