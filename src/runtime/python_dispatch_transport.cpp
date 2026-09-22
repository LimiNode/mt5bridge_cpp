/// \file python_dispatch_transport.cpp
/// \brief Implements the private embedded-Python dispatch transport.

#ifndef PY_SSIZE_T_CLEAN
#define PY_SSIZE_T_CLEAN
#endif
#include <Python.h>

#include "python_dispatch_transport.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace mt5bridge::runtime {
namespace {

/// \class PyRef
/// \brief Owns one strong CPython reference inside the adapter.
class PyRef {
public:
    /// \brief Takes ownership of a new Python reference.
    /// \param object Owned Python object; nullptr is permitted.
    explicit PyRef(PyObject *object = nullptr) : object_(object) {}

    /// \brief Releases the owned Python reference.
    ~PyRef() { Py_XDECREF(object_); }

    /// \brief Strong Python references are not copyable.
    PyRef(const PyRef &) = delete;

    /// \brief Strong Python references are not copy-assignable.
    PyRef &operator=(const PyRef &) = delete;

    /// \brief Transfers ownership from another reference wrapper.
    /// \param other Wrapper whose reference is transferred.
    PyRef(PyRef &&other) noexcept : object_(other.release()) {}

    /// \brief Replaces the owned reference by moving from another wrapper.
    /// \param other Wrapper whose reference is transferred.
    /// \return Reference to this wrapper.
    PyRef &operator=(PyRef &&other) noexcept {
        if (this != &other) {
            Py_XDECREF(object_);
            object_ = other.release();
        }
        return *this;
    }
    /// \brief Returns the borrowed Python object pointer.
    /// \return Borrowed pointer, or nullptr when empty.
    PyObject *get() const { return object_; }

    /// \brief Relinquishes ownership without decrementing the reference count.
    /// \return Previously owned new reference, or nullptr.
    PyObject *release() {
        PyObject *result = object_;
        object_ = nullptr;
        return result;
    }
    /// \brief Tests whether this wrapper owns a reference.
    /// \return True when the stored pointer is non-null.
    explicit operator bool() const { return object_ != nullptr; }

private:
    PyObject *object_ = nullptr;
};

/// \brief Reads one dictionary item or object attribute as a new reference.
/// \param object Borrowed Python object.
/// \param name Field name to read.
/// \return Owned field reference, or empty when the field is absent.
PyRef field(PyObject *object, const char *name) {
    if (!object)
        return PyRef();
    if (PyDict_Check(object)) {
        PyObject *value = PyDict_GetItemString(object, name);
        if (!value)
            return PyRef();
        Py_INCREF(value);
        return PyRef(value);
    }
    PyObject *value = PyObject_GetAttrString(object, name);
    if (!value)
        PyErr_Clear();
    return PyRef(value);
}

/// \brief Reads one required non-negative integer field.
/// \param object Borrowed Python result object.
/// \param name Field name to read.
/// \param[out] value Receives the converted integer.
/// \return True when the field is present and representable as uint64.
bool read_uint64(PyObject *object, const char *name, std::uint64_t *value) {
    PyRef item(field(object, name));
    if (!item || item.get() == Py_None || !PyLong_Check(item.get())) {
        PyErr_Clear();
        return false;
    }
    const unsigned long long converted = PyLong_AsUnsignedLongLong(item.get());
    if (PyErr_Occurred()) {
        PyErr_Clear();
        return false;
    }
    *value = static_cast<std::uint64_t>(converted);
    return true;
}

/// \brief Reads one signed 32-bit integer field.
/// \param object Borrowed Python result object.
/// \param name Field name to read.
/// \param[out] value Receives the converted integer.
/// \return True when the field is present and representable as int32.
bool read_int32(PyObject *object, const char *name, std::int32_t *value) {
    PyRef item(field(object, name));
    if (!item || item.get() == Py_None || !PyLong_Check(item.get())) {
        PyErr_Clear();
        return false;
    }
    const long long converted = PyLong_AsLongLong(item.get());
    if (PyErr_Occurred() ||
        converted < (std::numeric_limits<std::int32_t>::min)() ||
        converted > (std::numeric_limits<std::int32_t>::max)()) {
        PyErr_Clear();
        return false;
    }
    *value = static_cast<std::int32_t>(converted);
    return true;
}

/// \brief Validates one required finite numeric field.
/// \param object Borrowed Python result object.
/// \param name Field name to read.
/// \return True when the field is a finite Python float or integer.
bool read_double(PyObject *object, const char *name) {
    PyRef item(field(object, name));
    if (!item || item.get() == Py_None ||
        (!PyFloat_Check(item.get()) && !PyLong_Check(item.get()))) {
        PyErr_Clear();
        return false;
    }
    const double converted = PyFloat_AsDouble(item.get());
    if (PyErr_Occurred() || !std::isfinite(converted)) {
        PyErr_Clear();
        return false;
    }
    return true;
}

/// \brief Validates one required UTF-8 text field.
/// \param object Borrowed Python result object.
/// \param name Field name to read.
/// \return True when the field is a valid Python Unicode value.
bool read_text(PyObject *object, const char *name) {
    PyRef item(field(object, name));
    if (!item || item.get() == Py_None || !PyUnicode_Check(item.get())) {
        PyErr_Clear();
        return false;
    }
    return PyUnicode_AsUTF8(item.get()) != nullptr;
}

/// \brief Decodes `account_info()` into the immutable journal account key.
/// \param object Borrowed account-info object.
/// \return Complete account identity, or empty for malformed data.
std::optional<AccountKey> decode_account(PyObject *object) {
    PyRef server(field(object, "server"));
    std::uint64_t login = 0;
    if (!server || server.get() == Py_None || !PyUnicode_Check(server.get()) ||
        !read_uint64(object, "login", &login)) {
        PyErr_Clear();
        return std::nullopt;
    }
    const char *server_text = PyUnicode_AsUTF8(server.get());
    if (!server_text) {
        PyErr_Clear();
        return std::nullopt;
    }
    AccountKey account{server_text, login};
    return account.valid() ? std::optional<AccountKey>(std::move(account)) : std::nullopt;
}

/// \brief Clears any Python error and returns an unresolved transport failure.
/// \return Failure result without broker retcode or raw payload.
BackendCallResult transport_failure() {
    PyErr_Clear();
    return {BackendCallStatus::transport_failure,
            BrokerResultDisposition::reconciling, 0, {}};
}

/// \brief Clears any Python error and reports a final-account mismatch.
/// \return Account-mismatch result without broker evidence.
BackendCallResult account_mismatch() {
    PyErr_Clear();
    return {BackendCallStatus::account_mismatch,
            BrokerResultDisposition::reconciling, 0, {}};
}

/// \brief Tests whether an MT5 retcode is a definitive broker rejection.
/// \param retcode MqlTradeResult return code.
/// \return True when the operation is known not to require fill reconciliation.
bool deterministic_rejection(std::uint32_t retcode) {
    switch (retcode) {
    case 10006: // TRADE_RETCODE_REJECT
    case 10007: // TRADE_RETCODE_CANCEL
    case 10011: // TRADE_RETCODE_ERROR
    case 10013: // TRADE_RETCODE_INVALID
    case 10014: // TRADE_RETCODE_INVALID_VOLUME
    case 10015: // TRADE_RETCODE_INVALID_PRICE
    case 10016: // TRADE_RETCODE_INVALID_STOPS
    case 10017: // TRADE_RETCODE_TRADE_DISABLED
    case 10018: // TRADE_RETCODE_MARKET_CLOSED
    case 10019: // TRADE_RETCODE_NO_MONEY
    case 10020: // TRADE_RETCODE_PRICE_CHANGED
    case 10021: // TRADE_RETCODE_PRICE_OFF
    case 10022: // TRADE_RETCODE_INVALID_EXPIRATION
    case 10023: // TRADE_RETCODE_ORDER_CHANGED
    case 10024: // TRADE_RETCODE_TOO_MANY_REQUESTS
    case 10025: // TRADE_RETCODE_NO_CHANGES
    case 10026: // TRADE_RETCODE_SERVER_DISABLES_AT
    case 10027: // TRADE_RETCODE_CLIENT_DISABLES_AT
    case 10029: // TRADE_RETCODE_FROZEN
    case 10030: // TRADE_RETCODE_INVALID_FILL
    case 10032: // TRADE_RETCODE_ONLY_REAL
    case 10033: // TRADE_RETCODE_LIMIT_ORDERS
    case 10034: // TRADE_RETCODE_LIMIT_VOLUME
    case 10035: // TRADE_RETCODE_INVALID_ORDER
    case 10036: // TRADE_RETCODE_POSITION_CLOSED
    case 10038: // TRADE_RETCODE_INVALID_CLOSE_VOLUME
    case 10039: // TRADE_RETCODE_CLOSE_ORDER_EXIST
    case 10040: // TRADE_RETCODE_LIMIT_POSITIONS
    case 10041: // TRADE_RETCODE_REJECT_CANCEL
    case 10042: // TRADE_RETCODE_LONG_ONLY
    case 10043: // TRADE_RETCODE_SHORT_ONLY
    case 10044: // TRADE_RETCODE_CLOSE_ONLY
    case 10045: // TRADE_RETCODE_FIFO_CLOSE
    case 10046: // TRADE_RETCODE_HEDGE_PROHIBITED
        return true;
    default:
        return false;
    }
}

/// \brief Validates the durable subset of one `MqlTradeResult`.
/// \param result Borrowed result dictionary or namedtuple.
/// \param[out] retcode Receives the validated return code.
/// \return True only when every required result field is well-typed.
bool validate_result(PyObject *result, std::uint32_t *retcode) {
    std::uint64_t converted_retcode = 0;
    std::uint64_t ignored = 0;
    std::int32_t ignored_external = 0;
    if (!read_uint64(result, "retcode", &converted_retcode) ||
        converted_retcode > (std::numeric_limits<std::uint32_t>::max)() ||
        !read_int32(result, "retcode_external", &ignored_external) ||
        !read_uint64(result, "request_id", &ignored) ||
        !read_uint64(result, "order", &ignored) ||
        !read_uint64(result, "deal", &ignored) ||
        !read_double(result, "volume") || !read_double(result, "price") ||
        !read_double(result, "bid") || !read_double(result, "ask") ||
        !read_text(result, "comment"))
        return false;
    *retcode = static_cast<std::uint32_t>(converted_retcode);
    return true;
}

/// \brief Converts a result dictionary or namedtuple to a JSON-ready mapping.
/// \param result Borrowed result object.
/// \return Owned dictionary reference, or empty for unsupported results.
PyRef json_compatible(PyObject *value);

PyRef json_compatible(PyObject *value) {
    PyRef asdict(field(value, "_asdict"));
    if (asdict && PyCallable_Check(asdict.get())) {
        PyRef mapped(PyObject_CallObject(asdict.get(), nullptr));
        if (!mapped)
            return PyRef();
        return json_compatible(mapped.get());
    }
    if (PyDict_Check(value)) {
        PyRef normalized(PyDict_New());
        if (!normalized)
            return PyRef();
        PyObject *key = nullptr;
        PyObject *item = nullptr;
        Py_ssize_t position = 0;
        while (PyDict_Next(value, &position, &key, &item)) {
            PyRef converted(json_compatible(item));
            if (!converted || PyDict_SetItem(normalized.get(), key, converted.get()) != 0)
                return PyRef();
        }
        return normalized;
    }
    if (PyTuple_Check(value) || PyList_Check(value)) {
        const Py_ssize_t size = PySequence_Size(value);
        if (size < 0)
            return PyRef();
        PyRef normalized(PyList_New(size));
        if (!normalized)
            return PyRef();
        for (Py_ssize_t index = 0; index < size; ++index) {
            PyObject *item = PySequence_GetItem(value, index);
            PyRef converted(json_compatible(item));
            Py_XDECREF(item);
            if (!converted || PyList_SetItem(normalized.get(), index, converted.release()) != 0)
                return PyRef();
        }
        return normalized;
    }
    Py_INCREF(value);
    return PyRef(value);
}

/// \brief Serializes the complete validated result as UTF-8 JSON.
/// \param result Borrowed result object.
/// \return Opaque bytes, or empty when conversion/serialization fails.
std::optional<std::vector<std::uint8_t>> serialize_result(PyObject *result) {
    PyRef compatible(json_compatible(result));
    if (!compatible || !PyDict_Check(compatible.get())) {
        PyErr_Clear();
        return std::nullopt;
    }
    PyRef json_module(PyImport_ImportModule("json"));
    PyRef dumps(json_module ? PyObject_GetAttrString(json_module.get(), "dumps") : nullptr);
    if (!json_module || !dumps || !PyCallable_Check(dumps.get())) {
        PyErr_Clear();
        return std::nullopt;
    }
    PyRef encoded(PyObject_CallFunctionObjArgs(dumps.get(), compatible.get(), nullptr));
    if (!encoded || !PyUnicode_Check(encoded.get())) {
        PyErr_Clear();
        return std::nullopt;
    }
    const char *text = PyUnicode_AsUTF8(encoded.get());
    if (!text) {
        PyErr_Clear();
        return std::nullopt;
    }
    return std::vector<std::uint8_t>(text, text + std::char_traits<char>::length(text));
}

} // namespace

std::optional<AccountKey> Mt5PythonAccountProbe::current_account() {
    if (!mt5_)
        return std::nullopt;
    PyRef snapshot(PyObject_CallMethod(reinterpret_cast<PyObject *>(mt5_), "account_info",
                                       nullptr));
    if (!snapshot)
        return std::nullopt;
    return decode_account(snapshot.get());
}

BackendCallResult Mt5PythonDispatchTransport::submit_once(const OperationRecord &record) {
    if (!mt5_ || !record.key.valid() || record.request_payload.empty() ||
        record.request_payload.size() > static_cast<std::size_t>(PY_SSIZE_T_MAX))
        return transport_failure();

    try {
        PyRef json_module(PyImport_ImportModule("json"));
        PyRef loads(json_module ? PyObject_GetAttrString(json_module.get(), "loads") : nullptr);
        if (!json_module || !loads || !PyCallable_Check(loads.get()))
            return transport_failure();
        PyRef request(PyObject_CallFunction(
            loads.get(), "s#",
            reinterpret_cast<const char *>(record.request_payload.data()),
            static_cast<Py_ssize_t>(record.request_payload.size())));
        if (!request || !PyDict_Check(request.get()))
            return transport_failure();

        PyRef order_send(PyObject_GetAttrString(reinterpret_cast<PyObject *>(mt5_), "order_send"));
        if (!order_send || !PyCallable_Check(order_send.get()))
            return transport_failure();

        const auto current = account_probe_.current_account();
        if (!current || *current != record.key.account)
            return account_mismatch();

        PyRef result(PyObject_CallFunctionObjArgs(order_send.get(), request.get(), nullptr));
        if (!result || result.get() == Py_None)
            return transport_failure();

        std::uint32_t retcode = 0;
        if (!validate_result(result.get(), &retcode))
            return transport_failure();
        const auto raw = serialize_result(result.get());
        if (!raw || raw->empty())
            return transport_failure();
        return {BackendCallStatus::broker_result,
                deterministic_rejection(retcode) ? BrokerResultDisposition::rejected
                                                 : BrokerResultDisposition::reconciling,
                retcode, *raw};
    } catch (...) {
        return transport_failure();
    }
}

} // namespace mt5bridge::runtime
