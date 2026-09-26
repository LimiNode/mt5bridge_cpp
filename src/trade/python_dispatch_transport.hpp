#pragma once

/// \file python_dispatch_transport.hpp
/// \brief Declares the private embedded-Python dispatch transport.

#include "one_shot_backend.hpp"

#include <optional>

struct _object;

namespace mt5bridge::runtime {

/// \class Mt5PythonAccountProbe
/// \brief Reads the selected account from an embedded MetaTrader5 module.
class Mt5PythonAccountProbe final : public CurrentAccountProbe {
public:
    /// \brief Binds the probe to a borrowed `MetaTrader5` module.
    /// \param mt5 Borrowed module object; the caller owns its lifetime.
    explicit Mt5PythonAccountProbe(_object *mt5) : mt5_(mt5) {}

    /// \brief Reads the current server/login identity.
    /// \return A complete account key, or empty when Python returned invalid data.
    std::optional<AccountKey> current_account() override;

private:
    _object *mt5_ = nullptr;
};

/// \class Mt5PythonDispatchTransport
/// \brief Performs one guarded `MetaTrader5.order_send` call.
///
/// The operation request payload is UTF-8 JSON containing the Python
/// `MqlTradeRequest` mapping. The returned `MqlTradeResult` is validated and
/// serialized as UTF-8 JSON for durable opaque storage. This adapter never
/// retries and never treats a missing or malformed result as a broker result.
class Mt5PythonDispatchTransport final : public DispatchTransport {
public:
    /// \brief Binds the transport to a borrowed module and live account probe.
    /// \param mt5 Borrowed `MetaTrader5` module; the caller owns its lifetime.
    /// \param account_probe Probe used immediately before `order_send`.
    explicit Mt5PythonDispatchTransport(_object *mt5, CurrentAccountProbe &account_probe)
        : mt5_(mt5), account_probe_(account_probe) {}

    /// \brief Calls `order_send` exactly once and validates the broker result.
    /// \param record Durable operation containing a JSON request payload.
    /// \return Broker result, account mismatch, or unresolved transport failure.
    /// \note The caller must hold runtime admission and the CPython GIL.
    BackendCallResult submit_once(const OperationRecord &record) override;

private:
    _object *mt5_ = nullptr;
    CurrentAccountProbe &account_probe_;
};

} // namespace mt5bridge::runtime
