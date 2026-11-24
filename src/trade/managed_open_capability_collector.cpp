/// \file managed_open_capability_collector.cpp
/// \brief Strictly collects stable runtime capability facts without OPEN admission.

#ifndef PY_SSIZE_T_CLEAN
#define PY_SSIZE_T_CLEAN
#endif
#include <Python.h>

#include "managed_open_capability_collector.hpp"
#include "managed_opening_quantity_contract.hpp"
#include "python_ref.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace mt5bridge::trade::detail {
namespace {

using runtime::PyRef;
using Status = ManagedOpenCollectionStatus;

/// \brief Bounds account-wide collections before allocating row storage.
constexpr std::uint64_t kMaximumRows = 100000;
/// \brief Enumerates the documented SYMBOL_ORDER_* flags.
constexpr std::uint64_t kKnownOrderModes = 0x7f;
/// \brief Enumerates the documented SYMBOL_FILLING_* flags, including BOC.
constexpr std::uint64_t kKnownFillingModes = 0x07;

/// \brief Clears Python diagnostics at the private decoder failure boundary.
ManagedOpenReadResult failure(Status status) {
    PyErr_Clear();
    return {status, std::nullopt};
}

/// \brief Returns an owned dictionary field or namedtuple/object attribute.
PyRef field(PyObject *object, const char *name) {
    if (!object || object == Py_None || PyErr_Occurred())
        return PyRef();
    if (PyDict_Check(object)) {
        auto *value = PyDict_GetItemString(object, name);
        Py_XINCREF(value);
        return PyRef(value);
    }
    return PyRef(PyObject_GetAttrString(object, name));
}

/// \brief Reads a nonnegative integer without accepting Python bool or coercion.
bool read_unsigned(PyObject *object, const char *name, std::uint64_t &value) {
    const auto item = field(object, name);
    if (!item || !PyLong_CheckExact(item.get()))
        return false;
    const auto converted = PyLong_AsUnsignedLongLong(item.get());
    if (PyErr_Occurred())
        return false;
    value = static_cast<std::uint64_t>(converted);
    return true;
}

/// \brief Reads a reported permission only when it is an actual Python bool.
bool read_bool(PyObject *object, const char *name, bool &value) {
    const auto item = field(object, name);
    if (!item || !PyBool_Check(item.get()))
        return false;
    value = item.get() == Py_True;
    return true;
}

/// \brief Reads a finite float/integer without invoking numeric coercions.
bool read_number(PyObject *object, const char *name, double &value) {
    const auto item = field(object, name);
    if (!item || (!PyFloat_CheckExact(item.get()) && !PyLong_CheckExact(item.get())))
        return false;
    value = PyFloat_AsDouble(item.get());
    return !PyErr_Occurred() && std::isfinite(value);
}

/// \brief Copies a complete bounded UTF-8 identity, rejecting controls and NUL.
bool decode_identity(PyObject *item, std::size_t capacity, std::string &text) {
    if (!item || !PyUnicode_CheckExact(item))
        return false;
    Py_ssize_t length = 0;
    const auto *utf8 = PyUnicode_AsUTF8AndSize(item, &length);
    if (!utf8 || length <= 0 || static_cast<std::size_t>(length) >= capacity ||
        std::memchr(utf8, '\0', static_cast<std::size_t>(length)))
        return false;
    const auto characters = PyUnicode_GetLength(item);
    if (characters < 0)
        return false;
    for (Py_ssize_t index = 0; index != characters; ++index) {
        const auto character = PyUnicode_ReadChar(item, index);
        if (character == static_cast<Py_UCS4>(-1) || character < 0x20 ||
            (character >= 0x7f && character <= 0x9f))
            return false;
    }
    text.assign(utf8, static_cast<std::size_t>(length));
    return true;
}

/// \brief Reads an identity without truncating the source or accepting aliases.
bool read_identity(PyObject *object, const char *name, std::size_t capacity,
                   std::string &text) {
    const auto item = field(object, name);
    return item && decode_identity(item.get(), capacity, text);
}

/// \brief Validates caller identity bytes through the same strict UTF-8 boundary.
bool valid_identity(const std::string &text, std::size_t capacity) {
    if (text.empty() || text.size() >= capacity)
        return false;
    const PyRef decoded(PyUnicode_DecodeUTF8(text.data(),
                                            static_cast<Py_ssize_t>(text.size()),
                                            "strict"));
    std::string canonical;
    return decoded && decode_identity(decoded.get(), capacity, canonical) &&
           canonical == text;
}

/// \brief Calls a runtime module function, optionally with one exact symbol name.
PyRef call(PyObject *module, const char *name, PyObject *symbol = nullptr) {
    if (PyErr_Occurred())
        return PyRef();
    const PyRef method(PyObject_GetAttrString(module, name));
    if (!method || !PyCallable_Check(method.get()))
        return PyRef();
    return PyRef(symbol ? PyObject_CallFunctionObjArgs(method.get(), symbol, nullptr)
                        : PyObject_CallNoArgs(method.get()));
}

/// \brief Contains only the terminal permissions needed by this collector.
struct TerminalPermissions {
    bool connected = false;
    bool allowed = false;
    bool api_disabled = true;
};

/// \brief Decodes terminal flags without inferring one flag from another.
bool decode_terminal(PyObject *object, TerminalPermissions &permissions) {
    return read_bool(object, "connected", permissions.connected) &&
           read_bool(object, "trade_allowed", permissions.allowed) &&
           read_bool(object, "tradeapi_disabled", permissions.api_disabled);
}

/// \brief Strictly decodes account identity, margin/trade mode and permissions.
bool decode_account(PyObject *object, Mt5AccountInfo &account) {
    std::string server;
    std::uint64_t margin_mode = 0;
    std::uint64_t trade_mode = 0;
    bool allowed = false;
    bool expert = false;
    if (!read_identity(object, "server", sizeof(account.server), server) ||
        !read_unsigned(object, "login", account.login) || account.login == 0 ||
        !read_unsigned(object, "margin_mode", margin_mode) || margin_mode > 2 ||
        !read_unsigned(object, "trade_mode", trade_mode) || trade_mode > 2 ||
        !read_bool(object, "trade_allowed", allowed) ||
        !read_bool(object, "trade_expert", expert))
        return false;
    std::memcpy(account.server, server.data(), server.size());
    account.margin_mode = static_cast<std::int32_t>(margin_mode);
    account.trade_mode = static_cast<std::int32_t>(trade_mode);
    account.trade_allowed = allowed ? 1 : 0;
    account.trade_expert = expert ? 1 : 0;
    account.known_fields = MT5BRIDGE_ACCOUNT_KNOWN_SERVER |
                           MT5BRIDGE_ACCOUNT_KNOWN_LOGIN |
                           MT5BRIDGE_ACCOUNT_KNOWN_MARGIN_MODE |
                           MT5BRIDGE_ACCOUNT_KNOWN_TRADE_MODE |
                           MT5BRIDGE_ACCOUNT_KNOWN_TRADE_ALLOWED |
                           MT5BRIDGE_ACCOUNT_KNOWN_TRADE_EXPERT;
    return true;
}

/// \brief Strictly decodes documented symbol modes and complete volume limits.
bool decode_symbol(PyObject *object, const std::string &requested,
                   Mt5SymbolCapabilities &symbol) {
    std::string name;
    std::uint64_t trade_mode = 0;
    std::uint64_t execution = 0;
    std::uint64_t order_modes = 0;
    std::uint64_t filling_modes = 0;
    if (!read_identity(object, "name", sizeof(symbol.symbol), name) ||
        name != requested || !read_unsigned(object, "trade_mode", trade_mode) ||
        trade_mode > 4 || !read_unsigned(object, "trade_exemode", execution) ||
        execution > 3 || !read_unsigned(object, "order_mode", order_modes) ||
        (order_modes & ~kKnownOrderModes) != 0 ||
        !read_unsigned(object, "filling_mode", filling_modes) ||
        (filling_modes & ~kKnownFillingModes) != 0 ||
        !read_number(object, "volume_min", symbol.volume_min) ||
        !read_number(object, "volume_max", symbol.volume_max) ||
        !read_number(object, "volume_step", symbol.volume_step) ||
        !read_number(object, "volume_limit", symbol.volume_limit) ||
        symbol.volume_min <= 0 || symbol.volume_max < symbol.volume_min ||
        symbol.volume_step <= 0 || std::signbit(symbol.volume_limit))
        return false;
    std::memcpy(symbol.symbol, name.data(), name.size());
    symbol.trade_mode = static_cast<std::uint32_t>(trade_mode);
    symbol.trade_exemode = static_cast<std::uint32_t>(execution);
    symbol.order_mode = static_cast<std::uint32_t>(order_modes);
    symbol.filling_mode = static_cast<std::uint32_t>(filling_modes);
    symbol.known_fields = MT5BRIDGE_SYMBOL_KNOWN_TRADE_MODE |
                          MT5BRIDGE_SYMBOL_KNOWN_TRADE_EXEMODE |
                          MT5BRIDGE_SYMBOL_KNOWN_ORDER_MODE |
                          MT5BRIDGE_SYMBOL_KNOWN_FILLING_MODE |
                          MT5BRIDGE_SYMBOL_KNOWN_VOLUME_LIMITS;
    return true;
}

/// \brief Captures the account/symbol/terminal bracket around complete inventory.
struct Capabilities {
    Mt5AccountInfo account{};
    Mt5SymbolCapabilities symbol{};
    TerminalPermissions terminal;
};

/// \brief Reads one live bracket and enforces account identity and permissions.
Status read_capabilities(PyObject *module, PyObject *symbol_name,
                         const AccountKey &expected, const std::string &requested,
                         Capabilities &capabilities, bool trailing_account = false) {
    PyRef account;
    if (!trailing_account)
        account = call(module, "account_info");
    const auto terminal = call(module, "terminal_info");
    const auto symbol = call(module, "symbol_info", symbol_name);
    // The closing account check must follow every other MT5 observation.
    if (trailing_account)
        account = call(module, "account_info");
    if (!account || !terminal || !symbol)
        return Status::python_error;
    if (account.get() == Py_None || terminal.get() == Py_None || symbol.get() == Py_None)
        return Status::incomplete_observation;
    if (!decode_account(account.get(), capabilities.account) ||
        !decode_terminal(terminal.get(), capabilities.terminal) ||
        !decode_symbol(symbol.get(), requested, capabilities.symbol))
        return Status::invalid_capabilities;
    if (capabilities.account.login != expected.login ||
        std::string(capabilities.account.server) != expected.server)
        return Status::account_mismatch;
    if (!capabilities.account.trade_allowed || !capabilities.account.trade_expert ||
        !capabilities.terminal.connected || !capabilities.terminal.allowed ||
        capabilities.terminal.api_disabled)
        return Status::permission_denied;
    return Status::collected;
}

/// \brief Compares all decoded account/symbol facts without examining POD padding.
bool same_capabilities(const Capabilities &first, const Capabilities &second) {
    const auto &left = first.account;
    const auto &right = second.account;
    const auto &a = first.symbol;
    const auto &b = second.symbol;
    return std::string(left.server) == right.server && left.login == right.login &&
           left.margin_mode == right.margin_mode && left.trade_mode == right.trade_mode &&
           left.trade_allowed == right.trade_allowed && left.trade_expert == right.trade_expert &&
           std::string(a.symbol) == b.symbol && a.trade_mode == b.trade_mode &&
           a.trade_exemode == b.trade_exemode && a.order_mode == b.order_mode &&
           a.filling_mode == b.filling_mode && a.volume_min == b.volume_min &&
           a.volume_max == b.volume_max && a.volume_step == b.volume_step &&
           a.volume_limit == b.volume_limit &&
           first.terminal.connected == second.terminal.connected &&
           first.terminal.allowed == second.terminal.allowed &&
           first.terminal.api_disabled == second.terminal.api_disabled;
}

/// \brief Captures stable identity and exposure fields of one unfiltered row.
struct ExposureRow {
    std::uint64_t ticket = 0;
    std::uint64_t identifier = 0;
    std::uint64_t type = 0;
    std::uint64_t state = 0;
    std::string symbol;
    double volume = 0;

    /// \brief Compares complete collected row fields, not merely set cardinality.
    bool operator==(const ExposureRow &other) const {
        return std::tie(ticket, identifier, type, state, symbol, volume) ==
               std::tie(other.ticket, other.identifier, other.type, other.state,
                        other.symbol, other.volume);
    }
};

/// \brief Decodes unfiltered position/order rows and rejects duplicate identities.
bool decode_rows(PyObject *object, bool positions, std::uint64_t total,
                 std::vector<ExposureRow> &rows) {
    if (!object || (!PyTuple_CheckExact(object) && !PyList_CheckExact(object)))
        return false;
    const auto size = PySequence_Size(object);
    if (size < 0 || static_cast<std::uint64_t>(size) != total || total > kMaximumRows)
        return false;
    rows.reserve(static_cast<std::size_t>(size));
    for (Py_ssize_t index = 0; index != size; ++index) {
        const PyRef item(PySequence_GetItem(object, index));
        ExposureRow row;
        if (!item || !read_unsigned(item.get(), "ticket", row.ticket) ||
            row.ticket == 0 || !read_unsigned(item.get(), "type", row.type) ||
            row.type > (positions ? 1U : 7U) ||
            !read_identity(item.get(), "symbol", sizeof(Mt5SymbolCapabilities{}.symbol),
                           row.symbol) ||
            !read_number(item.get(), positions ? "volume" : "volume_current", row.volume) ||
            row.volume <= 0)
            return false;
        if (positions) {
            if (!read_unsigned(item.get(), "identifier", row.identifier) ||
                row.identifier == 0)
                return false;
        } else {
            if (!read_unsigned(item.get(), "state", row.state) || row.state > 9 ||
                row.state == 2 || row.state == 4 || row.state == 5 || row.state == 6)
                return false;
        }
        rows.push_back(std::move(row));
    }
    std::sort(rows.begin(), rows.end(), [](const auto &left, const auto &right) {
        return left.ticket < right.ticket;
    });
    for (std::size_t index = 1; index < rows.size(); ++index)
        if (rows[index - 1].ticket == rows[index].ticket)
            return false;
    if (positions) {
        std::vector<std::uint64_t> identifiers;
        identifiers.reserve(rows.size());
        for (const auto &row : rows)
            identifiers.push_back(row.identifier);
        std::sort(identifiers.begin(), identifiers.end());
        if (std::adjacent_find(identifiers.begin(), identifiers.end()) != identifiers.end())
            return false;
    }
    return true;
}

/// \brief Reads an exact bounded account-wide total from the runtime module.
std::optional<std::uint64_t> read_total(PyObject *module, const char *name) {
    const auto result = call(module, name);
    if (!result || !PyLong_CheckExact(result.get()))
        return std::nullopt;
    const auto count = PyLong_AsUnsignedLongLong(result.get());
    if (PyErr_Occurred() || count > kMaximumRows)
        return std::nullopt;
    return static_cast<std::uint64_t>(count);
}

/// \brief Reads full unfiltered inventories bracketed by both account-wide totals.
Status read_inventory(PyObject *module, std::vector<ExposureRow> &positions,
                      std::vector<ExposureRow> &orders) {
    const auto positions_before = read_total(module, "positions_total");
    const auto orders_before = read_total(module, "orders_total");
    if (!positions_before || !orders_before)
        return PyErr_Occurred() ? Status::python_error : Status::incomplete_observation;
    const auto position_rows = call(module, "positions_get");
    const auto order_rows = call(module, "orders_get");
    if (!position_rows || !order_rows)
        return Status::python_error;
    if (!decode_rows(position_rows.get(), true, *positions_before, positions) ||
        !decode_rows(order_rows.get(), false, *orders_before, orders))
        return Status::incomplete_observation;
    const auto positions_after = read_total(module, "positions_total");
    const auto orders_after = read_total(module, "orders_total");
    if (!positions_after || !orders_after)
        return PyErr_Occurred() ? Status::python_error : Status::incomplete_observation;
    return positions_before == positions_after && orders_before == orders_after
               ? Status::collected
               : Status::unstable_observation;
}

/// \brief Uses B2's exact rounding-interval search for one decimal numerator.
std::optional<std::uint64_t> decimal_units(double raw, std::uint32_t scale) {
    const auto interval = dispatch::detail::opening_quantity_rounding_interval(raw);
    if (!interval)
        return std::nullopt;
    const auto units = dispatch::detail::first_opening_quantity_step_above(
        interval->lower, 1, scale, interval->lower_inclusive,
        (std::numeric_limits<std::uint64_t>::max)());
    return units && dispatch::detail::opening_quantity_decimal_is_unique(raw, *units, scale)
               ? units
               : std::nullopt;
}

/// \brief Chooses the smallest representable decimal scale for the reported step.
std::optional<BrokerVolumeNormalization> derive_normalization(double step) {
    for (std::uint32_t scale = 0; scale <= 9; ++scale)
        if (const auto units = decimal_units(step, scale))
            return BrokerVolumeNormalization{scale, *units};
    return std::nullopt;
}

/// \brief Reuses B2 to normalize one positive broker-step-aligned observation.
std::optional<std::uint64_t> normalize(double raw, double step,
                                      const BrokerVolumeNormalization &normalization) {
    const auto result = dispatch::derive_managed_opening_quantity(
        raw, step, {normalization.scale, normalization.step_units, 1});
    return result ? std::optional<std::uint64_t>(result->broker_units()) : std::nullopt;
}

/// \brief Accumulates symbol exposures with exact step conversion and overflow checks.
bool accumulate(const std::vector<ExposureRow> &rows, bool positions,
                const std::string &symbol, ManagedOpenCapabilityData &data) {
    for (const auto &row : rows) {
        if (row.symbol != symbol)
            continue;
        const auto volume = normalize(row.volume, data.symbol.volume_step, data.normalization);
        if (!volume)
            return false;
        auto &units = row.type % 2 == 0 ? data.buy_exposure_units : data.sell_exposure_units;
        if (*volume > (std::numeric_limits<std::uint64_t>::max)() - units)
            return false;
        units += *volume;
        if (positions)
            ++data.symbol_position_count;
        else
            ++data.symbol_order_count;
    }
    return true;
}

} // namespace

ManagedOpenReadResult read_managed_open_capabilities(
    _object *mt5, const AccountKey &expected, const std::string &symbol) {
    if (!mt5)
        return failure(Status::runtime_unavailable);
    try {
        if (!expected.valid() ||
            !valid_identity(expected.server, sizeof(Mt5AccountInfo{}.server)) ||
            !valid_identity(symbol, sizeof(Mt5SymbolCapabilities{}.symbol)))
            return failure(Status::invalid_request);
        const PyRef name(PyUnicode_DecodeUTF8(symbol.data(),
                                              static_cast<Py_ssize_t>(symbol.size()),
                                              "strict"));
        if (!name)
            return failure(Status::invalid_request);
        Capabilities before;
        auto status = read_capabilities(mt5, name.get(), expected, symbol, before);
        if (status != Status::collected)
            return failure(status);
        std::vector<ExposureRow> first_positions;
        std::vector<ExposureRow> first_orders;
        status = read_inventory(mt5, first_positions, first_orders);
        if (status != Status::collected)
            return failure(status);
        std::vector<ExposureRow> second_positions;
        std::vector<ExposureRow> second_orders;
        status = read_inventory(mt5, second_positions, second_orders);
        if (status != Status::collected)
            return failure(status);
        Capabilities after;
        status = read_capabilities(mt5, name.get(), expected, symbol, after, true);
        if (status != Status::collected)
            return failure(status);
        if (!same_capabilities(before, after) || first_positions != second_positions ||
            first_orders != second_orders)
            return failure(Status::unstable_observation);
        ManagedOpenCapabilityData data;
        data.account = before.account;
        data.symbol = before.symbol;
        const auto normalization = derive_normalization(data.symbol.volume_step);
        if (!normalization)
            return failure(Status::invalid_volume);
        data.normalization = *normalization;
        const auto minimum = normalize(data.symbol.volume_min, data.symbol.volume_step,
                                       data.normalization);
        const auto maximum = normalize(data.symbol.volume_max, data.symbol.volume_step,
                                       data.normalization);
        const auto limit = data.symbol.volume_limit == 0
                               ? std::optional<std::uint64_t>(0)
                               : normalize(data.symbol.volume_limit, data.symbol.volume_step,
                                           data.normalization);
        if (!minimum || !maximum || !limit || *maximum < *minimum)
            return failure(Status::invalid_volume);
        data.minimum_units = *minimum;
        data.maximum_units = *maximum;
        data.directional_limit_units = *limit;
        data.account_position_count = first_positions.size();
        data.account_order_count = first_orders.size();
        if (!accumulate(first_positions, true, symbol, data) ||
            !accumulate(first_orders, false, symbol, data))
            return failure(Status::invalid_volume);
        return {Status::collected, std::move(data)};
    } catch (...) {
        return failure(Status::python_error);
    }
}

} // namespace mt5bridge::trade::detail
