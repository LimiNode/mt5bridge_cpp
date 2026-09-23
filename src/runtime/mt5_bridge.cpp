/// \file mt5_bridge.cpp
/// \brief Implements the CPython-backed mt5_bridge.dll runtime.

#include <mt5bridge/abi.h>
#include <mt5bridge/market/data.h>
#include <mt5bridge/trade/observation.h>
#include "python_dispatch_transport.hpp"
#include "runtime_lane.hpp"

#ifndef PY_SSIZE_T_CLEAN
#define PY_SSIZE_T_CLEAN
#endif
#include <Python.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <deque>
#include <utility>
#include <vector>

/// \struct Mt5TickBuffer
/// \brief Owns a contiguous tick result and its diagnostic snapshot inside the DLL.
struct Mt5TickBuffer {
    std::vector<Mt5Tick> values;       ///< Tick values exposed through the C accessors.
    Mt5FetchDiagnostics diagnostics{}; ///< Recovery diagnostics for the completed query.
};

/// \struct Mt5RateBuffer
/// \brief Owns a contiguous rate result and its diagnostic snapshot inside the DLL.
struct Mt5RateBuffer {
    std::vector<Mt5Rate> values;       ///< Rate values exposed through the C accessors.
    Mt5FetchDiagnostics diagnostics{}; ///< Recovery diagnostics for the completed query.
    Mt5RateCoverageV1 coverage{};      ///< Immutable V1 range-coverage evidence.
};

/// \struct Mt5OrderBuffer
/// \brief Owns an active-order snapshot inside the DLL.
struct Mt5OrderBuffer { std::vector<Mt5OrderSnapshot> values; };

/// \struct Mt5PositionBuffer
/// \brief Owns an active-position snapshot inside the DLL.
struct Mt5PositionBuffer { std::vector<Mt5PositionSnapshot> values; };

/// \struct Mt5HistoryOrderBuffer
/// \brief Owns a history-order snapshot inside the DLL.
struct Mt5HistoryOrderBuffer { std::vector<Mt5HistoryOrderSnapshot> values; };

/// \struct Mt5DealBuffer
/// \brief Owns a history-deal snapshot inside the DLL.
struct Mt5DealBuffer { std::vector<Mt5DealSnapshot> values; };

namespace {

using mt5bridge::runtime::RuntimeCallLane;
using mt5bridge::runtime::RuntimeLane;

std::mutex g_mutex;
// Serializes calls into the embedded interpreter without extending the state mutex
// across potentially slow MT5 IPC operations.
std::mutex g_python_mutex;
RuntimeLane g_runtime_lane;
// Tracks admitted calls so shutdown can wait for them without extending the
// lifecycle mutex across potentially slow MT5 IPC operations.
std::size_t g_active_runtime_calls = 0;
std::condition_variable g_runtime_calls_cv;
bool g_initialized = false;
enum class RuntimeState { stopped, running, shutting_down };
RuntimeState g_runtime_state = RuntimeState::stopped;
bool g_owns_interpreter = false;
PyThreadState *g_main_thread_state = nullptr;
std::thread::id g_owner_thread;
thread_local std::string g_last_error;
thread_local Mt5FetchDiagnostics g_last_fetch_diagnostics{};
thread_local bool g_last_fetch_fatal_error = false;

/// \struct RealtimeBatch
/// \brief Immutable tick batch retained by a physical source ring.
struct RealtimeBatch {
    uint64_t sequence = 0;
    std::vector<Mt5Tick> ticks;
};
/// \struct RealtimeSource
/// \brief Shared polling state for one symbol and tick filter.
struct RealtimeSource {
    std::string symbol;
    uint32_t flags = 0;
    uint32_t interval_ms = 250;
    uint32_t max_batch = 1024;
    uint32_t capacity = 64;
    int64_t initial_from_msc = -1;
    std::chrono::steady_clock::time_point next_poll{};
    uint64_t next_sequence = 1;
    int64_t cursor_time_msc = -1;
    int64_t overlap_ms = 1000;
    std::vector<Mt5Tick> overlap_snapshot;
    Mt5SubscriptionDiagnostics diagnostics{};
    uint32_t consecutive_failures = 0;
    std::chrono::steady_clock::time_point retry_at{};
    Mt5SubscriptionStatus status = MT5_SUBSCRIPTION_STARTING;
    std::deque<RealtimeBatch> ring;
    bool observed_valid = false;
    int64_t observed_cursor_time_msc = -1;
    std::vector<Mt5Tick> cursor_boundary;
    std::vector<Mt5Tick> observed_boundary;
    bool recovery_from_committed = false;
    uint64_t inconsistency_epoch = 0;
    int64_t recovery_from_msc = -1;
    int64_t recovery_to_msc = -1;
};
/// \struct RealtimeSubscription
/// \brief Logical subscriber cursor over a shared source ring.
struct RealtimeMember {
    std::shared_ptr<RealtimeSource> source;
    uint64_t next_sequence = 1;
    Mt5SubscriptionStatus delivered_status = static_cast<Mt5SubscriptionStatus>(-1);
    uint32_t interval_ms = 250;
    uint32_t max_batch = 1024;
    uint32_t capacity = 64;
    uint64_t dropped_batches = 0;
    Mt5GapReason last_gap_reason = MT5_GAP_NONE;
    uint64_t delivered_inconsistency_epoch = 0;
};

/// \struct RealtimeSubscription
/// \brief Logical group containing one member per requested symbol source.
struct RealtimeSubscription {
    Mt5SubscriptionHandle handle{};
    std::vector<RealtimeMember> members;
    std::size_t member_cursor = 0;
};
std::unordered_map<std::string, std::shared_ptr<RealtimeSource>> g_realtime_sources;
std::unordered_map<uint64_t, RealtimeSubscription> g_realtime_subscriptions;
std::vector<uint64_t> g_subscription_order;
std::size_t g_rr_cursor = 0;
uint64_t g_next_subscription_id = 1;
uint64_t g_runtime_generation = 0;
std::thread g_poller;
std::condition_variable g_poller_cv;
bool g_poller_stop = false;


/// \brief Clears the calling thread's latest market-data diagnostics.
void clear_fetch_diagnostics() {
    g_last_fetch_diagnostics = Mt5FetchDiagnostics{};
    g_last_fetch_fatal_error = false;
}

/// \brief Marks the current history request as stopped by a non-recoverable error.
void mark_fetch_fatal() { g_last_fetch_fatal_error = true; }

/// \brief Saves market-data diagnostics for retrieval after a failed query.
void save_fetch_diagnostics(const Mt5FetchDiagnostics &diagnostics) {
    g_last_fetch_diagnostics = diagnostics;
}

/// \brief Replaces the calling thread's bridge diagnostic.
/// \param message Null-terminated message, or nullptr for a generic fallback.
void set_error(const char *message) { g_last_error = message ? message : "unknown error"; }

/// \brief Clears the calling thread's bridge diagnostic.
void clear_error() { g_last_error.clear(); }

/// \class RuntimeCallAdmission
/// \brief Admits one serialized Python/MT5 operation without holding g_mutex.
///
/// Admission validates the running state and increments the in-flight count
/// while holding the lifecycle mutex, then releases it before waiting for the
/// interpreter mutex. Shutdown closes admission and waits for this count to
/// reach zero before finalization.
class RuntimeCallAdmission {
public:
    explicit RuntimeCallAdmission(RuntimeCallLane lane = RuntimeCallLane::market_data)
        : lane_(lane), python_lock_(g_python_mutex, std::defer_lock) {
        {
            std::unique_lock<std::mutex> state_lock(g_mutex);
            if (g_runtime_state != RuntimeState::running) {
                set_error(g_runtime_state == RuntimeState::shutting_down
                              ? "bridge is shutting down"
                              : "bridge not initialized");
                return;
            }
            ++g_active_runtime_calls;
            counted_ = true;
        }
        try {
            lane_permit_ = g_runtime_lane.acquire(lane_);
            python_lock_.lock();
            acquired_ = true;
        } catch (...) {
            std::lock_guard<std::mutex> state_lock(g_mutex);
            --g_active_runtime_calls;
            counted_ = false;
            if (g_active_runtime_calls == 0)
                g_runtime_calls_cv.notify_all();
            throw;
        }
    }

    RuntimeCallAdmission(const RuntimeCallAdmission &) = delete;
    RuntimeCallAdmission &operator=(const RuntimeCallAdmission &) = delete;

    ~RuntimeCallAdmission() {
        if (python_lock_.owns_lock())
            python_lock_.unlock();
        if (counted_) {
            std::lock_guard<std::mutex> state_lock(g_mutex);
            --g_active_runtime_calls;
            if (g_active_runtime_calls == 0)
                g_runtime_calls_cv.notify_all();
        }
    }

    /// \brief Tests whether the operation was admitted while the runtime ran.
    /// \return True when the in-flight admission and interpreter mutex are held.
    explicit operator bool() const noexcept { return acquired_; }

private:
    RuntimeCallLane lane_;
    RuntimeLane::Permit lane_permit_;
    std::unique_lock<std::mutex> python_lock_;
    bool counted_ = false;
    bool acquired_ = false;
};

/// \brief Converts the active C++ exception into a non-throwing ABI diagnostic.
/// \note Call only while handling an exception at an exported C ABI boundary.
void set_current_exception_error() noexcept {
    try {
        throw;
    } catch (const std::exception &error) {
        try {
            set_error(error.what());
        } catch (...) {
        }
    } catch (...) {
        try {
            set_error("unhandled C++ exception");
        } catch (...) {
        }
    }
}

/// \brief Converts the active Python exception into the bridge diagnostic string.
void set_python_error() {
    PyObject *type = nullptr;
    PyObject *value = nullptr;
    PyObject *traceback = nullptr;
    PyErr_Fetch(&type, &value, &traceback);
    PyErr_NormalizeException(&type, &value, &traceback);
    PyObject *text = PyObject_Str(value ? value : Py_None);
    if (text) {
        const char *utf8 = PyUnicode_AsUTF8(text);
        set_error(utf8 ? utf8 : "unknown Python error");
        Py_DECREF(text);
    } else {
        set_error("unknown Python error");
    }
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(traceback);
}

/// \class PyRef
/// \brief Provides move-only ownership of a strong PyObject reference.
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
    PyObject *object_;
};

// Trade-observation collection helpers are defined later in the translation
// unit, after the generic market-data helpers.
PyRef make_datetime(int64_t milliseconds);

/// \class GilScope
/// \brief Acquires and releases the CPython global interpreter lock for one scope.
class GilScope {
public:
    /// \brief Optionally acquires the GIL for the calling thread.
    /// \param acquire True to acquire the GIL.
    explicit GilScope(bool acquire) : acquired_(acquire) {
        if (acquired_)
            state_ = PyGILState_Ensure();
    }
    /// \brief Releases the GIL when it is still owned by this scope.
    ~GilScope() {
        if (acquired_)
            PyGILState_Release(state_);
    }
    /// \brief Releases the GIL early and makes later releases harmless.
    void release() {
        if (acquired_) {
            PyGILState_Release(state_);
            acquired_ = false;
        }
    }

private:
    bool acquired_;
    PyGILState_STATE state_{};
};

/// \brief Converts common MetaTrader Python containers to JSON-compatible values.
/// \param value Borrowed Python value to inspect.
/// \return A new reference to a compatible value, or nullptr on failure.
PyObject *json_compatible(PyObject *value) {
    // MetaTrader returns namedtuples and NumPy arrays. Convert those two
    // common containers before handing the value to the standard JSON module.
    PyRef asdict(PyObject_GetAttrString(value, "_asdict"));
    if (asdict && PyCallable_Check(asdict.get())) {
        return PyObject_CallObject(asdict.get(), nullptr);
    }
    PyErr_Clear();

    PyRef tolist(PyObject_GetAttrString(value, "tolist"));
    if (tolist && PyCallable_Check(tolist.get())) {
        return PyObject_CallObject(tolist.get(), nullptr);
    }
    PyErr_Clear();
    Py_INCREF(value);
    return value;
}

/// \brief Reads a required UTF-8 string from a Python dictionary.
/// \param dict Borrowed request dictionary.
/// \param key Required field name.
/// \param[out] value Receives a borrowed UTF-8 pointer.
/// \return True when the field exists and is a valid Python string.
bool read_string(PyObject *dict, const char *key, const char **value) {
    PyObject *item = PyDict_GetItemString(dict, key);
    if (!item || !PyUnicode_Check(item)) {
        set_error(key);
        g_last_error += " must be a string";
        return false;
    }
    *value = PyUnicode_AsUTF8(item);
    if (!*value) {
        set_python_error();
        return false;
    }
    return true;
}

/// \brief Reads a required non-negative int from a Python dictionary.
/// \param dict Borrowed request dictionary.
/// \param key Required field name.
/// \param[out] value Receives the converted integer.
/// \return True when the field is present and in range.
bool read_int(PyObject *dict, const char *key, int *value) {
    PyObject *item = PyDict_GetItemString(dict, key);
    if (!item || !PyLong_Check(item)) {
        set_error(key);
        g_last_error += " must be an integer";
        return false;
    }
    const long number = PyLong_AsLong(item);
    if (PyErr_Occurred() || number < 0 || number > std::numeric_limits<int>::max()) {
        set_error("integer argument is out of range");
        PyErr_Clear();
        return false;
    }
    *value = static_cast<int>(number);
    return true;
}

/// \brief Reads a required floating-point value from a Python dictionary.
/// \param dict Borrowed request dictionary.
/// \param key Required field name.
/// \param[out] value Receives the converted number.
/// \return True when the field is numeric and conversion succeeds.
bool read_double(PyObject *dict, const char *key, double *value) {
    PyObject *item = PyDict_GetItemString(dict, key);
    if (!item || (!PyFloat_Check(item) && !PyLong_Check(item))) {
        set_error(key);
        g_last_error += " must be a number";
        return false;
    }
    *value = PyFloat_AsDouble(item);
    if (PyErr_Occurred()) {
        set_python_error();
        return false;
    }
    return true;
}

/// \brief Retrieves a named attribute or mapping value as an owned reference.
/// \param object Borrowed Python object returned by MetaTrader.
/// \param name Field name to retrieve.
/// \return Owned field reference, or nullptr when the field is absent.
PyRef trade_field(PyObject *object, const char *name) {
    if (PyDict_Check(object)) {
        PyObject *value = PyDict_GetItemString(object, name);
        if (!value)
            return PyRef();
        Py_INCREF(value);
        return PyRef(value);
    }
    return PyRef(PyObject_GetAttrString(object, name));
}

/// \brief Tests whether a trade record contains a non-None field.
/// \param object Borrowed Python trade record.
/// \param name Field name.
/// \return True when the field is present and carries a value.
bool trade_has_field(PyObject *object, const char *name) {
    PyRef field(trade_field(object, name));
    if (!field) {
        PyErr_Clear();
        return false;
    }
    return field.get() != Py_None;
}

/// \brief Reads an optional or required signed integer field from a trade record.
/// \param object Borrowed Python trade record.
/// \param name Field name.
/// \param[out] value Destination value.
/// \param required Whether absence or an invalid value is an error.
/// \return True when the field was copied or was optional and absent.
bool trade_int64(PyObject *object, const char *name, int64_t *value, bool required) {
    PyRef field(trade_field(object, name));
    if (!field) {
        PyErr_Clear();
        if (required) {
            set_error(name);
            g_last_error += " is required";
        }
        return !required;
    }
    if (field.get() == Py_None) {
        if (required) {
            set_error(name);
            g_last_error += " is required";
        }
        return !required;
    }
    if (!PyLong_Check(field.get())) {
        PyErr_Clear();
        set_error(name);
        g_last_error += " must be an integer";
        return false;
    }
    const long long converted = PyLong_AsLongLong(field.get());
    if (PyErr_Occurred()) {
        set_python_error();
        return false;
    }
    *value = static_cast<int64_t>(converted);
    return true;
}

/// \brief Reads an optional or required non-negative 64-bit integer field.
/// \param object Borrowed Python trade record.
/// \param name Field name.
/// \param[out] value Destination value.
/// \param required Whether absence or an invalid value is an error.
/// \return True when the field was copied or was optional and absent.
bool trade_uint64(PyObject *object, const char *name, uint64_t *value, bool required) {
    PyRef field(trade_field(object, name));
    if (!field) {
        PyErr_Clear();
        if (required) {
            set_error(name);
            g_last_error += " is required";
        }
        return !required;
    }
    if (field.get() == Py_None || !PyLong_Check(field.get())) {
        if (required) {
            set_error(name);
            g_last_error += field.get() == Py_None ? " is required" : " must be an integer";
        }
        PyErr_Clear();
        return !required;
    }
    const unsigned long long converted = PyLong_AsUnsignedLongLong(field.get());
    if (PyErr_Occurred()) {
        set_python_error();
        return false;
    }
    *value = static_cast<uint64_t>(converted);
    return true;
}

/// \brief Reads a signed 32-bit field with range validation.
/// \param object Borrowed Python trade record.
/// \param name Field name.
/// \param[out] value Destination value.
/// \param required Whether absence is an error.
/// \return True when the field was copied or was optional and absent.
bool trade_int32(PyObject *object, const char *name, int32_t *value, bool required) {
    int64_t converted = 0;
    if (!trade_int64(object, name, &converted, required))
        return false;
    if (converted < std::numeric_limits<int32_t>::min() ||
        converted > std::numeric_limits<int32_t>::max()) {
        set_error(name);
        g_last_error += " is out of range";
        return false;
    }
    *value = static_cast<int32_t>(converted);
    return true;
}

/// \brief Reads an unsigned 32-bit field with range validation.
/// \param object Borrowed Python trade record.
/// \param name Field name.
/// \param[out] value Destination value.
/// \param required Whether absence is an error.
/// \return True when the field was copied or was optional and absent.
bool trade_uint32(PyObject *object, const char *name, uint32_t *value, bool required) {
    int64_t converted = 0;
    if (!trade_int64(object, name, &converted, required))
        return false;
    if (converted < 0 || converted > std::numeric_limits<uint32_t>::max()) {
        set_error(name);
        g_last_error += " is out of range";
        return false;
    }
    *value = static_cast<uint32_t>(converted);
    return true;
}

/// \brief Reads an optional or required floating-point field from a trade record.
/// \param object Borrowed Python trade record.
/// \param name Field name.
/// \param[out] value Destination value.
/// \param required Whether absence or an invalid value is an error.
/// \return True when the field was copied or was optional and absent.
bool trade_double(PyObject *object, const char *name, double *value, bool required) {
    PyRef field(trade_field(object, name));
    if (!field) {
        PyErr_Clear();
        if (required) {
            set_error(name);
            g_last_error += " is required";
        }
        return !required;
    }
    if (field.get() == Py_None) {
        if (required) {
            set_error(name);
            g_last_error += " is required";
        }
        return !required;
    }
    if (!PyFloat_Check(field.get()) && !PyLong_Check(field.get())) {
        PyErr_Clear();
        set_error(name);
        g_last_error += " must be a number";
        return false;
    }
    *value = PyFloat_AsDouble(field.get());
    if (PyErr_Occurred()) {
        set_python_error();
        return false;
    }
    return true;
}

/// \brief Reads an optional boolean field from a trade record.
/// \param object Borrowed Python trade record.
/// \param name Field name.
/// \param[out] value Destination byte value.
/// \return True when the field was copied or absent.
bool trade_bool(PyObject *object, const char *name, uint8_t *value) {
    PyRef field(trade_field(object, name));
    if (!field) {
        PyErr_Clear();
        return true;
    }
    if (field.get() == Py_None)
        return true;
    const int converted = PyObject_IsTrue(field.get());
    if (converted < 0) {
        set_python_error();
        return false;
    }
    *value = converted != 0 ? 1u : 0u;
    return true;
}

/// \brief Copies an optional UTF-8 field into a fixed-size ABI text slot.
/// \param object Borrowed Python trade record.
/// \param name Field name.
/// \param[out] destination Fixed-size destination buffer.
/// \param capacity Destination capacity in bytes.
/// \return True when copied, absent, or empty; false on type/length errors.
bool trade_text(PyObject *object, const char *name, char *destination, std::size_t capacity,
                bool required = false) {
    destination[0] = '\0';
    PyRef field(trade_field(object, name));
    if (!field) {
        PyErr_Clear();
        if (required) {
            set_error(name);
            g_last_error += " is required";
        }
        return !required;
    }
    if (field.get() == Py_None) {
        if (required) {
            set_error(name);
            g_last_error += " is required";
        }
        return !required;
    }
    if (!PyUnicode_Check(field.get())) {
        set_error(name);
        g_last_error += " must be a string";
        PyErr_Clear();
        return false;
    }
    const char *text = PyUnicode_AsUTF8(field.get());
    if (!text) {
        set_python_error();
        return false;
    }
    const std::size_t length = std::strlen(text);
    if (length >= capacity) {
        set_error(name);
        g_last_error += " is too long";
        return false;
    }
    std::memcpy(destination, text, length + 1);
    return true;
}

/// \brief Sets a borrowed UTF-8 string in a Python dictionary.
/// \param dictionary Destination dictionary.
/// \param name Key name.
/// \param value Optional UTF-8 value.
/// \return True when the key was inserted or omitted.
bool set_trade_text(PyObject *dictionary, const char *name, const char *value) {
    if (!value)
        return true;
    PyRef text(PyUnicode_FromString(value));
    return text && PyDict_SetItemString(dictionary, name, text.get()) == 0;
}

/// \brief Sets an unsigned integer request field in a Python dictionary.
/// \param dictionary Destination dictionary.
/// \param name Key name.
/// \param value Integer value.
/// \return True when the key was inserted.
bool set_trade_uint(PyObject *dictionary, const char *name, uint64_t value) {
    PyRef integer(PyLong_FromUnsignedLongLong(value));
    return integer && PyDict_SetItemString(dictionary, name, integer.get()) == 0;
}

/// \brief Sets a signed integer request field in a Python dictionary.
/// \param dictionary Destination dictionary.
/// \param name Key name.
/// \param value Integer value.
/// \return True when the key was inserted.
bool set_trade_int(PyObject *dictionary, const char *name, int64_t value) {
    PyRef integer(PyLong_FromLongLong(value));
    return integer && PyDict_SetItemString(dictionary, name, integer.get()) == 0;
}

/// \brief Sets a floating-point request field in a Python dictionary.
/// \param dictionary Destination dictionary.
/// \param name Key name.
/// \param value Floating-point value.
/// \return True when the key was inserted.
bool set_trade_double(PyObject *dictionary, const char *name, double value) {
    PyRef number(PyFloat_FromDouble(value));
    return number && PyDict_SetItemString(dictionary, name, number.get()) == 0;
}

/// \brief Converts a plain-C order-check request into an MT5 Python mapping.
/// \param request Borrowed ABI request.
/// \return Owned Python dictionary, or nullptr on allocation failure.
PyRef make_order_check_request(const Mt5OrderCheckRequest *request) {
    PyRef dictionary(PyDict_New());
    if (!dictionary)
        return PyRef();
    const bool valid = set_trade_text(dictionary.get(), "symbol", request->symbol_utf8) &&
        set_trade_text(dictionary.get(), "comment", request->comment_utf8) &&
        set_trade_uint(dictionary.get(), "magic", request->magic) &&
        set_trade_uint(dictionary.get(), "order", request->order) &&
        set_trade_uint(dictionary.get(), "position", request->position) &&
        set_trade_uint(dictionary.get(), "position_by", request->position_by) &&
        set_trade_double(dictionary.get(), "volume", request->volume) &&
        set_trade_double(dictionary.get(), "price", request->price) &&
        set_trade_double(dictionary.get(), "stoplimit", request->stoplimit) &&
        set_trade_double(dictionary.get(), "sl", request->sl) &&
        set_trade_double(dictionary.get(), "tp", request->tp) &&
        set_trade_int(dictionary.get(), "expiration", request->expiration) &&
        set_trade_uint(dictionary.get(), "action", request->action) &&
        set_trade_uint(dictionary.get(), "type", request->type) &&
        set_trade_uint(dictionary.get(), "type_filling", request->type_filling) &&
        set_trade_uint(dictionary.get(), "type_time", request->type_time) &&
        set_trade_uint(dictionary.get(), "deviation", request->deviation);
    if (!valid)
        return PyRef();
    return dictionary;
}

/// \brief Copies an account_info() result into the stable account POD.
/// \param object Borrowed namedtuple or mapping returned by MetaTrader5.
/// \param[out] info Destination snapshot.
/// \return True when the required identity fields and all supplied values fit.
bool copy_account_info(PyObject *object, Mt5AccountInfo *info) {
    if (!object || object == Py_None || !info) {
        set_error("MetaTrader5 account_info returned no snapshot");
        return false;
    }
    Mt5AccountInfo converted{};
    if (!trade_text(object, "server", converted.server, sizeof(converted.server), true) ||
        !trade_text(object, "currency", converted.currency, sizeof(converted.currency), true)) {
        return false;
    }
    int64_t login = 0;
    if (!trade_int64(object, "login", &login, true) || login < 0) {
        set_error("login must be a non-negative integer");
        return false;
    }
    converted.login = static_cast<uint64_t>(login);
    if (!trade_int32(object, "margin_mode", &converted.margin_mode, true) ||
        !trade_int32(object, "trade_mode", &converted.trade_mode, true) ||
        !trade_int32(object, "leverage", &converted.leverage, false) ||
        !trade_bool(object, "trade_allowed", &converted.trade_allowed) ||
        !trade_bool(object, "trade_expert", &converted.trade_expert) ||
        !trade_bool(object, "fifo_close", &converted.fifo_close) ||
        !trade_bool(object, "hedge_allowed", &converted.hedge_allowed) ||
        !trade_double(object, "balance", &converted.balance, true) ||
        !trade_double(object, "equity", &converted.equity, true)) {
        return false;
    }
    uint64_t known_fields = 0;
    if (trade_has_field(object, "server"))
        known_fields |= MT5BRIDGE_ACCOUNT_KNOWN_SERVER;
    if (trade_has_field(object, "currency"))
        known_fields |= MT5BRIDGE_ACCOUNT_KNOWN_CURRENCY;
    if (trade_has_field(object, "login"))
        known_fields |= MT5BRIDGE_ACCOUNT_KNOWN_LOGIN;
    if (trade_has_field(object, "margin_mode"))
        known_fields |= MT5BRIDGE_ACCOUNT_KNOWN_MARGIN_MODE;
    if (trade_has_field(object, "trade_mode"))
        known_fields |= MT5BRIDGE_ACCOUNT_KNOWN_TRADE_MODE;
    if (trade_has_field(object, "leverage"))
        known_fields |= MT5BRIDGE_ACCOUNT_KNOWN_LEVERAGE;
    if (trade_has_field(object, "trade_allowed"))
        known_fields |= MT5BRIDGE_ACCOUNT_KNOWN_TRADE_ALLOWED;
    if (trade_has_field(object, "trade_expert"))
        known_fields |= MT5BRIDGE_ACCOUNT_KNOWN_TRADE_EXPERT;
    if (trade_has_field(object, "fifo_close"))
        known_fields |= MT5BRIDGE_ACCOUNT_KNOWN_FIFO_CLOSE;
    if (trade_has_field(object, "hedge_allowed"))
        known_fields |= MT5BRIDGE_ACCOUNT_KNOWN_HEDGE_ALLOWED;
    if (trade_has_field(object, "balance"))
        known_fields |= MT5BRIDGE_ACCOUNT_KNOWN_BALANCE;
    if (trade_has_field(object, "equity"))
        known_fields |= MT5BRIDGE_ACCOUNT_KNOWN_EQUITY;
    converted.known_fields = known_fields;
    *info = converted;
    return true;
}

/// \brief Copies a symbol_info() result into the stable capability POD.
/// \param object Borrowed namedtuple or mapping returned by MetaTrader5.
/// \param mt5 Borrowed MetaTrader5 module used for capability constants.
/// \param[out] capabilities Destination snapshot.
/// \return True when the symbol record is valid and fits the ABI fields.
bool copy_symbol_capabilities(PyObject *object, PyObject *mt5,
                              Mt5SymbolCapabilities *capabilities) {
    if (!object || object == Py_None || !mt5 || !capabilities) {
        set_error("MetaTrader5 symbol_info returned no snapshot");
        return false;
    }
    Mt5SymbolCapabilities converted{};
    if (!trade_text(object, "name", converted.symbol, sizeof(converted.symbol), true) ||
        !trade_uint32(object, "trade_mode", &converted.trade_mode, true) ||
        !trade_uint32(object, "trade_exemode", &converted.trade_exemode, true) ||
        !trade_uint32(object, "order_mode", &converted.order_mode, true) ||
        !trade_uint32(object, "filling_mode", &converted.filling_mode, false) ||
        !trade_uint32(object, "expiration_mode", &converted.expiration_mode, false) ||
        !trade_uint32(object, "order_gtc_mode", &converted.order_gtc_mode, false) ||
        !trade_int32(object, "trade_stops_level", &converted.trade_stops_level, false) ||
        !trade_int32(object, "trade_freeze_level", &converted.trade_freeze_level, false) ||
        !trade_uint32(object, "visible", &converted.visible, false) ||
        !trade_uint32(object, "select", &converted.selected, false) ||
        !trade_double(object, "volume_min", &converted.volume_min, false) ||
        !trade_double(object, "volume_max", &converted.volume_max, false) ||
        !trade_double(object, "volume_step", &converted.volume_step, false) ||
        !trade_double(object, "volume_limit", &converted.volume_limit, false) ||
        !trade_double(object, "trade_tick_size", &converted.trade_tick_size, false) ||
        !trade_double(object, "point", &converted.point, false)) {
        return false;
    }
    PyRef closeby(PyObject_GetAttrString(mt5, "SYMBOL_ORDER_CLOSEBY"));
    if (closeby) {
        const long long flag = PyLong_AsLongLong(closeby.get());
        if (PyErr_Occurred() || flag < 0 ||
            static_cast<unsigned long long>(flag) > std::numeric_limits<uint32_t>::max()) {
            if (PyErr_Occurred())
                set_python_error();
            else
                set_error("SYMBOL_ORDER_CLOSEBY is out of range");
            return false;
        }
        converted.closeby_allowed =
            (converted.order_mode & static_cast<uint32_t>(flag)) != 0 ? 1u : 0u;
    } else {
        PyErr_Clear();
        // SYMBOL_ORDER_CLOSEBY is 64 in the current MT5 API.  Keep the
        // fallback local so fake/minimal modules can still expose capabilities.
        converted.closeby_allowed = (converted.order_mode & 64u) != 0 ? 1u : 0u;
    }
    uint64_t known_fields = MT5BRIDGE_SYMBOL_KNOWN_TRADE_MODE |
                            MT5BRIDGE_SYMBOL_KNOWN_ORDER_MODE |
                            MT5BRIDGE_SYMBOL_KNOWN_TRADE_EXEMODE;
    if (trade_has_field(object, "filling_mode"))
        known_fields |= MT5BRIDGE_SYMBOL_KNOWN_FILLING_MODE;
    if (trade_has_field(object, "expiration_mode"))
        known_fields |= MT5BRIDGE_SYMBOL_KNOWN_EXPIRATION_MODE;
    if (trade_has_field(object, "order_gtc_mode"))
        known_fields |= MT5BRIDGE_SYMBOL_KNOWN_ORDER_GTC_MODE;
    if (trade_has_field(object, "trade_stops_level"))
        known_fields |= MT5BRIDGE_SYMBOL_KNOWN_STOPS_LEVEL;
    if (trade_has_field(object, "trade_freeze_level"))
        known_fields |= MT5BRIDGE_SYMBOL_KNOWN_FREEZE_LEVEL;
    if (trade_has_field(object, "visible"))
        known_fields |= MT5BRIDGE_SYMBOL_KNOWN_VISIBLE;
    if (trade_has_field(object, "select"))
        known_fields |= MT5BRIDGE_SYMBOL_KNOWN_SELECTED;
    if (trade_has_field(object, "volume_min") && trade_has_field(object, "volume_max") &&
        trade_has_field(object, "volume_step") && trade_has_field(object, "volume_limit"))
        known_fields |= MT5BRIDGE_SYMBOL_KNOWN_VOLUME_LIMITS;
    if (trade_has_field(object, "trade_tick_size"))
        known_fields |= MT5BRIDGE_SYMBOL_KNOWN_TICK_SIZE;
    if (trade_has_field(object, "point"))
        known_fields |= MT5BRIDGE_SYMBOL_KNOWN_POINT;
    known_fields |= MT5BRIDGE_SYMBOL_KNOWN_CLOSEBY;
    converted.known_fields = known_fields;
    *capabilities = converted;
    return true;
}

/// \brief Copies an order_check() result into the stable result POD.
/// \param object Borrowed namedtuple or mapping returned by MetaTrader5.
/// \param[out] result Destination result structure.
/// \return True when the retcode and supplied numeric fields are valid.
bool copy_order_check_result(PyObject *object, Mt5OrderCheckResult *result) {
    if (!object || object == Py_None || !result) {
        set_error("MetaTrader5 order_check returned no result");
        return false;
    }
    Mt5OrderCheckResult converted{};
    // MqlTradeCheckResult has a fixed documented shape.  Do not turn a
    // malformed or truncated backend record into a successful result with
    // indistinguishable zero/default fields; callers must see the failure.
    if (!trade_uint32(object, "retcode", &converted.retcode, true) ||
        !trade_double(object, "balance", &converted.balance, true) ||
        !trade_double(object, "equity", &converted.equity, true) ||
        !trade_double(object, "profit", &converted.profit, true) ||
        !trade_double(object, "margin", &converted.margin, true) ||
        !trade_double(object, "margin_free", &converted.margin_free, true) ||
        !trade_double(object, "margin_level", &converted.margin_level, true) ||
        !trade_text(object, "comment", converted.comment, sizeof(converted.comment), true)) {
        return false;
    }
    *result = converted;
    return true;
}

/// \brief Converts a Python seconds or millisecond timestamp to Unix milliseconds.
/// \param object Borrowed trade record.
/// \param msc_name Preferred millisecond field.
/// \param seconds_name Fallback seconds field.
/// \param[out] value Destination timestamp.
/// \param required Whether the timestamp must be present.
/// \return True when the timestamp was copied or was optional and absent.
bool trade_time_msc(PyObject *object, const char *msc_name, const char *seconds_name,
                    int64_t *value, bool required) {
    if (trade_has_field(object, msc_name))
        return trade_int64(object, msc_name, value, required);
    int64_t seconds = 0;
    if (!trade_int64(object, seconds_name, &seconds, required))
        return false;
    if (!trade_has_field(object, seconds_name))
        return true;
    if (seconds > std::numeric_limits<int64_t>::max() / 1000 ||
        seconds < std::numeric_limits<int64_t>::min() / 1000) {
        set_error(seconds_name);
        g_last_error += " timestamp is out of range";
        return false;
    }
    *value = seconds * 1000;
    return true;
}

/// \brief Copies the common MT5 order/history-order record shape.
/// \param object Borrowed namedtuple or mapping returned by MetaTrader5.
/// \param[out] snapshot Destination order snapshot.
/// \return True when required graph fields are valid.
bool copy_order_snapshot(PyObject *object, Mt5OrderSnapshot *snapshot) {
    if (!object || object == Py_None || !snapshot) {
        set_error("MetaTrader5 order snapshot is required");
        return false;
    }
    Mt5OrderSnapshot converted{};
    if (!trade_uint64(object, "ticket", &converted.ticket, true) ||
        !trade_uint64(object, "position_id", &converted.position_id, true) ||
        !trade_uint64(object, "position_by_id", &converted.position_by_id, true) ||
        !trade_uint64(object, "magic", &converted.magic, true) ||
        !trade_uint32(object, "type", &converted.type, true) ||
        !trade_uint32(object, "state", &converted.state, true) ||
        !trade_uint32(object, "reason", &converted.reason, true) ||
        !trade_uint32(object, "type_time", &converted.type_time, true) ||
        !trade_uint32(object, "type_filling", &converted.type_filling, true) ||
        !trade_double(object, "volume_initial", &converted.volume_initial, true) ||
        !trade_double(object, "volume_current", &converted.volume_current, true) ||
        !trade_double(object, "price_open", &converted.price_open, true) ||
        !trade_double(object, "price_current", &converted.price_current, true) ||
        !trade_double(object, "price_stoplimit", &converted.price_stoplimit, true) ||
        !trade_double(object, "sl", &converted.sl, true) ||
        !trade_double(object, "tp", &converted.tp, true) ||
        !trade_time_msc(object, "time_setup_msc", "time_setup", &converted.time_setup_msc, true) ||
        !trade_time_msc(object, "time_done_msc", "time_done", &converted.time_done_msc, true) ||
        !trade_time_msc(object, "time_expiration_msc", "time_expiration",
                        &converted.time_expiration_msc, true) ||
        !trade_text(object, "symbol", converted.symbol, sizeof(converted.symbol), true) ||
        !trade_text(object, "comment", converted.comment, sizeof(converted.comment), true) ||
        !trade_text(object, "external_id", converted.external_id,
                    sizeof(converted.external_id), false)) {
        return false;
    }
    if (trade_has_field(object, "ticket")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_TICKET;
    if (trade_has_field(object, "position_id")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_POSITION_ID;
    if (trade_has_field(object, "position_by_id")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_POSITION_BY_ID;
    if (trade_has_field(object, "magic")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_MAGIC;
    if (trade_has_field(object, "type")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_TYPE;
    if (trade_has_field(object, "state")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_STATE;
    if (trade_has_field(object, "reason")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_REASON;
    if (trade_has_field(object, "type_time")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_TYPE_TIME;
    if (trade_has_field(object, "type_filling")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_TYPE_FILLING;
    if (trade_has_field(object, "volume_initial")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_VOLUME_INITIAL;
    if (trade_has_field(object, "volume_current")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_VOLUME_CURRENT;
    if (trade_has_field(object, "price_open")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_PRICE_OPEN;
    if (trade_has_field(object, "price_current")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_PRICE_CURRENT;
    if (trade_has_field(object, "price_stoplimit")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_PRICE_STOPLIMIT;
    if (trade_has_field(object, "sl")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_SL;
    if (trade_has_field(object, "tp")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_TP;
    if (trade_has_field(object, "time_setup") || trade_has_field(object, "time_setup_msc")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_TIME_SETUP;
    if (trade_has_field(object, "time_done") || trade_has_field(object, "time_done_msc")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_TIME_DONE;
    if (trade_has_field(object, "time_expiration") || trade_has_field(object, "time_expiration_msc")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_TIME_EXPIRATION;
    if (trade_has_field(object, "symbol")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_SYMBOL;
    if (trade_has_field(object, "comment")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_COMMENT;
    if (trade_has_field(object, "external_id")) converted.known_fields |= MT5BRIDGE_ORDER_KNOWN_EXTERNAL_ID;
    *snapshot = converted;
    return true;
}

/// \brief Copies an MT5 position record into its stable POD snapshot.
/// \param object Borrowed namedtuple or mapping returned by MetaTrader5.
/// \param[out] snapshot Destination position snapshot.
/// \return True when required graph fields are valid.
bool copy_position_snapshot(PyObject *object, Mt5PositionSnapshot *snapshot) {
    if (!object || object == Py_None || !snapshot) {
        set_error("MetaTrader5 position snapshot is required");
        return false;
    }
    Mt5PositionSnapshot converted{};
    if (!trade_uint64(object, "ticket", &converted.ticket, true) ||
        !trade_uint64(object, "identifier", &converted.identifier, true) ||
        !trade_uint64(object, "magic", &converted.magic, true) ||
        !trade_uint32(object, "type", &converted.type, true) ||
        !trade_uint32(object, "reason", &converted.reason, true) ||
        !trade_double(object, "volume", &converted.volume, true) ||
        !trade_double(object, "price_open", &converted.price_open, true) ||
        !trade_double(object, "price_current", &converted.price_current, true) ||
        !trade_double(object, "sl", &converted.sl, true) ||
        !trade_double(object, "tp", &converted.tp, true) ||
        !trade_double(object, "profit", &converted.profit, true) ||
        !trade_double(object, "swap", &converted.swap, true) ||
        !trade_time_msc(object, "time_msc", "time", &converted.time_msc, true) ||
        !trade_time_msc(object, "time_update_msc", "time_update", &converted.time_update_msc, true) ||
        !trade_text(object, "symbol", converted.symbol, sizeof(converted.symbol), true) ||
        !trade_text(object, "comment", converted.comment, sizeof(converted.comment), true) ||
        !trade_text(object, "external_id", converted.external_id,
                    sizeof(converted.external_id), false)) {
        return false;
    }
    if (trade_has_field(object, "ticket")) converted.known_fields |= MT5BRIDGE_POSITION_KNOWN_TICKET;
    if (trade_has_field(object, "identifier")) converted.known_fields |= MT5BRIDGE_POSITION_KNOWN_IDENTIFIER;
    if (trade_has_field(object, "magic")) converted.known_fields |= MT5BRIDGE_POSITION_KNOWN_MAGIC;
    if (trade_has_field(object, "type")) converted.known_fields |= MT5BRIDGE_POSITION_KNOWN_TYPE;
    if (trade_has_field(object, "reason")) converted.known_fields |= MT5BRIDGE_POSITION_KNOWN_REASON;
    if (trade_has_field(object, "volume")) converted.known_fields |= MT5BRIDGE_POSITION_KNOWN_VOLUME;
    if (trade_has_field(object, "price_open")) converted.known_fields |= MT5BRIDGE_POSITION_KNOWN_PRICE_OPEN;
    if (trade_has_field(object, "price_current")) converted.known_fields |= MT5BRIDGE_POSITION_KNOWN_PRICE_CURRENT;
    if (trade_has_field(object, "sl")) converted.known_fields |= MT5BRIDGE_POSITION_KNOWN_SL;
    if (trade_has_field(object, "tp")) converted.known_fields |= MT5BRIDGE_POSITION_KNOWN_TP;
    if (trade_has_field(object, "profit")) converted.known_fields |= MT5BRIDGE_POSITION_KNOWN_PROFIT;
    if (trade_has_field(object, "swap")) converted.known_fields |= MT5BRIDGE_POSITION_KNOWN_SWAP;
    if (trade_has_field(object, "time") || trade_has_field(object, "time_msc")) converted.known_fields |= MT5BRIDGE_POSITION_KNOWN_TIME;
    if (trade_has_field(object, "time_update") || trade_has_field(object, "time_update_msc")) converted.known_fields |= MT5BRIDGE_POSITION_KNOWN_TIME_UPDATE;
    if (trade_has_field(object, "symbol")) converted.known_fields |= MT5BRIDGE_POSITION_KNOWN_SYMBOL;
    if (trade_has_field(object, "comment")) converted.known_fields |= MT5BRIDGE_POSITION_KNOWN_COMMENT;
    if (trade_has_field(object, "external_id")) converted.known_fields |= MT5BRIDGE_POSITION_KNOWN_EXTERNAL_ID;
    *snapshot = converted;
    return true;
}

/// \brief Copies an MT5 deal record into its stable POD snapshot.
/// \param object Borrowed namedtuple or mapping returned by MetaTrader5.
/// \param[out] snapshot Destination deal snapshot.
/// \return True when required graph fields are valid.
bool copy_deal_snapshot(PyObject *object, Mt5DealSnapshot *snapshot) {
    if (!object || object == Py_None || !snapshot) {
        set_error("MetaTrader5 deal snapshot is required");
        return false;
    }
    Mt5DealSnapshot converted{};
    if (!trade_uint64(object, "ticket", &converted.ticket, true) ||
        !trade_uint64(object, "order", &converted.order_ticket, true) ||
        !trade_uint64(object, "position_id", &converted.position_id, true) ||
        !trade_uint64(object, "magic", &converted.magic, true) ||
        !trade_uint32(object, "type", &converted.type, true) ||
        !trade_uint32(object, "entry", &converted.entry, true) ||
        !trade_uint32(object, "reason", &converted.reason, true) ||
        !trade_double(object, "volume", &converted.volume, true) ||
        !trade_double(object, "price", &converted.price, true) ||
        !trade_double(object, "profit", &converted.profit, true) ||
        !trade_double(object, "commission", &converted.commission, true) ||
        !trade_double(object, "swap", &converted.swap, true) ||
        !trade_double(object, "fee", &converted.fee, true) ||
        !trade_time_msc(object, "time_msc", "time", &converted.time_msc, true) ||
        !trade_text(object, "symbol", converted.symbol, sizeof(converted.symbol), true) ||
        !trade_text(object, "comment", converted.comment, sizeof(converted.comment), true) ||
        !trade_text(object, "external_id", converted.external_id,
                    sizeof(converted.external_id), false)) {
        return false;
    }
    if (trade_has_field(object, "ticket")) converted.known_fields |= MT5BRIDGE_DEAL_KNOWN_TICKET;
    if (trade_has_field(object, "order")) converted.known_fields |= MT5BRIDGE_DEAL_KNOWN_ORDER_TICKET;
    if (trade_has_field(object, "position_id")) converted.known_fields |= MT5BRIDGE_DEAL_KNOWN_POSITION_ID;
    if (trade_has_field(object, "magic")) converted.known_fields |= MT5BRIDGE_DEAL_KNOWN_MAGIC;
    if (trade_has_field(object, "type")) converted.known_fields |= MT5BRIDGE_DEAL_KNOWN_TYPE;
    if (trade_has_field(object, "entry")) converted.known_fields |= MT5BRIDGE_DEAL_KNOWN_ENTRY;
    if (trade_has_field(object, "reason")) converted.known_fields |= MT5BRIDGE_DEAL_KNOWN_REASON;
    if (trade_has_field(object, "volume")) converted.known_fields |= MT5BRIDGE_DEAL_KNOWN_VOLUME;
    if (trade_has_field(object, "price")) converted.known_fields |= MT5BRIDGE_DEAL_KNOWN_PRICE;
    if (trade_has_field(object, "profit")) converted.known_fields |= MT5BRIDGE_DEAL_KNOWN_PROFIT;
    if (trade_has_field(object, "commission")) converted.known_fields |= MT5BRIDGE_DEAL_KNOWN_COMMISSION;
    if (trade_has_field(object, "swap")) converted.known_fields |= MT5BRIDGE_DEAL_KNOWN_SWAP;
    if (trade_has_field(object, "fee")) converted.known_fields |= MT5BRIDGE_DEAL_KNOWN_FEE;
    if (trade_has_field(object, "time") || trade_has_field(object, "time_msc")) converted.known_fields |= MT5BRIDGE_DEAL_KNOWN_TIME;
    if (trade_has_field(object, "symbol")) converted.known_fields |= MT5BRIDGE_DEAL_KNOWN_SYMBOL;
    if (trade_has_field(object, "comment")) converted.known_fields |= MT5BRIDGE_DEAL_KNOWN_COMMENT;
    if (trade_has_field(object, "external_id")) converted.known_fields |= MT5BRIDGE_DEAL_KNOWN_EXTERNAL_ID;
    *snapshot = converted;
    return true;
}

/// \brief Builds keyword arguments shared by MT5 trade collection queries.
/// \param kwargs Destination Python dictionary.
/// \param symbol Optional symbol filter.
/// \param group Optional MT5 group mask.
/// \param ticket Optional ticket filter.
/// \return True when all supplied values were inserted.
bool set_trade_query_filters(PyObject *kwargs, const char *symbol, const char *group,
                             uint64_t ticket) {
    auto set_text = [kwargs](const char *name, const char *value) {
        if (!value || !*value)
            return true;
        PyRef text(PyUnicode_FromString(value));
        return text && PyDict_SetItemString(kwargs, name, text.get()) == 0;
    };
    auto set_uint = [kwargs](const char *name, uint64_t value) {
        if (value == 0)
            return true;
        PyRef integer(PyLong_FromUnsignedLongLong(value));
        return integer && PyDict_SetItemString(kwargs, name, integer.get()) == 0;
    };
    return set_text("symbol", symbol) && set_text("group", group) &&
           set_uint("ticket", ticket);
}

/// \brief Calls one MetaTrader trade collection method with typed filters.
/// \param mt5 Borrowed MetaTrader5 module.
/// \param method Collection method name.
/// \param symbol Optional active-query symbol filter.
/// \param group Optional MT5 group mask.
/// \param ticket Optional ticket filter.
/// \param position_id Optional position identifier filter.
/// \param from_msc History start, or zero for active queries.
/// \param to_msc History end, or zero for active queries.
/// \param history True when positional history dates are required.
/// \return Owned Python collection result, or nullptr on failure.
PyRef call_trade_collection(PyObject *mt5, const char *method, const char *symbol,
                            const char *group, uint64_t ticket,
                            int64_t from_msc, int64_t to_msc, bool history) {
    PyRef callable(PyObject_GetAttrString(mt5, method));
    PyRef args(PyTuple_New(history ? 2 : 0));
    PyRef kwargs(PyDict_New());
    if (!callable || !args || !kwargs)
        return PyRef();
    if (history) {
        PyRef from(make_datetime(from_msc));
        PyRef to(make_datetime(to_msc));
        if (!from || !to || PyTuple_SetItem(args.get(), 0, from.release()) != 0 ||
            PyTuple_SetItem(args.get(), 1, to.release()) != 0)
            return PyRef();
    }
    if (!set_trade_query_filters(kwargs.get(), symbol, group, ticket))
        return PyRef();
    return PyRef(PyObject_Call(callable.get(), args.get(), kwargs.get()));
}

/// \brief Copies one Python trade collection into a typed vector.
/// \tparam T Snapshot POD type.
/// \tparam Converter Callable converting one record.
/// \param object Borrowed Python collection result.
/// \param[out] values Destination vector replaced with converted records.
/// \param converter Record converter.
/// \return True when the collection is a sequence, including an empty result.
template <typename T, typename Converter>
bool copy_trade_collection(PyObject *object, std::vector<T> *values, Converter converter) {
    if (!object || !values) {
        set_error("MetaTrader trade collection returned no result");
        return false;
    }
    values->clear();
    if (object == Py_None) {
        set_error("MetaTrader trade collection query failed");
        return false;
    }
    if (!PySequence_Check(object)) {
        set_error("MetaTrader trade collection must be a sequence");
        return false;
    }
    const Py_ssize_t count = PySequence_Size(object);
    if (count < 0) {
        set_python_error();
        return false;
    }
    values->reserve(static_cast<std::size_t>(count));
    for (Py_ssize_t index = 0; index < count; ++index) {
        PyRef item(PySequence_GetItem(object, index));
        T converted{};
        if (!item || !converter(item.get(), &converted)) {
            if (item && PyErr_Occurred())
                set_python_error();
            values->clear();
            return false;
        }
        values->push_back(converted);
    }
    return true;
}

/// \brief Validates the documented mutually-exclusive MT5 active-query selectors.
/// \param symbol Optional symbol selector.
/// \param group Optional group selector.
/// \param ticket Optional ticket selector.
/// \param operation Human-readable operation name for diagnostics.
/// \return True when zero or one native selector is present.
bool valid_active_selector(const char *symbol, const char *group, uint64_t ticket,
                           const char *operation) {
    const unsigned count = (symbol && *symbol ? 1u : 0u) +
                           (group && *group ? 1u : 0u) + (ticket != 0 ? 1u : 0u);
    if (count > 1) {
        set_error(operation);
        g_last_error += " accepts only one of symbol, group, or ticket";
        return false;
    }
    return true;
}

/// \brief Finds a named field in a NumPy structured-array dtype.
/// \param array Borrowed structured array.
/// \param name Field name to locate.
/// \param[out] offset Byte offset of the field within an item.
/// \param[out] itemsize Total byte size of one array item.
/// \return True when the dtype contains a valid field description.
bool field_offset(PyObject *array, const char *name, std::size_t *offset,
                  std::size_t *itemsize) {
    PyRef dtype(PyObject_GetAttrString(array, "dtype"));
    PyRef fields(dtype ? PyObject_GetAttrString(dtype.get(), "fields") : nullptr);
    PyRef entry(fields && PyMapping_Check(fields.get())
                    ? PyMapping_GetItemString(fields.get(), name)
                    : nullptr);
    if (!entry || !PyTuple_Check(entry.get()) || PyTuple_Size(entry.get()) < 2) {
        PyErr_Clear();
        return false;
    }
    PyObject *offset_object = PyTuple_GetItem(entry.get(), 1);
    const unsigned long long value = PyLong_AsUnsignedLongLong(offset_object);
    if (PyErr_Occurred()) {
        PyErr_Clear();
        return false;
    }
    PyRef size(PyObject_GetAttrString(dtype.get(), "itemsize"));
    const long long item_value = size ? PyLong_AsLongLong(size.get()) : -1;
    if (item_value <= 0 || value > static_cast<unsigned long long>(item_value))
        return false;
    *offset = static_cast<std::size_t>(value);
    *itemsize = static_cast<std::size_t>(item_value);
    return true;
}

/// \brief Copies a trivially represented field from a structured-array buffer.
/// \tparam T Destination field type.
/// \param view Acquired Python buffer view.
/// \param index Array element index.
/// \param offset Field byte offset within one item.
/// \param itemsize Total byte size of one item.
/// \param[out] value Destination value.
/// \return True when the requested bytes fit within the item.
template <typename T>
bool copy_field(const Py_buffer &view, std::size_t index, std::size_t offset,
                std::size_t itemsize, T *value) {
    const auto *base = static_cast<const unsigned char *>(view.buf) + index * itemsize;
    if (offset + sizeof(T) > itemsize)
        return false;
    std::memcpy(value, base + offset, sizeof(T));
    return true;
}

/// \brief Converts a standard MetaTrader tick NumPy array to POD ticks.
/// \param array Borrowed NumPy structured array.
/// \param[out] output Destination vector replaced with converted ticks.
/// \return True when the array layout is supported and fully converted.
bool copy_ticks_array(PyObject *array, std::vector<Mt5Tick> *output) {
    Py_buffer view{};
    if (PyObject_GetBuffer(array, &view, PyBUF_SIMPLE) != 0)
        return false;
    const Py_ssize_t length = PyObject_Length(array);
    std::size_t itemsize = 0;
    std::size_t time_msc_offset = 0;
    std::size_t time_offset = 0;
    std::size_t bid_offset = 0;
    std::size_t ask_offset = 0;
    std::size_t last_offset = 0;
    std::size_t volume_offset = 0;
    std::size_t volume_real_offset = 0;
    std::size_t flags_offset = 0;
    const bool has_time_msc = field_offset(array, "time_msc", &time_msc_offset, &itemsize);
    const bool has_time = field_offset(array, "time", &time_offset, &itemsize);
    const bool valid = has_time && field_offset(array, "bid", &bid_offset, &itemsize) &&
                       field_offset(array, "ask", &ask_offset, &itemsize) &&
                       field_offset(array, "last", &last_offset, &itemsize) &&
                       field_offset(array, "volume", &volume_offset, &itemsize) &&
                       field_offset(array, "volume_real", &volume_real_offset, &itemsize) &&
                       field_offset(array, "flags", &flags_offset, &itemsize) &&
                       has_time_msc;
    if (!valid || length < 0) {
        PyBuffer_Release(&view);
        return false;
    }
    output->clear();
    output->reserve(static_cast<std::size_t>(length));
    for (Py_ssize_t i = 0; i < length; ++i) {
        Mt5Tick tick{};
        int64_t time = 0;
        if (!copy_field(view, static_cast<std::size_t>(i), has_time_msc ? time_msc_offset : time_offset,
                        itemsize, &time) ||
            !copy_field(view, static_cast<std::size_t>(i), bid_offset, itemsize, &tick.bid) ||
            !copy_field(view, static_cast<std::size_t>(i), ask_offset, itemsize, &tick.ask) ||
            !copy_field(view, static_cast<std::size_t>(i), last_offset, itemsize, &tick.last) ||
            !copy_field(view, static_cast<std::size_t>(i), volume_offset, itemsize,
                        &tick.volume) ||
            !copy_field(view, static_cast<std::size_t>(i), volume_real_offset, itemsize,
                        &tick.volume_real) ||
            !copy_field(view, static_cast<std::size_t>(i), flags_offset, itemsize, &tick.flags)) {
            PyBuffer_Release(&view);
            return false;
        }
        tick.time_msc = time;
        output->push_back(tick);
    }
    PyBuffer_Release(&view);
    return true;
}

/// \brief Converts a standard MetaTrader rate NumPy array to POD rates.
/// \param array Borrowed NumPy structured array.
/// \param[out] output Destination vector replaced with converted rates.
/// \return True when the array layout is supported and fully converted.
bool copy_rates_array(PyObject *array, std::vector<Mt5Rate> *output) {
    Py_buffer view{};
    if (PyObject_GetBuffer(array, &view, PyBUF_SIMPLE) != 0)
        return false;
    const Py_ssize_t length = PyObject_Length(array);
    std::size_t itemsize = 0;
    std::size_t time_offset = 0, open_offset = 0, high_offset = 0, low_offset = 0;
    std::size_t close_offset = 0, tick_volume_offset = 0, spread_offset = 0;
    std::size_t real_volume_offset = 0;
    const bool valid = field_offset(array, "time", &time_offset, &itemsize) &&
                       field_offset(array, "open", &open_offset, &itemsize) &&
                       field_offset(array, "high", &high_offset, &itemsize) &&
                       field_offset(array, "low", &low_offset, &itemsize) &&
                       field_offset(array, "close", &close_offset, &itemsize) &&
                       field_offset(array, "tick_volume", &tick_volume_offset, &itemsize) &&
                       field_offset(array, "spread", &spread_offset, &itemsize) &&
                       field_offset(array, "real_volume", &real_volume_offset, &itemsize);
    if (!valid || length < 0) {
        PyBuffer_Release(&view);
        return false;
    }
    output->clear();
    output->reserve(static_cast<std::size_t>(length));
    for (Py_ssize_t i = 0; i < length; ++i) {
        Mt5Rate rate{};
        if (!copy_field(view, static_cast<std::size_t>(i), time_offset, itemsize, &rate.time) ||
            !copy_field(view, static_cast<std::size_t>(i), open_offset, itemsize, &rate.open) ||
            !copy_field(view, static_cast<std::size_t>(i), high_offset, itemsize, &rate.high) ||
            !copy_field(view, static_cast<std::size_t>(i), low_offset, itemsize, &rate.low) ||
            !copy_field(view, static_cast<std::size_t>(i), close_offset, itemsize, &rate.close) ||
            !copy_field(view, static_cast<std::size_t>(i), tick_volume_offset, itemsize,
                        &rate.tick_volume) ||
            !copy_field(view, static_cast<std::size_t>(i), spread_offset, itemsize, &rate.spread) ||
            !copy_field(view, static_cast<std::size_t>(i), real_volume_offset, itemsize,
                        &rate.real_volume)) {
            PyBuffer_Release(&view);
            return false;
        }
        output->push_back(rate);
    }
    PyBuffer_Release(&view);
    return true;
}

/// \brief Resolves an MT5 timeframe to its bar period in seconds.
/// \param timeframe Numeric MetaTrader TIMEFRAME_* value.
/// \return Period in seconds, or zero when the value is not recognized.
int64_t timeframe_period_seconds(int32_t timeframe) {
    switch (timeframe) {
    case 1: return 60;
    case 2: return 2LL * 60;
    case 3: return 3LL * 60;
    case 4: return 4LL * 60;
    case 5: return 5LL * 60;
    case 6: return 6LL * 60;
    case 10: return 10LL * 60;
    case 12: return 12LL * 60;
    case 15: return 15LL * 60;
    case 20: return 20LL * 60;
    case 30: return 30LL * 60;
    case 16385: return 60LL * 60;
    case 16386: return 2LL * 60 * 60;
    case 16387: return 3LL * 60 * 60;
    case 16388: return 4LL * 60 * 60;
    case 16390: return 6LL * 60 * 60;
    case 16392: return 8LL * 60 * 60;
    case 16396: return 12LL * 60 * 60;
    // D1/W1/MN1 use broker/session calendar boundaries rather than fixed
    // Unix periods.  Leave them unproven until a calendar-aware contract exists.
    case 16408:
    case 32769:
    case 49153:
        return 0;
    default: return 0;
    }
}

/// \brief Filters rate rows and records their raw timestamp envelope.
/// \param values Converted rows returned by MetaTrader.
/// \param request Original inclusive range.
/// \param[out] filtered Rows whose open time lies in the requested range.
/// \param[in,out] coverage Coverage record whose observed envelope is updated.
/// \return False when a timestamp cannot be represented as Unix milliseconds.
bool prepare_rate_observation(const std::vector<Mt5Rate> &values,
                              const Mt5RatesRequest *request,
                              std::vector<Mt5Rate> *filtered,
                              Mt5RateCoverageV1 *coverage) {
    if (!request || !filtered || !coverage)
        return false;
    filtered->clear();
    filtered->reserve(values.size());
    coverage->observed_from_msc = -1;
    coverage->observed_to_msc = -1;
    constexpr int64_t kMillisecondsPerSecond = 1000;
    for (const auto &value : values) {
        if (value.time < 0 || value.time > std::numeric_limits<int64_t>::max() /
                                  kMillisecondsPerSecond) {
            set_error("MetaTrader returned an invalid rate timestamp");
            return false;
        }
        const int64_t time_msc = value.time * kMillisecondsPerSecond;
        if (coverage->observed_from_msc < 0 || time_msc < coverage->observed_from_msc)
            coverage->observed_from_msc = time_msc;
        if (coverage->observed_to_msc < 0 || time_msc > coverage->observed_to_msc)
            coverage->observed_to_msc = time_msc;
        if (time_msc >= request->from_msc && time_msc <= request->to_msc)
            filtered->push_back(value);
    }
    return true;
}

/// \brief Tests timeframe-aware boundary coverage for a filtered rate result.
/// \param request Original inclusive range.
/// \param values Rows retained inside the requested range.
/// \param coverage Raw timestamp envelope observed from MetaTrader.
/// \return True only when both aligned boundaries are evidenced.
bool rate_coverage_proven(const Mt5RatesRequest *request,
                           const std::vector<Mt5Rate> &values,
                           const Mt5RateCoverageV1 &coverage) {
    if (!request || values.empty() || coverage.observed_from_msc < 0 ||
        coverage.observed_to_msc < 0)
        return false;
    const int64_t period_seconds = timeframe_period_seconds(request->timeframe);
    if (period_seconds <= 0)
        return false;
    const int64_t period_msc = period_seconds * 1000;
    const int64_t from_remainder = request->from_msc % period_msc;
    const int64_t first_index = request->from_msc / period_msc +
        (from_remainder ? 1 : 0);
    if (first_index > std::numeric_limits<int64_t>::max() / period_msc)
        return false;
    const int64_t expected_from = first_index * period_msc;
    const int64_t expected_to = (request->to_msc / period_msc) * period_msc;
    if (expected_from > expected_to)
        return false;
    bool has_expected_from = false;
    bool has_expected_to = false;
    for (const auto &value : values) {
        const int64_t time_msc = value.time * 1000;
        has_expected_from = has_expected_from || time_msc == expected_from;
        has_expected_to = has_expected_to || time_msc == expected_to;
    }
    return coverage.observed_from_msc <= expected_from &&
           coverage.observed_to_msc >= expected_to && has_expected_from && has_expected_to;
}

/// \brief Compares the stable range evidence used by two rate observations.
/// \param left First observation coverage.
/// \param right Second observation coverage.
/// \return True when both observations have the same immutable V1 evidence.
bool same_rate_coverage(const Mt5RateCoverageV1 &left, const Mt5RateCoverageV1 &right) {
    return left.version == right.version && left.state == right.state &&
           left.requested_from_msc == right.requested_from_msc &&
           left.requested_to_msc == right.requested_to_msc &&
           left.observed_from_msc == right.observed_from_msc &&
           left.observed_to_msc == right.observed_to_msc;
}

/// \brief Creates a UTC Python datetime from a Unix millisecond timestamp.
/// \param milliseconds Unix timestamp in milliseconds.
/// \return Owning reference wrapper; empty when conversion fails.
PyRef make_datetime(int64_t milliseconds) {
    PyRef module(PyImport_ImportModule("datetime"));
    PyRef type(module ? PyObject_GetAttrString(module.get(), "datetime") : nullptr);
    PyRef timezone(module ? PyObject_GetAttrString(module.get(), "timezone") : nullptr);
    PyRef utc(timezone ? PyObject_GetAttrString(timezone.get(), "utc") : nullptr);
    return PyRef(type && utc ? PyObject_CallMethod(type.get(), "fromtimestamp", "dO",
                                                    static_cast<double>(milliseconds) / 1000.0,
                                                    utc.get())
                            : nullptr);
}

/// \struct HistoryQueryRange
/// \brief Second-aligned superset used for MT5 history selection.
struct HistoryQueryRange {
    int64_t from_msc = 0; ///< Floor-aligned query start in milliseconds.
    int64_t to_msc = 0;   ///< Ceil-aligned query end in milliseconds.
};

/// \brief Expands a millisecond history window to the server's second granularity.
/// \param from_msc Inclusive requested range start in Unix milliseconds.
/// \param to_msc Inclusive requested range end in Unix milliseconds.
/// \param[out] range Second-aligned superset range for the MT5 Python API.
/// \return True when the expanded range is representable as int64 milliseconds.
/// \note The caller must apply the exact millisecond filter after conversion.
bool make_history_query_range(int64_t from_msc, int64_t to_msc,
                              HistoryQueryRange *range) {
    if (!range || from_msc < 0 || to_msc < from_msc) {
        set_error("invalid history time range");
        return false;
    }
    constexpr int64_t kMillisecondsPerSecond = 1000;
    const int64_t from_remainder = from_msc % kMillisecondsPerSecond;
    const int64_t to_remainder = to_msc % kMillisecondsPerSecond;
    int64_t query_to = to_msc;
    if (to_remainder != 0) {
        const int64_t delta = kMillisecondsPerSecond - to_remainder;
        if (to_msc > std::numeric_limits<int64_t>::max() - delta) {
            set_error("history time range exceeds supported timestamp range");
            return false;
        }
        query_to += delta;
    }
    range->from_msc = from_msc - from_remainder;
    range->to_msc = query_to;
    return true;
}

/// \brief Resolves zero tick flags to MetaTrader's COPY_TICKS_ALL value.
/// \param mt5 Borrowed MetaTrader5 module.
/// \param requested Caller-provided flags.
/// \return Effective flags passed to the Python API.
int resolve_tick_flags(PyObject *mt5, uint32_t requested) {
    if (requested)
        return static_cast<int>(requested);
    PyRef value(PyObject_GetAttrString(mt5, "COPY_TICKS_ALL"));
    return value ? static_cast<int>(PyLong_AsLong(value.get())) : 0;
}

/// \brief Validates the common symbol and timestamp range contract.
/// \param symbol Null-terminated UTF-8 symbol name.
/// \param from_msc Inclusive range start as Unix milliseconds.
/// \param to_msc Inclusive range end as Unix milliseconds.
/// \return True when all arguments satisfy the public preconditions.
bool valid_range(const char *symbol, int64_t from_msc, int64_t to_msc) {
    if (!symbol || !*symbol) {
        set_error("symbol_utf8 is required");
        return false;
    }
    if (from_msc < 0 || to_msc < from_msc) {
        set_error("invalid time range");
        return false;
    }
    return true;
}

/// \brief Maximum number of ticks requested from Python in one bounded page.
constexpr int kTickPageSize = 65536;
/// \brief Maximum number of additional short-page probes before ending a read.
constexpr uint32_t kShortPageProbes = 2;
/// \brief Maximum number of partial pages accepted while recovering history.
constexpr uint32_t kPartialPageLimit = 3;
/// \brief Minimum requested range that activates progressive history bootstrap.
constexpr int64_t kTickBootstrapTriggerMs = 30LL * 24LL * 60LL * 60LL * 1000LL;
/// \brief Initial backward step used by progressive history synchronization probes.
constexpr int64_t kTickBootstrapInitialStepMs = 366LL * 24LL * 60LL * 60LL * 1000LL;
/// \brief Smallest backward step used after a probe reports no frontier movement.
constexpr int64_t kTickBootstrapMinimumStepMs = 30LL * 24LL * 60LL * 60LL * 1000LL;
/// \brief Maximum unobserved gap accepted when confirming a first available tick.
///
/// A target probe can only prove that the terminal exposes a suffix.  Limiting
/// the gap that may be treated as a clean weekend/holiday boundary prevents a
/// stale, distant suffix from masquerading as coverage of the requested
/// frontier.  Larger gaps remain retry-exhausted until a probe actually moves
/// the frontier to the requested start.
constexpr int64_t kTickBootstrapMaximumAnchorGapMs = kTickBootstrapMinimumStepMs;
/// \brief Maximum number of synchronization probes in one historical query.
constexpr uint32_t kTickBootstrapProbeLimit = 16;
/// \brief Consecutive stalled probes allowed at the smallest adaptive step.
constexpr uint32_t kTickBootstrapNoProgressLimit = 3;
/// \brief Hard per-epoch tick budget preventing unbounded realtime catch-up allocations.
constexpr std::size_t kRealtimeEpochTickLimit = 1000000;
constexpr std::size_t kRealtimeMaxRetainedTicks = 1000000;
/// \brief Lower bound for overlap reconciliation in milliseconds.
constexpr int64_t kRealtimeMinOverlapMs = 1000;
/// \brief Upper bound for adaptive overlap reconciliation in milliseconds.
constexpr int64_t kRealtimeMaxOverlapMs = 60000;

/// \brief Gets the numeric status reported by the MetaTrader Python module.
/// \param mt5 Borrowed MetaTrader5 module.
/// \return MetaTrader status code, or zero when the diagnostic is unavailable.
int mt5_last_error_code(PyObject *mt5) {
    PyRef result(PyObject_CallMethod(mt5, "last_error", nullptr));
    if (!result) {
        PyErr_Clear();
        return 0;
    }
    PyObject *code = PyTuple_Check(result.get()) && PyTuple_Size(result.get()) > 0
                         ? PyTuple_GetItem(result.get(), 0)
                         : result.get();
    const long value = PyLong_AsLong(code);
    if (PyErr_Occurred()) {
        PyErr_Clear();
        return 0;
    }
    return static_cast<int>(value);
}

/// \brief Tests whether an MT5 status can be retried for an idempotent history read.
/// \param code MetaTrader Python API status code.
/// \return True for history-not-found, IPC, and timeout conditions.
bool is_transient_read_error(int code) {
    return code == 0 || code == -4 || code == 4403 || (code <= -10000 && code >= -10005);
}

/// \brief Tests whether returned data must be treated as a recoverable partial page.
/// \param code MetaTrader Python API status code.
/// \return True when the result carries a recognized timeout or IPC error.
bool is_partial_read_error(int code) {
    return code == -4 || code == 4403 || (code <= -10000 && code >= -10005);
}

/// \brief Tests whether an MT5 status requires terminal reinitialization before retrying.
/// \param code MetaTrader Python API status code.
/// \return True for IPC transport failures.
bool is_ipc_error(int code) { return code <= -10000 && code >= -10005; }

/// \brief Reinitializes the MetaTrader Python module after an IPC failure.
/// \param mt5 Borrowed MetaTrader5 module.
/// \return True when the terminal reconnects successfully.
bool reinitialize_terminal(PyObject *mt5) {
    PyRef shutdown(PyObject_CallMethod(mt5, "shutdown", nullptr));
    if (!shutdown) {
        PyErr_Clear();
        return false;
    }
    PyRef initialize(PyObject_CallMethod(mt5, "initialize", nullptr));
    const int initialized = initialize ? PyObject_IsTrue(initialize.get()) : 0;
    if (initialized <= 0) {
        PyErr_Clear();
        return false;
    }
    return true;
}

/// \brief Invokes a native consumer without allowing exceptions across the C ABI.
/// \param callback Consumer function to invoke.
/// \param ticks Borrowed transient tick array.
/// \param count Number of ticks in the array.
/// \param user_data Opaque consumer context.
/// \return Consumer status, or -1 when it throws.
int invoke_tick_callback(Mt5TickChunkCallback callback, const Mt5Tick *ticks, size_t count,
                         void *user_data) {
    try {
        return callback(ticks, count, user_data);
    } catch (...) {
        set_error("tick consumer threw an exception");
        return -1;
    }
}

/// \brief Fetches one bounded tick page with history warm-up retries.
/// \param mt5 Borrowed MetaTrader5 module.
/// \param symbol Null-terminated UTF-8 symbol name.
/// \param from_msc Inclusive page start as Unix milliseconds.
/// \param flags Effective COPY_TICKS_* filter.
/// \param count Maximum number of ticks to request from MetaTrader 5.
/// \param[out] page Destination vector replaced with the returned page.
/// \param[in,out] diagnostics Optional recovery counters to update.
/// \param[out] partial Receives true when MT5 attached a transient error to returned data.
/// \return True after a valid array response, including an empty array.
/// \note Python None is treated as transient because it commonly accompanies history warm-up.
bool copy_ticks_page(PyObject *mt5, const char *symbol, int64_t from_msc, int flags, int count,
                     std::vector<Mt5Tick> *page, Mt5FetchDiagnostics *diagnostics,
                     bool *partial, bool *transient_failure = nullptr,
                     uint32_t max_attempts = 3) {
    if (partial)
        *partial = false;
    if (transient_failure)
        *transient_failure = false;
    max_attempts = std::max<uint32_t>(1, max_attempts);
    for (uint32_t attempt = 1; attempt <= max_attempts; ++attempt) {
        if (diagnostics)
            ++diagnostics->attempts;
        PyRef from(make_datetime(from_msc));
        if (!from) {
            mark_fetch_fatal();
            set_python_error();
            return false;
        }
        PyRef ticks(PyObject_CallMethod(mt5, "copy_ticks_from", "sOii", symbol, from.get(),
                                        count, flags));
        if (ticks && ticks.get() != Py_None) {
            if (!copy_ticks_array(ticks.get(), page)) {
                mark_fetch_fatal();
                set_error("MetaTrader5 returned an unsupported tick layout");
                return false;
            }
            const int code = mt5_last_error_code(mt5);
            if (diagnostics && code != 1)
                diagnostics->last_mt5_error = code;
            const bool transient_page = is_partial_read_error(code);
            if (partial)
                *partial = transient_page;
            if (transient_page) {
                if (diagnostics)
                    diagnostics->history_warmup_detected = 1;
                if (is_ipc_error(code) && reinitialize_terminal(mt5) && diagnostics)
                    ++diagnostics->reconnects;
                if (attempt < max_attempts) {
                    if (diagnostics)
                        ++diagnostics->retries;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50u << (attempt - 1)));
                    continue;
                }
                if (transient_failure)
                    *transient_failure = true;
            }
            return true;
        }
        if (PyErr_Occurred())
            PyErr_Clear();
        const int code = mt5_last_error_code(mt5);
        if (diagnostics && code != 1)
            diagnostics->last_mt5_error = code;
        if (!is_transient_read_error(code)) {
            mark_fetch_fatal();
            if (diagnostics)
                diagnostics->history_warmup_detected = 0;
            set_error("MetaTrader5 tick request failed");
            return false;
        }
        if (attempt < max_attempts) {
            if (diagnostics)
                diagnostics->history_warmup_detected = 1;
            if (is_ipc_error(code) && reinitialize_terminal(mt5) && diagnostics)
                ++diagnostics->reconnects;
            if (diagnostics)
                ++diagnostics->retries;
            std::this_thread::sleep_for(std::chrono::milliseconds(50u << (attempt - 1)));
        } else {
            if (transient_failure)
                *transient_failure = true;
            if (diagnostics)
                diagnostics->history_warmup_detected = 1;
        }
    }
    return false;
}

/// \brief Progressively warms a deep tick-history request before pagination.
/// \param request Symbol and inclusive historical range.
/// \param flags Effective COPY_TICKS_* filter.
/// \param[in,out] diagnostics Recovery counters to update.
/// \return True when the requested frontier was positively observed and confirmed,
/// or when no bootstrap is needed.
/// \note Probe rows are deliberately discarded; the normal lossless paginator remains
/// authoritative for the returned range and its exact boundary multiplicity.
/// \warning The bounded probe budget fails closed when the terminal keeps returning the
/// same oldest observable tick without moving the synchronization frontier.
bool bootstrap_tick_history(const Mt5TicksRequest *request, int flags,
                            Mt5FetchDiagnostics *diagnostics) {
    if (!request || request->to_msc - request->from_msc < kTickBootstrapTriggerMs)
        return true;

    const auto now = std::chrono::system_clock::now();
    const auto now_msc = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()).count();
    int64_t search_cursor = std::min(request->to_msc, static_cast<int64_t>(now_msc));
    if (search_cursor <= request->from_msc)
        return true;

    int64_t step = kTickBootstrapInitialStepMs;
    uint32_t stalled = 0;
    // A clean empty response may move the search cursor, but it is never
    // synchronization evidence.  Only a positive tick observation can move
    // this confirmed frontier and make a target-anchor confirmation possible.
    std::optional<int64_t> confirmed_frontier;
    std::optional<int64_t> target_confirmation;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::chrono::milliseconds backoff(100);
    for (uint32_t probe_no = 0; probe_no < kTickBootstrapProbeLimit; ++probe_no) {
        if (std::chrono::steady_clock::now() >= deadline) {
            set_error("MetaTrader tick history bootstrap time budget exhausted");
            return false;
        }
        const int64_t distance = std::max<int64_t>(0, search_cursor - request->from_msc);
        const int64_t effective_step = std::min(step, distance);
        const int64_t candidate = search_cursor - effective_step;
        std::vector<Mt5Tick> probe;
        bool partial = false;
        bool transient_failure = false;
        bool probe_ok = false;
        {
            RuntimeCallAdmission call;
            if (!call) {
                mark_fetch_fatal();
                return false;
            }
            GilScope gil(true);
            PyRef mt5(PyImport_ImportModule("MetaTrader5"));
            if (!mt5) {
                mark_fetch_fatal();
                set_python_error();
                return false;
            }
            // One physical probe is deliberately scoped to one runtime call.
            // Backoff and the next probe happen after the admission/GIL scope.
            probe_ok = copy_ticks_page(mt5.get(), request->symbol_utf8, candidate, flags, 1,
                                       &probe, diagnostics, &partial, &transient_failure, 1);
        }
        if (!probe_ok && !transient_failure)
            return false;
        // CopyTicksFrom may return rows older than the requested anchor.  Such
        // rows are not positive evidence that this probe reached its candidate.
        std::vector<Mt5Tick> valid_probe;
        valid_probe.reserve(probe.size());
        for (const auto &tick : probe) {
            if (tick.time_msc >= candidate)
                valid_probe.push_back(tick);
        }
        if (!probe_ok || partial) {
            if (diagnostics)
                diagnostics->history_warmup_detected = 1;
            target_confirmation.reset();
            ++stalled;
        } else if (valid_probe.empty()) {
            // A clean empty response may move only the bounded search cursor.
            // It does not prove that the candidate is covered: MT5 can return
            // an empty page while its local history is still warming up.
            if (diagnostics)
                diagnostics->history_warmup_detected = 1;
            target_confirmation.reset();
            if (candidate < search_cursor) {
                search_cursor = candidate;
                stalled = 0;
                if (step < kTickBootstrapInitialStepMs) {
                    const int64_t doubled = step > kTickBootstrapInitialStepMs / 2
                                                ? kTickBootstrapInitialStepMs
                                                : step * 2;
                    step = std::min(kTickBootstrapInitialStepMs, doubled);
                }
                backoff = std::chrono::milliseconds(100);
            } else {
                ++stalled;
            }
        } else {
            int64_t oldest = std::numeric_limits<int64_t>::max();
            for (const auto &tick : valid_probe)
                oldest = std::min(oldest, tick.time_msc);
            confirmed_frontier = confirmed_frontier
                                     ? std::min(*confirmed_frontier, oldest)
                                     : std::optional<int64_t>(oldest);
            if (oldest < search_cursor) {
                if (diagnostics)
                    diagnostics->history_warmup_detected = 1;
                search_cursor = oldest;
                stalled = 0;
                if (oldest <= request->from_msc) {
                    search_cursor = request->from_msc;
                    target_confirmation = oldest;
                } else if (candidate == request->from_msc) {
                    // The first available tick may legitimately be later than
                    // the requested start (weekend/holiday gap).  Confirm
                    // that same oldest tick at the requested anchor before
                    // allowing pagination to claim the frontier was reached.
                    target_confirmation = oldest;
                } else {
                    target_confirmation.reset();
                }
                if (step < kTickBootstrapInitialStepMs) {
                    const int64_t doubled = step > kTickBootstrapInitialStepMs / 2
                                                ? kTickBootstrapInitialStepMs
                                                : step * 2;
                    step = std::min(kTickBootstrapInitialStepMs, doubled);
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    set_error("MetaTrader tick history bootstrap time budget exhausted");
                    return false;
                }
                std::this_thread::sleep_for(backoff);
                backoff = std::chrono::milliseconds(100);
                continue;
            }

            if (diagnostics)
                diagnostics->history_warmup_detected = 1;
            if (candidate == request->from_msc) {
                const bool bounded_anchor_gap =
                    oldest >= request->from_msc &&
                    oldest - request->from_msc <= kTickBootstrapMaximumAnchorGapMs;
                if (bounded_anchor_gap) {
                    if (confirmed_frontier && target_confirmation &&
                        *target_confirmation == oldest)
                        return true;
                    target_confirmation = oldest;
                } else {
                    target_confirmation.reset();
                }
            } else {
                target_confirmation.reset();
            }
            ++stalled;
        }

        if (step > kTickBootstrapMinimumStepMs) {
            step = std::max(kTickBootstrapMinimumStepMs, step / 2);
            stalled = 0;
        }
        if (step == kTickBootstrapMinimumStepMs && stalled >= kTickBootstrapNoProgressLimit) {
            set_error("MetaTrader tick history bootstrap made no progress");
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            set_error("MetaTrader tick history bootstrap time budget exhausted");
            return false;
        }
        std::this_thread::sleep_for(backoff);
        backoff = std::min(std::chrono::milliseconds(1000), backoff * 2);
    }

    set_error("MetaTrader tick history bootstrap probe budget exhausted");
    return false;
}

/// \brief Visits an inclusive tick range using lossless, bounded pagination.
/// \tparam Consumer Callable accepting `(const Mt5Tick*, size_t)` and returning int.
/// \param request Symbol, inclusive range, and tick flags.
/// \param[in,out] diagnostics Optional recovery counters to update.
/// \param consume Consumer invoked outside the Python GIL critical section.
/// \return True after complete traversal; false on fetch, progress, or consumer failure.
template <typename Consumer>
bool visit_ticks_range(const Mt5TicksRequest *request, Mt5FetchDiagnostics *diagnostics,
                       Consumer consume) {
    int flags = 0;
    {
        RuntimeCallAdmission call;
        if (!call) {
            mark_fetch_fatal();
            return false;
        }
        GilScope gil(true);
        PyRef mt5(PyImport_ImportModule("MetaTrader5"));
        if (!mt5) {
            mark_fetch_fatal();
            set_python_error();
            return false;
        }
        flags = resolve_tick_flags(mt5.get(), request->flags);
    }
    if (!bootstrap_tick_history(request, flags, diagnostics))
        return false;

    int64_t cursor_msc = request->from_msc;
    std::unordered_map<std::string, std::size_t> consumed_boundary;
    std::size_t consumed_boundary_count = 0;
    uint32_t short_page_confirmations = 0;
    uint32_t partial_page_count = 0;
    for (;;) {
        std::vector<Mt5Tick> page;
        bool page_ok = false;
        bool partial_page = false;
        int page_size = kTickPageSize;
        if (consumed_boundary_count >
            static_cast<std::size_t>(std::numeric_limits<int>::max() - kTickPageSize)) {
            mark_fetch_fatal();
            set_error("too many ticks share one timestamp");
            return false;
        }
        page_size += static_cast<int>(consumed_boundary_count);
        {
            RuntimeCallAdmission call;
            if (!call) {
                mark_fetch_fatal();
                return false;
            }
            GilScope gil(true);
            PyRef mt5(PyImport_ImportModule("MetaTrader5"));
            if (!mt5) {
                mark_fetch_fatal();
                set_python_error();
                return false;
            }
            page_ok = copy_ticks_page(mt5.get(), request->symbol_utf8, cursor_msc, flags,
                                      page_size, &page, diagnostics, &partial_page);
        }
        if (!page_ok)
            return false;
        if (page.empty()) {
            if (partial_page) {
                set_error("MetaTrader returned no ticks during history recovery");
                return false;
            }
            if (short_page_confirmations >= kShortPageProbes)
                break;
            ++short_page_confirmations;
            std::this_thread::sleep_for(std::chrono::milliseconds(50u));
            continue;
        }
        if (partial_page) {
            // A non-empty page paired with a transient status is only a
            // provisional observation.  Do not publish it or move the cursor:
            // the next inclusive read must replay the same boundary after MT5
            // finishes synchronization.
            ++partial_page_count;
            if (partial_page_count > kPartialPageLimit) {
                set_error("MetaTrader history remained partial after recovery");
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50u));
            continue;
        }

        std::vector<Mt5Tick> deliver;
        deliver.reserve(page.size());
        const int64_t last_timestamp = page.back().time_msc;
        // Matching is destructive only for this page.  Keep the persistent
        // boundary intact so a third (or later) inclusive reread can still
        // match every occurrence that was consumed at this timestamp.
        auto remaining_consumed = consumed_boundary;
        std::unordered_map<std::string, std::size_t> next_boundary;
        std::size_t next_boundary_count = 0;
        for (const Mt5Tick &tick : page) {
            if (tick.time_msc < request->from_msc || tick.time_msc > request->to_msc)
                continue;
            if (tick.time_msc < cursor_msc)
                continue;
            if (tick.time_msc == cursor_msc) {
                const auto key = tick_payload_key(tick);
                auto it = remaining_consumed.find(key);
                if (it != remaining_consumed.end() && it->second != 0) {
                    --it->second;
                    continue;
                }
                // A boundary is retained only while traversal remains at the
                // same timestamp.  Late records at the old timestamp must be
                // delivered, but must not leak into the next timestamp's
                // boundary state.
                if (last_timestamp == cursor_msc) {
                    ++next_boundary[key];
                    ++next_boundary_count;
                }
            } else if (tick.time_msc == last_timestamp) {
                ++next_boundary[tick_payload_key(tick)];
                ++next_boundary_count;
            }
            deliver.push_back(tick);
        }
        const bool made_progress = !deliver.empty();
        if (!deliver.empty() && consume(deliver.data(), deliver.size()) != 0) {
            mark_fetch_fatal();
            return false;
        }

        // Confirmation probes prove stability only after a page with no new
        // accepted ticks. Any progress means history may still be warming up.
        if (made_progress)
            short_page_confirmations = 0;

        // copy_ticks_from is inclusive. Keep the timestamp and payload
        // multiplicity at its boundary; adding one millisecond would lose
        // tied ticks, while positional skipping would lose reordered ticks.
        const bool short_page = page.size() < static_cast<std::size_t>(page_size);
        if (last_timestamp < cursor_msc) {
            mark_fetch_fatal();
            set_error("MetaTrader returned ticks before the active cursor");
            return false;
        }
        if (last_timestamp == cursor_msc && next_boundary_count == 0) {
            if (short_page) {
                if (short_page_confirmations >= kShortPageProbes)
                    break;
                ++short_page_confirmations;
                std::this_thread::sleep_for(std::chrono::milliseconds(50u));
                continue;
            }
            mark_fetch_fatal();
            set_error("MetaTrader returned a non-progressing tick page");
            return false;
        }
        const bool stayed_on_boundary = last_timestamp == cursor_msc;
        cursor_msc = last_timestamp;
        if (stayed_on_boundary) {
            consumed_boundary_count += next_boundary_count;
            for (const auto &entry : next_boundary)
                consumed_boundary[entry.first] += entry.second;
        } else {
            consumed_boundary = std::move(next_boundary);
            consumed_boundary_count = next_boundary_count;
        }
        partial_page_count = 0;
        if (short_page) {
            if (last_timestamp > request->to_msc || short_page_confirmations >= kShortPageProbes)
                break;
            ++short_page_confirmations;
            std::this_thread::sleep_for(std::chrono::milliseconds(50u));
        } else {
            short_page_confirmations = 0;
        }
    }
    return true;
}

/// \brief Polls all active physical realtime sources while the runtime is alive.
std::string realtime_source_key(const char *symbol, uint32_t flags) {
    return std::string(symbol) + "\n" + std::to_string(flags);
}

std::string tick_payload_key(const Mt5Tick &tick) {
    return std::string(reinterpret_cast<const char *>(&tick), sizeof(tick));
}

void realtime_poller() try {
    for (;;) {
        std::vector<std::shared_ptr<RealtimeSource>> sources;
        std::chrono::steady_clock::time_point wake_at{};
        {
            std::unique_lock<std::mutex> lock(g_mutex);
            if (g_poller_stop || g_runtime_state != RuntimeState::running)
                return;
            if (g_realtime_sources.empty()) {
                g_poller_cv.wait(lock, [] {
                    return g_poller_stop || g_runtime_state != RuntimeState::running ||
                           !g_realtime_sources.empty();
                });
                continue;
            }
            const auto now = std::chrono::steady_clock::now();
            wake_at = now + std::chrono::hours(24);
            for (const auto &entry : g_realtime_sources) {
                if (entry.second->status == MT5_SUBSCRIPTION_FAILED)
                    continue;
                sources.push_back(entry.second);
                auto due = entry.second->next_poll;
                if (entry.second->retry_at != std::chrono::steady_clock::time_point{})
                    due = std::max(due, entry.second->retry_at);
                wake_at = std::min(wake_at, due);
            }
            g_poller_cv.wait_until(lock, wake_at);
            if (g_poller_stop || g_runtime_state != RuntimeState::running)
                return;
        }

        // Snapshot each source under the state mutex, then perform the potentially slow
        // Python/IPC work without blocking unsubscribe, diagnostics, or process_events.
        for (const auto &source : sources) {
            const auto now = std::chrono::steady_clock::now();
            std::string symbol;
            uint32_t flags = 0, interval = 250, max_batch = 1024, capacity = 64;
            int64_t cursor = -1, overlap_ms = 1000;
            std::vector<Mt5Tick> old_overlap;
            std::vector<Mt5Tick> boundary_snapshot;
            {
                std::lock_guard<std::mutex> lock(g_mutex);
                if (g_runtime_state != RuntimeState::running || now < source->retry_at || now < source->next_poll)
                    continue;
                source->next_poll = now + std::chrono::milliseconds(source->interval_ms);
                symbol = source->symbol; flags = source->flags; interval = source->interval_ms;
                max_batch = source->max_batch; capacity = source->capacity;
                const bool use_observed = source->observed_valid && !source->recovery_from_committed;
                cursor = use_observed ? source->observed_cursor_time_msc : source->cursor_time_msc;
                boundary_snapshot = use_observed ? source->observed_boundary : source->cursor_boundary;
                overlap_ms = source->overlap_ms; old_overlap = source->overlap_snapshot;
            }

            std::vector<Mt5Tick> forward;
            std::vector<Mt5Tick> reconcile;
            int error_code = 1;
            int failure_code = 0;
            bool partial = false;
            bool upstream_partial = false;
            bool budget_exhausted = false;
            bool fatal = false;
            bool ok = true;
            bool reconnected = false;
            bool reconcile_complete = false;
            bool forward_complete = false;
            std::vector<Mt5Tick> forward_boundary;
            const auto poll_started = std::chrono::steady_clock::now();
            {
                RuntimeCallAdmission call;
                if (!call) {
                    ok = false;
                    error_code = 0;
                } else {
                    GilScope gil(true);
                    PyRef mt5(PyImport_ImportModule("MetaTrader5"));
                    if (!mt5) {
                        PyErr_Clear(); ok = false; fatal = true; error_code = 0;
                    } else {
                        const int64_t now_msc = static_cast<int64_t>(std::chrono::duration_cast<
                            std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
                        int64_t page_from = cursor >= 0 ? cursor :
                            (source->initial_from_msc >= 0 ? source->initial_from_msc
                                                           : std::max<int64_t>(0, now_msc - static_cast<int64_t>(interval) * 4));
                        std::unordered_map<std::string, std::size_t> consumed_boundary;
                        for (const auto &tick : boundary_snapshot)
                            ++consumed_boundary[tick_payload_key(tick)];
                        std::size_t consumed_boundary_count = boundary_snapshot.size();
                        std::vector<Mt5Tick> boundary_records = boundary_snapshot;
                        const uint32_t physical_page_size = std::max<uint32_t>(1024u, max_batch);
                        for (uint32_t page_no = 0; page_no < 100000u; ++page_no) {
                            const uint64_t requested = static_cast<uint64_t>(physical_page_size) +
                                consumed_boundary_count;
                            const int count = static_cast<int>(std::min<uint64_t>(requested,
                                static_cast<uint64_t>(std::numeric_limits<int>::max())));
                            PyRef from(make_datetime(page_from));
                            PyRef ticks(from ? PyObject_CallMethod(mt5.get(), "copy_ticks_from", "sOii",
                                symbol.c_str(), from.get(), count, static_cast<int>(flags)) : nullptr);
                            if (!ticks || ticks.get() == Py_None) {
                                PyErr_Clear(); error_code = mt5_last_error_code(mt5.get());
                                failure_code = error_code;
                                partial = is_transient_read_error(error_code);
                                upstream_partial = partial;
                                ok = false; break;
                            }
                            std::vector<Mt5Tick> page;
                            if (!copy_ticks_array(ticks.get(), &page)) { PyErr_Clear(); ok = false; fatal = true; error_code = 0; failure_code = 0; break; }
                            error_code = mt5_last_error_code(mt5.get());
                            if (error_code != 1 && is_transient_read_error(error_code)) { failure_code = error_code; upstream_partial = true; }
                            if (is_partial_read_error(error_code)) partial = true;
                            if (page.empty()) { forward_complete = true; break; }
                            // copy_ticks_from() has no upper time bound.  Rows
                            // newer than this poll's snapshot may therefore be
                            // present in the response, but they must not advance
                            // the committed cursor.
                            bool saw_after_snapshot = false;
                            int64_t last_ts = page_from - 1;
                            for (const auto &tick : page) {
                                if (tick.time_msc > now_msc) {
                                    saw_after_snapshot = true;
                                    continue;
                                }
                                if (tick.time_msc >= page_from)
                                    last_ts = std::max(last_ts, tick.time_msc);
                            }
                            // Keep the persistent boundary immutable while this
                            // page is matched.  Only the temporary copy is
                            // decremented; otherwise repeated inclusive reads
                            // lose the original multiplicities.
                            auto remaining_consumed = consumed_boundary;
                            std::unordered_map<std::string, std::size_t> new_boundary;
                            std::size_t new_boundary_count = 0;
                            std::vector<Mt5Tick> new_boundary_records;
                            for (const auto &tick : page) {
                                if (tick.time_msc < page_from || tick.time_msc > now_msc) continue;
                                if (tick.time_msc == page_from) {
                                    const auto key = tick_payload_key(tick);
                                    auto it = remaining_consumed.find(key);
                                    if (it != remaining_consumed.end() && it->second != 0) {
                                        --it->second;
                                        continue;
                                    }
                                    // Retain only the current timestamp as the
                                    // next boundary.  A late insert at the old
                                    // timestamp is delivered but must not be
                                    // mixed with a newer boundary.
                                    if (last_ts == page_from) {
                                        ++new_boundary[key];
                                        ++new_boundary_count;
                                        new_boundary_records.push_back(tick);
                                    }
                                } else if (tick.time_msc == last_ts) {
                                    ++new_boundary[tick_payload_key(tick)];
                                    ++new_boundary_count;
                                    new_boundary_records.push_back(tick);
                                }
                                forward.push_back(tick);
                                if (forward.size() >= kRealtimeEpochTickLimit) {
                                    partial = true;
                                    budget_exhausted = true;
                                    break;
                                }
                            }
                            if (forward.size() >= kRealtimeEpochTickLimit) break;
                            if (last_ts < page_from) {
                                if (saw_after_snapshot)
                                    forward_complete = true;
                                break;
                            }
                            if (last_ts == page_from && new_boundary_count == 0) {
                                // The inclusive boundary has been fully consumed;
                                // a full page with no new payload multiplicity is
                                // a successful end of traversal, not a partial
                                // recovery state.
                                forward_complete = true;
                                break;
                            }
                            const bool stayed_on_boundary = last_ts == page_from;
                            if (stayed_on_boundary) {
                                consumed_boundary_count += new_boundary_count;
                                for (const auto &entry : new_boundary)
                                    consumed_boundary[entry.first] += entry.second;
                                boundary_records.insert(boundary_records.end(),
                                    new_boundary_records.begin(), new_boundary_records.end());
                            } else {
                                page_from = last_ts;
                                consumed_boundary = std::move(new_boundary);
                                consumed_boundary_count = new_boundary_count;
                                boundary_records = std::move(new_boundary_records);
                            }
                            if (saw_after_snapshot || page.size() < static_cast<std::size_t>(count)) {
                                forward_complete = true;
                                break;
                            }
                        }
                        forward_boundary = std::move(boundary_records);
                        const int64_t overlap_from = std::max<int64_t>(
                            source->initial_from_msc, std::max<int64_t>(0, now_msc - overlap_ms));
                        int64_t tail_from = overlap_from;
                        std::unordered_map<std::string, std::size_t> tail_boundary;
                        std::size_t tail_boundary_count = 0;
                        for (uint32_t page_no = 0; page_no < 100000u; ++page_no) {
                            const uint64_t requested = static_cast<uint64_t>(physical_page_size) +
                                tail_boundary_count;
                            const int count = static_cast<int>(std::min<uint64_t>(requested,
                                static_cast<uint64_t>(std::numeric_limits<int>::max())));
                            PyRef from(make_datetime(tail_from));
                            PyRef tail(from ? PyObject_CallMethod(mt5.get(), "copy_ticks_from", "sOii",
                                symbol.c_str(), from.get(), count, static_cast<int>(flags)) : nullptr);
                            if (!tail || tail.get() == Py_None) {
                                PyErr_Clear();
                                error_code = mt5_last_error_code(mt5.get());
                                failure_code = error_code;
                                partial = is_transient_read_error(error_code);
                                upstream_partial = upstream_partial || partial;
                                ok = false;
                                break;
                            }
                            std::vector<Mt5Tick> page;
                            if (!copy_ticks_array(tail.get(), &page)) { PyErr_Clear(); ok = false; fatal = true; error_code = 0; failure_code = 0; break; }
                            error_code = mt5_last_error_code(mt5.get());
                            if (error_code != 1 && is_transient_read_error(error_code)) { failure_code = error_code; upstream_partial = true; }
                            if (is_partial_read_error(error_code)) partial = true;
                            if (page.empty()) { reconcile_complete = true; break; }
                            const int64_t last_ts = page.back().time_msc;
                            auto remaining_boundary = tail_boundary;
                            std::unordered_map<std::string, std::size_t> next_boundary;
                            std::size_t next_boundary_count = 0;
                            bool delivered_new_boundary = false;
                            for (const auto &tick : page) {
                                if (tick.time_msc < overlap_from || tick.time_msc > now_msc) continue;
                                if (tick.time_msc < tail_from) continue;
                                if (tick.time_msc == tail_from) {
                                    const auto key = tick_payload_key(tick);
                                    auto it = remaining_boundary.find(key);
                                    if (it != remaining_boundary.end() && it->second != 0) {
                                        --it->second;
                                        continue;
                                    }
                                    if (last_ts == tail_from) {
                                        ++next_boundary[key];
                                        ++next_boundary_count;
                                        delivered_new_boundary = true;
                                    }
                                } else if (tick.time_msc == last_ts) {
                                    ++next_boundary[tick_payload_key(tick)];
                                    ++next_boundary_count;
                                    delivered_new_boundary = true;
                                }
                                reconcile.push_back(tick);
                                if (reconcile.size() >= kRealtimeEpochTickLimit) {
                                    partial = true;
                                    budget_exhausted = true;
                                    break;
                                }
                            }
                            if (reconcile.size() >= kRealtimeEpochTickLimit)
                                break;
                            if (last_ts < tail_from) {
                                partial = true;
                                upstream_partial = true;
                                reconcile_complete = false;
                                break;
                            }
                            if (last_ts == tail_from && !delivered_new_boundary) {
                                // The inclusive boundary has been completely
                                // consumed; there is no further page to seek.
                                reconcile_complete = true;
                                break;
                            }
                            const bool stayed_on_boundary = last_ts == tail_from;
                            if (stayed_on_boundary) {
                                tail_boundary_count += next_boundary_count;
                                for (const auto &entry : next_boundary)
                                    tail_boundary[entry.first] += entry.second;
                            } else {
                                tail_from = last_ts;
                                tail_boundary = std::move(next_boundary);
                                tail_boundary_count = next_boundary_count;
                            }
                            if (page.size() < static_cast<std::size_t>(count) || last_ts > now_msc) {
                                reconcile_complete = true;
                                break;
                            }
                        }
                        if (is_ipc_error(failure_code) && reinitialize_terminal(mt5.get()))
                            reconnected = true;
                    }
                }
            }
            const auto duration_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - poll_started).count());

            std::lock_guard<std::mutex> lock(g_mutex);
            if (g_runtime_state != RuntimeState::running)
                continue;
            auto current = g_realtime_sources.find(realtime_source_key(symbol.c_str(), flags));
            if (current == g_realtime_sources.end() || current->second.get() != source.get())
                continue;
            source->diagnostics.poll_duration_us = duration_us;
            source->diagnostics.last_mt5_error = failure_code != 0 ? failure_code : (error_code == 1 ? 0 : error_code);
            if (reconnected)
                ++source->diagnostics.reconnects;
            if (fatal) {
                source->status = MT5_SUBSCRIPTION_FAILED;
                source->diagnostics.status = MT5_SUBSCRIPTION_FAILED;
                source->retry_at = std::chrono::steady_clock::time_point{};
                continue;
            }
            if (!ok && forward.empty() && reconcile.empty()) {
                ++source->consecutive_failures;
                source->diagnostics.consecutive_failures = source->consecutive_failures;
                source->status = MT5_SUBSCRIPTION_RECONNECTING;
                source->diagnostics.status = source->status;
                const uint32_t shift = std::min<uint32_t>(source->consecutive_failures, 5u);
                source->retry_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::min<uint32_t>(5000u, 100u << shift));
                continue;
            }
            if (!ok || !forward_complete)
                partial = true;
            const bool requires_committed_replay =
                upstream_partial || (!forward_complete && !budget_exhausted);
            if (requires_committed_replay)
                source->recovery_from_committed = true;
            if (partial) {
                if (!budget_exhausted)
                    ++source->consecutive_failures;
                source->diagnostics.consecutive_failures = source->consecutive_failures;
                source->status = MT5_SUBSCRIPTION_RECONNECTING;
                source->diagnostics.status = source->status;
                if (budget_exhausted)
                    source->retry_at = std::chrono::steady_clock::now();
                else {
                    const uint32_t shift = std::min<uint32_t>(source->consecutive_failures, 5u);
                    source->retry_at = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(std::min<uint32_t>(5000u, 100u << shift));
                }
            } else {
                source->consecutive_failures = 0;
                source->diagnostics.consecutive_failures = 0;
                source->retry_at = std::chrono::steady_clock::time_point{};
                source->status = MT5_SUBSCRIPTION_READY;
                source->diagnostics.status = source->status;
                source->recovery_from_committed = false;
            }

            std::unordered_map<std::string, std::size_t> previous;
            const int64_t current_now_msc = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
            const int64_t current_overlap_from = std::max<int64_t>(
                source->initial_from_msc, std::max<int64_t>(0, current_now_msc - overlap_ms));
            for (const auto &tick : old_overlap)
                if (tick.time_msc >= current_overlap_from)
                    ++previous[tick_payload_key(tick)];
            // The committed boundary is a delivery fact, even when MT5 has
            // not returned a complete overlap snapshot for this epoch. Include
            // it in reconciliation so a multiplicity increase (A×2 -> A×3)
            // contributes only the genuinely new occurrence.
            std::unordered_map<std::string, std::size_t> boundary_counts;
            for (const auto &tick : source->cursor_boundary)
                if (tick.time_msc >= current_overlap_from)
                    ++boundary_counts[tick_payload_key(tick)];
            for (const auto &entry : boundary_counts)
                previous[entry.first] = std::max(previous[entry.first], entry.second);
            std::unordered_map<std::string, std::size_t> forward_keys;
            for (const auto &tick : forward) ++forward_keys[tick_payload_key(tick)];
            std::vector<Mt5Tick> fresh;
            // Forward traversal starts at the committed timestamp and payload
            // boundary, so its records are already distinct from committed
            // delivery.  Do
            // not deduplicate against the overlap snapshot here: that snapshot
            // contains observed records, including ticks from an earlier partial
            // epoch that may never have reached a consumer.  A committed replay
            // must deliver those records again rather than silently losing them.
            fresh = std::move(forward);
            bool rewrite = false;
            for (const auto &tick : reconcile) {
                const auto key = tick_payload_key(tick);
                auto it = previous.find(key);
                if (it != previous.end() && it->second != 0) { --it->second; continue; }
                auto fit = forward_keys.find(key);
                if (fit != forward_keys.end() && fit->second != 0) { --fit->second; continue; }
                if (cursor >= 0 && tick.time_msc <= cursor) {
                    rewrite = true;
                    // Preserve late inserts for consumers in an explicit recovery batch.
                    fresh.push_back(tick);
                } else {
                    fresh.push_back(tick);
                }
            }
            if (reconcile_complete)
                for (const auto &entry : previous)
                    if (entry.second != 0) rewrite = true;
            // Reconciliation may discover additional provisional records.  Do
            // not publish any of them when the epoch must replay from commit.
            if (requires_committed_replay)
                fresh.clear();
            if (rewrite) {
                ++source->diagnostics.history_rewrites;
                source->diagnostics.gap_reason = MT5_GAP_SOURCE_INCONSISTENCY;
                ++source->inconsistency_epoch;
                const int64_t rewrite_from = cursor >= 0 ? std::max<int64_t>(0, cursor - overlap_ms) : 0;
                const int64_t rewrite_to = cursor;
                source->recovery_from_msc = source->recovery_from_msc >= 0
                    ? std::min(source->recovery_from_msc, rewrite_from) : rewrite_from;
                source->recovery_to_msc = std::max(source->recovery_to_msc, rewrite_to);
            }
            if (reconcile_complete)
                source->overlap_snapshot = std::move(reconcile);
            // Do not publish provisional upstream data.  The next clean replay
            // starts from the committed cursor and delivers each tick once.
            const auto observed = !source->overlap_snapshot.empty() ? source->overlap_snapshot.back().time_msc :
                (!fresh.empty() ? fresh.back().time_msc : -1);
            if (observed >= 0) {
                const int64_t now_msc = static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
                source->diagnostics.history_lag_ms = std::max<int64_t>(0, now_msc - observed);
            }
            if (!fresh.empty()) {
                const bool grow_overlap = partial || reconnected || rewrite ||
                    source->diagnostics.history_lag_ms > source->overlap_ms;
                if (grow_overlap)
                    source->overlap_ms = std::min<int64_t>(kRealtimeMaxOverlapMs, source->overlap_ms * 2);
                else if (!source->overlap_snapshot.empty() && source->overlap_ms > kRealtimeMinOverlapMs)
                    source->overlap_ms = std::max<int64_t>(kRealtimeMinOverlapMs, source->overlap_ms / 2);
                for (std::size_t offset = 0; offset < fresh.size();) {
                    const std::size_t count = std::min<std::size_t>(max_batch, fresh.size() - offset);
                    RealtimeBatch batch;
                    batch.sequence = source->next_sequence++;
                    batch.ticks.insert(batch.ticks.end(), fresh.begin() + offset,
                                       fresh.begin() + offset + count);
                    source->ring.push_back(std::move(batch));
                    while (source->ring.size() > source->capacity) source->ring.pop_front();
                    offset += count;
                }
            }
            if (!forward_boundary.empty()) {
                // forward_boundary is maintained as a single-timestamp
                // payload multiset.  Late inserts from an older timestamp
                // may be delivered in the same page, but they must never
                // become part of the new cursor or make it move backwards.
                const int64_t boundary_time = forward_boundary.front().time_msc;
                source->observed_cursor_time_msc = boundary_time;
                source->observed_boundary = forward_boundary;
                source->observed_valid = true;
                if (!partial && forward_complete) {
                    source->cursor_time_msc = boundary_time;
                    source->cursor_boundary = forward_boundary;
                }
            }
        }
    }
} catch (...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto &entry : g_realtime_sources) {
        entry.second->status = MT5_SUBSCRIPTION_FAILED;
        entry.second->diagnostics.status = MT5_SUBSCRIPTION_FAILED;
    }
}

} // namespace

extern "C" {

MT5BRIDGE_EXPORT uint32_t mt5bridge_abi_version() { return MT5BRIDGE_ABI_VERSION; }

MT5BRIDGE_EXPORT uint32_t mt5bridge_trade_api_version() {
    return MT5BRIDGE_TRADE_API_VERSION;
}

MT5BRIDGE_API int mt5bridge_account_info(Mt5AccountInfo *info) try {
    clear_error();
    if (!info) {
        set_error("info is required");
        return -1;
    }
    *info = Mt5AccountInfo{};
    RuntimeCallAdmission call(RuntimeCallLane::trade_critical);
    if (!call)
        return -1;
    GilScope gil(true);
    PyRef mt5(PyImport_ImportModule("MetaTrader5"));
    if (!mt5) {
        set_python_error();
        return -1;
    }
    PyRef snapshot(PyObject_CallMethod(mt5.get(), "account_info", nullptr));
    if (!snapshot) {
        set_python_error();
        return -1;
    }
    if (!copy_account_info(snapshot.get(), info))
        return -1;
    return 0;
} catch (...) {
    set_current_exception_error();
    return -1;
}

MT5BRIDGE_API int mt5bridge_symbol_capabilities(
    const Mt5SymbolRequest *request, Mt5SymbolCapabilities *capabilities) try {
    clear_error();
    if (!request || !capabilities || !request->symbol_utf8 || !*request->symbol_utf8) {
        set_error("valid symbol request and capabilities are required");
        return -1;
    }
    *capabilities = Mt5SymbolCapabilities{};
    if (request->reserved != 0) {
        set_error("symbol request reserved fields must be zero");
        return -1;
    }
    RuntimeCallAdmission call(RuntimeCallLane::trade_critical);
    if (!call)
        return -1;
    GilScope gil(true);
    PyRef mt5(PyImport_ImportModule("MetaTrader5"));
    if (!mt5) {
        set_python_error();
        return -1;
    }
    PyRef snapshot(PyObject_CallMethod(mt5.get(), "symbol_info", "s", request->symbol_utf8));
    if (!snapshot) {
        set_python_error();
        return -1;
    }
    return copy_symbol_capabilities(snapshot.get(), mt5.get(), capabilities) ? 0 : -1;
} catch (...) {
    set_current_exception_error();
    return -1;
}

MT5BRIDGE_API int mt5bridge_order_check(const Mt5OrderCheckRequest *request,
                                        Mt5OrderCheckResult *result) try {
    clear_error();
    if (!request || !result) {
        set_error("valid order_check request and result are required");
        return -1;
    }
    *result = Mt5OrderCheckResult{};
    if (request->reserved[0] != 0 || request->reserved[1] != 0 || request->reserved[2] != 0) {
        set_error("order_check reserved fields must be zero");
        return -1;
    }
    if (!std::isfinite(request->volume) || !std::isfinite(request->price) ||
         !std::isfinite(request->stoplimit) || !std::isfinite(request->sl) ||
         !std::isfinite(request->tp)) {
        set_error("valid order_check request and result are required");
        return -1;
    }
    RuntimeCallAdmission call(RuntimeCallLane::trade_critical);
    if (!call)
        return -1;
    GilScope gil(true);
    PyRef mt5(PyImport_ImportModule("MetaTrader5"));
    if (!mt5) {
        set_python_error();
        return -1;
    }
    PyRef request_dict(make_order_check_request(request));
    if (!request_dict) {
        if (PyErr_Occurred())
            set_python_error();
        else
            set_error("failed to build order_check request");
        return -1;
    }
    PyRef checked(PyObject_CallMethod(mt5.get(), "order_check", "O", request_dict.get()));
    if (!checked) {
        set_python_error();
        return -1;
    }
    return copy_order_check_result(checked.get(), result) ? 0 : -1;
} catch (...) {
    set_current_exception_error();
    return -1;
}

MT5BRIDGE_API int mt5bridge_query_orders(const Mt5OrdersRequest *request,
                                         Mt5OrderBuffer **result) try {
    if (result)
        *result = nullptr;
    clear_error();
    if (!request || !result || request->reserved != 0) {
        set_error("valid orders request and result are required");
        return -1;
    }
    if (!valid_active_selector(request->symbol_utf8, request->group_utf8, request->ticket,
                               "orders_get"))
        return -1;
    RuntimeCallAdmission call(RuntimeCallLane::trade_critical);
    if (!call)
        return -1;
    GilScope gil(true);
    PyRef mt5(PyImport_ImportModule("MetaTrader5"));
    if (!mt5) {
        set_python_error();
        return -1;
    }
    PyRef rows(call_trade_collection(mt5.get(), "orders_get", request->symbol_utf8,
                                     request->group_utf8, request->ticket, 0, 0, false));
    if (!rows) {
        if (PyErr_Occurred())
            set_python_error();
        else if (g_last_error.empty())
            set_error("MetaTrader orders_get failed");
        return -1;
    }
    auto buffer = std::make_unique<Mt5OrderBuffer>();
    if (!copy_trade_collection(rows.get(), &buffer->values, copy_order_snapshot))
        return -1;
    *result = buffer.release();
    return 0;
} catch (...) {
    set_current_exception_error();
    return -1;
}

MT5BRIDGE_API const Mt5OrderSnapshot *mt5bridge_order_buffer_data(
    const Mt5OrderBuffer *buffer) {
    return buffer && !buffer->values.empty() ? buffer->values.data() : nullptr;
}

MT5BRIDGE_API size_t mt5bridge_order_buffer_size(const Mt5OrderBuffer *buffer) {
    return buffer ? buffer->values.size() : 0;
}

MT5BRIDGE_API void mt5bridge_order_buffer_free(Mt5OrderBuffer *buffer) { delete buffer; }

MT5BRIDGE_API int mt5bridge_query_positions(const Mt5PositionsRequest *request,
                                            Mt5PositionBuffer **result) try {
    if (result)
        *result = nullptr;
    clear_error();
    if (!request || !result || request->reserved != 0) {
        set_error("valid positions request and result are required");
        return -1;
    }
    if (!valid_active_selector(request->symbol_utf8, request->group_utf8, request->ticket,
                               "positions_get"))
        return -1;
    RuntimeCallAdmission call(RuntimeCallLane::trade_critical);
    if (!call)
        return -1;
    GilScope gil(true);
    PyRef mt5(PyImport_ImportModule("MetaTrader5"));
    if (!mt5) {
        set_python_error();
        return -1;
    }
    PyRef rows(call_trade_collection(mt5.get(), "positions_get", request->symbol_utf8,
                                     request->group_utf8, request->ticket, 0, 0, false));
    if (!rows) {
        if (PyErr_Occurred())
            set_python_error();
        else if (g_last_error.empty())
            set_error("MetaTrader positions_get failed");
        return -1;
    }
    auto buffer = std::make_unique<Mt5PositionBuffer>();
    if (!copy_trade_collection(rows.get(), &buffer->values, copy_position_snapshot))
        return -1;
    if (request->identifier != 0) {
        buffer->values.erase(
            std::remove_if(buffer->values.begin(), buffer->values.end(),
                           [identifier = request->identifier](const Mt5PositionSnapshot &value) {
                               return value.identifier != identifier;
                           }),
            buffer->values.end());
    }
    *result = buffer.release();
    return 0;
} catch (...) {
    set_current_exception_error();
    return -1;
}

MT5BRIDGE_API const Mt5PositionSnapshot *mt5bridge_position_buffer_data(
    const Mt5PositionBuffer *buffer) {
    return buffer && !buffer->values.empty() ? buffer->values.data() : nullptr;
}

MT5BRIDGE_API size_t mt5bridge_position_buffer_size(const Mt5PositionBuffer *buffer) {
    return buffer ? buffer->values.size() : 0;
}

MT5BRIDGE_API void mt5bridge_position_buffer_free(Mt5PositionBuffer *buffer) { delete buffer; }

MT5BRIDGE_API int mt5bridge_query_history_orders(const Mt5HistoryOrdersRequest *request,
                                                 Mt5HistoryOrderBuffer **result) try {
    if (result)
        *result = nullptr;
    clear_error();
    if (!request || !result || request->reserved != 0 || request->from_msc < 0 ||
        request->to_msc < request->from_msc) {
        set_error("valid history orders request and result are required");
        return -1;
    }
    HistoryQueryRange query_range;
    if (!make_history_query_range(request->from_msc, request->to_msc, &query_range))
        return -1;
    RuntimeCallAdmission call(RuntimeCallLane::trade_critical);
    if (!call)
        return -1;
    GilScope gil(true);
    PyRef mt5(PyImport_ImportModule("MetaTrader5"));
    if (!mt5) {
        set_python_error();
        return -1;
    }
    PyRef rows(call_trade_collection(mt5.get(), "history_orders_get", nullptr,
                                     request->group_utf8, 0, query_range.from_msc,
                                     query_range.to_msc, true));
    if (!rows) {
        if (PyErr_Occurred())
            set_python_error();
        else if (g_last_error.empty())
            set_error("MetaTrader history_orders_get failed");
        return -1;
    }
    auto buffer = std::make_unique<Mt5HistoryOrderBuffer>();
    if (!copy_trade_collection(rows.get(), &buffer->values, copy_order_snapshot))
        return -1;
    // MT5 selects history by whole-second completion time; restore the exact
    // inclusive millisecond contract before applying identity predicates.
    buffer->values.erase(
        std::remove_if(buffer->values.begin(), buffer->values.end(),
                       [request](const Mt5HistoryOrderSnapshot &value) {
                           return value.time_done_msc < request->from_msc ||
                                  value.time_done_msc > request->to_msc ||
                                  (request->order_ticket != 0 &&
                                   value.ticket != request->order_ticket) ||
                                  (request->position_id != 0 &&
                                   value.position_id != request->position_id);
                       }),
        buffer->values.end());
    *result = buffer.release();
    return 0;
} catch (...) {
    set_current_exception_error();
    return -1;
}

MT5BRIDGE_API const Mt5HistoryOrderSnapshot *mt5bridge_history_order_buffer_data(
    const Mt5HistoryOrderBuffer *buffer) {
    return buffer && !buffer->values.empty() ? buffer->values.data() : nullptr;
}

MT5BRIDGE_API size_t mt5bridge_history_order_buffer_size(
    const Mt5HistoryOrderBuffer *buffer) {
    return buffer ? buffer->values.size() : 0;
}

MT5BRIDGE_API void mt5bridge_history_order_buffer_free(Mt5HistoryOrderBuffer *buffer) {
    delete buffer;
}

MT5BRIDGE_API int mt5bridge_query_history_deals(const Mt5HistoryDealsRequest *request,
                                                Mt5DealBuffer **result) try {
    if (result)
        *result = nullptr;
    clear_error();
    if (!request || !result || request->reserved != 0 || request->from_msc < 0 ||
        request->to_msc < request->from_msc) {
        set_error("valid history deals request and result are required");
        return -1;
    }
    HistoryQueryRange query_range;
    if (!make_history_query_range(request->from_msc, request->to_msc, &query_range))
        return -1;
    RuntimeCallAdmission call(RuntimeCallLane::trade_critical);
    if (!call)
        return -1;
    GilScope gil(true);
    PyRef mt5(PyImport_ImportModule("MetaTrader5"));
    if (!mt5) {
        set_python_error();
        return -1;
    }
    PyRef rows(call_trade_collection(mt5.get(), "history_deals_get", nullptr,
                                     request->group_utf8, 0, query_range.from_msc,
                                     query_range.to_msc, true));
    if (!rows) {
        if (PyErr_Occurred())
            set_python_error();
        else if (g_last_error.empty())
            set_error("MetaTrader history_deals_get failed");
        return -1;
    }
    auto buffer = std::make_unique<Mt5DealBuffer>();
    if (!copy_trade_collection(rows.get(), &buffer->values, copy_deal_snapshot))
        return -1;
    // MT5 selects history by whole-second timestamps; restore the exact
    // inclusive millisecond contract before applying identity predicates.
    buffer->values.erase(
        std::remove_if(buffer->values.begin(), buffer->values.end(),
                       [request](const Mt5DealSnapshot &value) {
                           return value.time_msc < request->from_msc ||
                                  value.time_msc > request->to_msc ||
                                  (request->deal_ticket != 0 &&
                                   value.ticket != request->deal_ticket) ||
                                  (request->order_ticket != 0 &&
                                   value.order_ticket != request->order_ticket) ||
                                  (request->position_id != 0 &&
                                   value.position_id != request->position_id);
                       }),
        buffer->values.end());
    *result = buffer.release();
    return 0;
} catch (...) {
    set_current_exception_error();
    return -1;
}

MT5BRIDGE_API const Mt5DealSnapshot *mt5bridge_deal_buffer_data(const Mt5DealBuffer *buffer) {
    return buffer && !buffer->values.empty() ? buffer->values.data() : nullptr;
}

MT5BRIDGE_API size_t mt5bridge_deal_buffer_size(const Mt5DealBuffer *buffer) {
    return buffer ? buffer->values.size() : 0;
}

MT5BRIDGE_API void mt5bridge_deal_buffer_free(Mt5DealBuffer *buffer) { delete buffer; }

MT5BRIDGE_API int mt5bridge_initialize(const wchar_t *python_home) try {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::lock_guard<std::mutex> python_lock(g_python_mutex);
    clear_error();
    if (g_runtime_state == RuntimeState::shutting_down) {
        set_error("bridge is shutting down");
        return -1;
    }
    if (g_initialized)
        return 0;

    g_owns_interpreter = !Py_IsInitialized();
    if (g_owns_interpreter) {
        PyConfig config;
        PyConfig_InitPythonConfig(&config);
        PyStatus status = PyConfig_SetString(&config, &config.program_name, L"mt5bridge");
        if (!PyStatus_Exception(status) && python_home)
            status = PyConfig_SetString(&config, &config.home, python_home);
        if (!PyStatus_Exception(status))
            status = Py_InitializeFromConfig(&config);
        if (PyStatus_Exception(status)) {
            set_error(status.err_msg ? status.err_msg : "Py_InitializeFromConfig failed");
            PyConfig_Clear(&config);
            g_owns_interpreter = false;
            return -1;
        }
        PyConfig_Clear(&config);
        g_owner_thread = std::this_thread::get_id();
    }
    if (!Py_IsInitialized()) {
        set_error("Py_Initialize failed");
        g_owns_interpreter = false;
        return -1;
    }

    // Py_Initialize leaves the GIL held on the current (main) thread.  An
    // embedding application, on the other hand, may already own an
    // interpreter but not the GIL, so acquire it explicitly in that case.
    PyGILState_STATE external_gil{};
    const bool acquired_external_gil = !g_owns_interpreter;
    if (acquired_external_gil)
        external_gil = PyGILState_Ensure();
    PyRef mt5(PyImport_ImportModule("MetaTrader5"));
    if (!mt5) {
        set_python_error();
        mt5 = PyRef();
        if (g_owns_interpreter)
            Py_FinalizeEx();
        else
            PyGILState_Release(external_gil);
        g_owns_interpreter = false;
        return -1;
    }
    PyRef result(PyObject_CallMethod(mt5.get(), "initialize", nullptr));
    const int initialized = result ? PyObject_IsTrue(result.get()) : 0;
    if (!result || initialized <= 0) {
        if (!result || initialized < 0)
            set_python_error();
        else
            set_error("MetaTrader5 initialize failed");
        result = PyRef();
        mt5 = PyRef();
        if (g_owns_interpreter)
            Py_FinalizeEx();
        else
            PyGILState_Release(external_gil);
        g_owns_interpreter = false;
        return -1;
    }
    result = PyRef();
    mt5 = PyRef();
    if (g_owns_interpreter)
        g_main_thread_state = PyEval_SaveThread();
    else
        PyGILState_Release(external_gil);
    g_initialized = true;
    g_runtime_state = RuntimeState::running;
    ++g_runtime_generation;
    g_poller_stop = false;
    return 0;
} catch (...) {
    set_current_exception_error();
    return -1;
}

MT5BRIDGE_API int mt5bridge_shutdown() try {
    std::thread poller;
    {
    std::unique_lock<std::mutex> lock(g_mutex);
    clear_error();
    if (g_runtime_state == RuntimeState::shutting_down) {
        set_error("bridge shutdown already in progress");
        return -1;
    }
    if (!g_initialized)
        return 0;

    const bool wrong_owner_thread =
        g_owns_interpreter && std::this_thread::get_id() != g_owner_thread;
    if (wrong_owner_thread) {
        set_error("runtime shutdown requires initialize owner thread");
        return -1;
    }

    g_poller_stop = true;
    g_runtime_state = RuntimeState::shutting_down;
    g_poller_cv.notify_all();
    poller = std::move(g_poller);
    }
    if (poller.joinable())
        poller.join();
    std::unique_lock<std::mutex> lock(g_mutex);
    // New admissions are rejected by the shutting_down state above.  Waiting
    // on the condition variable releases g_mutex while an admitted call
    // finishes, so its destructor can decrement the in-flight count.
    g_runtime_calls_cv.wait(lock, [] {
        return g_active_runtime_calls == 0;
    });
    std::unique_lock<std::mutex> python_lock(g_python_mutex);

    int shutdown_status = 0;
    if (g_owns_interpreter) {
        // Restore the thread state saved immediately after Py_Initialize.
        // Finalizing while that state is detached is undefined behaviour.
        PyEval_RestoreThread(g_main_thread_state);
        g_main_thread_state = nullptr;
    }
    PyGILState_STATE external_gil{};
    const bool acquired_external_gil = !g_owns_interpreter;
    if (acquired_external_gil)
        external_gil = PyGILState_Ensure();
    {
        PyRef mt5(PyImport_ImportModule("MetaTrader5"));
        if (mt5) {
            PyRef result(PyObject_CallMethod(mt5.get(), "shutdown", nullptr));
            if (!result) {
                set_python_error();
                shutdown_status = -1;
            }
        } else {
            PyErr_Clear();
        }
    }
    if (g_owns_interpreter) {
        if (Py_FinalizeEx() != 0)
            shutdown_status = -1;
    } else {
        PyGILState_Release(external_gil);
    }
    g_owns_interpreter = false;
    g_initialized = false;
    g_runtime_state = RuntimeState::stopped;
    g_realtime_subscriptions.clear();
    g_subscription_order.clear();
    g_rr_cursor = 0;
    g_realtime_sources.clear();
    if (shutdown_status != 0 && g_last_error.empty())
        set_error("CPython finalization failed");
    return shutdown_status;
} catch (...) {
    set_current_exception_error();
    return -1;
}

MT5BRIDGE_API int mt5bridge_eval_json(const char *request_json, char **response_json) try {
    if (response_json)
        *response_json = nullptr;
    if (!response_json || !request_json) {
        set_error("request and response_json are required");
        return -1;
    }

    clear_error();
    RuntimeCallAdmission call;
    if (!call)
        return -1;

    GilScope gil(true);
    const int status = [&]() {
        PyRef json_module(PyImport_ImportModule("json"));
        PyRef mt5(PyImport_ImportModule("MetaTrader5"));
        PyRef loads(json_module ? PyObject_GetAttrString(json_module.get(), "loads") : nullptr);
        PyRef dumps(json_module ? PyObject_GetAttrString(json_module.get(), "dumps") : nullptr);
        if (!json_module || !mt5 || !loads || !dumps) {
            set_python_error();
            return -1;
        }

        PyRef request(PyObject_CallFunction(loads.get(), "s", request_json));
        if (!request || !PyDict_Check(request.get())) {
            if (PyErr_Occurred())
                set_python_error();
            else
                set_error("request must be a JSON object");
            return -1;
        }
        const char *method = nullptr;
        if (!read_string(request.get(), "method", &method))
            return -1;

        PyRef result;
        if (std::strcmp(method, "terminal_info") == 0) {
            result = PyRef(PyObject_CallMethod(mt5.get(), "terminal_info", nullptr));
        } else if (std::strcmp(method, "get_m1_bars") == 0) {
            const char *symbol = nullptr;
            int count = 0;
            PyRef timeframe(PyObject_GetAttrString(mt5.get(), "TIMEFRAME_M1"));
            if (!read_string(request.get(), "symbol", &symbol) ||
                !read_int(request.get(), "count", &count) || !timeframe) {
                if (PyErr_Occurred())
                    set_python_error();
            } else {
                result = PyRef(PyObject_CallMethod(mt5.get(), "copy_rates_from_pos", "sOii",
                                                   symbol, timeframe.get(), 0, count));
            }
        } else if (std::strcmp(method, "open_market_buy") == 0) {
            // Stage 1 deliberately exposes no unmanaged side-effecting send.
            // The old convenience method is rejected before touching MT5;
            // durable journal dispatch belongs to the Stage 2 implementation.
            set_error("open_market_buy is disabled until durable trade dispatch is implemented");
#if defined(MT5BRIDGE_INTERNAL_TESTING)
        } else if (std::strcmp(method, "test_dispatch_transport") == 0) {
            // This branch is test-only: it exercises the private Python adapter
            // through the existing runtime admission/GIL path without adding a
            // production C ABI order-send method.
            constexpr char request_payload[] =
                "{\"action\":1,\"type\":0,\"symbol\":\"EURUSD\","
                "\"volume\":0.01}";
            mt5bridge::OperationRecord record;
            record.key.account = mt5bridge::AccountKey{"Fake-Server", 42};
            record.key.trade_id = 1;
            record.key.operation_id = 1;
            record.request_payload.assign(
                reinterpret_cast<const std::uint8_t *>(request_payload),
                reinterpret_cast<const std::uint8_t *>(request_payload) +
                    std::strlen(request_payload));

            mt5bridge::runtime::Mt5PythonAccountProbe account_probe(mt5.get());
            mt5bridge::runtime::Mt5PythonDispatchTransport transport(
                mt5.get(), account_probe);
            const auto call = transport.submit_once(record);

            PyRef response(PyDict_New());
            if (!response) {
                set_python_error();
                return -1;
            }
            const char *status = "transport_failure";
            switch (call.status) {
            case mt5bridge::runtime::BackendCallStatus::broker_result:
                status = "broker_result";
                break;
            case mt5bridge::runtime::BackendCallStatus::account_mismatch:
                status = "account_mismatch";
                break;
            case mt5bridge::runtime::BackendCallStatus::transport_failure:
                status = "transport_failure";
                break;
            }
            const char *disposition = "reconciling";
            switch (call.disposition) {
            case mt5bridge::runtime::BrokerResultDisposition::accepted:
                disposition = "accepted";
                break;
            case mt5bridge::runtime::BrokerResultDisposition::rejected:
                disposition = "rejected";
                break;
            case mt5bridge::runtime::BrokerResultDisposition::reconciling:
                disposition = "reconciling";
                break;
            }
            PyRef status_value(PyUnicode_FromString(status));
            PyRef disposition_value(PyUnicode_FromString(disposition));
            PyRef retcode_value(PyLong_FromUnsignedLong(call.retcode));
            if (!status_value || !disposition_value || !retcode_value ||
                PyDict_SetItemString(response.get(), "status", status_value.get()) != 0 ||
                PyDict_SetItemString(response.get(), "disposition",
                                     disposition_value.get()) != 0 ||
                PyDict_SetItemString(response.get(), "retcode", retcode_value.get()) != 0) {
                set_python_error();
                return -1;
            }
            if (!call.raw_result.empty()) {
                PyRef raw(PyObject_CallFunction(
                    loads.get(), "s#", call.raw_result.data(),
                    static_cast<Py_ssize_t>(call.raw_result.size())));
                if (!raw || PyDict_SetItemString(response.get(), "raw_result", raw.get()) != 0) {
                    set_python_error();
                    return -1;
                }
            } else {
                if (PyDict_SetItemString(response.get(), "raw_result", Py_None) != 0) {
                    set_python_error();
                    return -1;
                }
            }
            result = std::move(response);
#endif
        } else {
            set_error("unknown method");
        }

        if (!result || result.get() == Py_None) {
            if (result && result.get() == Py_None)
                set_error("MetaTrader5 operation returned None (see last_error)");
            if (PyErr_Occurred())
                set_python_error();
            return -1;
        }

        PyRef compatible(json_compatible(result.get()));
        PyRef encoded(PyObject_CallFunctionObjArgs(dumps.get(), compatible.get(), nullptr));
        if (!encoded) {
            set_python_error();
            return -1;
        }
        const char *text = PyUnicode_AsUTF8(encoded.get());
        if (!text) {
            set_python_error();
            return -1;
        }
        const std::size_t size = std::strlen(text) + 1;
        char *copy = static_cast<char *>(std::malloc(size));
        if (!copy) {
            set_error("failed to allocate response");
            return -1;
        }
        std::memcpy(copy, text, size);
        *response_json = copy;
        return 0;
    }();
    return status;
} catch (...) {
    set_current_exception_error();
    return -1;
}

MT5BRIDGE_EXPORT int mt5bridge_query_ticks(const Mt5TicksRequest *request,
                                            Mt5TickBuffer **result) try {
    if (result)
        *result = nullptr;
    clear_fetch_diagnostics();
    if (!result || !request || request->reserved != 0 ||
        request->flags > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        !valid_range(request->symbol_utf8, request->from_msc, request->to_msc))
        return -1;
    clear_error();
    auto buffer = std::make_unique<Mt5TickBuffer>();
    const auto append = [&buffer](const Mt5Tick *ticks, std::size_t count) {
        buffer->values.insert(buffer->values.end(), ticks, ticks + count);
        return 0;
    };
    if (!visit_ticks_range(request, &buffer->diagnostics, append)) {
        buffer->diagnostics.status = g_last_fetch_fatal_error
                                         ? MT5_FETCH_FATAL_ERROR
                                         : (buffer->diagnostics.history_warmup_detected
                                                ? (buffer->values.empty()
                                                       ? MT5_FETCH_RETRY_EXHAUSTED
                                                       : MT5_FETCH_PARTIAL)
                                                : MT5_FETCH_FATAL_ERROR);
        if (g_last_error.empty()) {
            if (buffer->diagnostics.status != MT5_FETCH_FATAL_ERROR)
                set_error("MetaTrader5 history is unavailable after retries");
            else
                set_error("MetaTrader5 tick query failed");
        }
        save_fetch_diagnostics(buffer->diagnostics);
        return -1;
    }
    buffer->diagnostics.status = buffer->values.empty() ? MT5_FETCH_EMPTY
                                                          : MT5_FETCH_COMPLETE;
    buffer->diagnostics.complete = 1;
    save_fetch_diagnostics(buffer->diagnostics);
    *result = buffer.release();
    return 0;
} catch (...) {
    set_current_exception_error();
    return -1;
}

MT5BRIDGE_EXPORT const Mt5Tick *mt5bridge_tick_buffer_data(const Mt5TickBuffer *buffer) {
    return buffer && !buffer->values.empty() ? buffer->values.data() : nullptr;
}

MT5BRIDGE_EXPORT size_t mt5bridge_tick_buffer_size(const Mt5TickBuffer *buffer) {
    return buffer ? buffer->values.size() : 0;
}

MT5BRIDGE_EXPORT void mt5bridge_tick_buffer_free(Mt5TickBuffer *buffer) { delete buffer; }

MT5BRIDGE_EXPORT int mt5bridge_tick_buffer_diagnostics(const Mt5TickBuffer *buffer,
                                                       Mt5FetchDiagnostics *diagnostics) {
    if (!buffer || !diagnostics)
        return -1;
    *diagnostics = buffer->diagnostics;
    return 0;
}

MT5BRIDGE_EXPORT int mt5bridge_copy_ticks_range(const Mt5TicksRequest *request,
                                                size_t chunk_size,
                                                Mt5TickChunkCallback callback,
                                                void *user_data) try {
    clear_fetch_diagnostics();
    if (!request || request->reserved != 0 ||
        request->flags > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
        !valid_range(request->symbol_utf8, request->from_msc, request->to_msc) ||
        !callback || chunk_size == 0) {
        set_error("callback and chunk_size are required");
        return -1;
    }
    clear_error();

    const auto deliver = [=](const Mt5Tick *ticks, std::size_t count) {
        for (std::size_t offset = 0; offset < count; offset += chunk_size) {
            const std::size_t length = std::min(chunk_size, count - offset);
            if (invoke_tick_callback(callback, ticks + offset, length, user_data) != 0) {
                if (g_last_error.empty())
                    set_error("tick consumer cancelled delivery");
                return -1;
            }
        }
        return 0;
    };
    Mt5FetchDiagnostics diagnostics{};
    if (visit_ticks_range(request, &diagnostics, deliver)) {
        diagnostics.status = MT5_FETCH_COMPLETE;
        diagnostics.complete = 1;
        save_fetch_diagnostics(diagnostics);
        return 0;
    }
    diagnostics.status = g_last_fetch_fatal_error
                             ? MT5_FETCH_FATAL_ERROR
                             : (diagnostics.history_warmup_detected
                                    ? MT5_FETCH_RETRY_EXHAUSTED
                                    : MT5_FETCH_FATAL_ERROR);
    save_fetch_diagnostics(diagnostics);
    if (g_last_error.empty())
        set_error("MetaTrader5 history is unavailable after retries");
    return -1;
} catch (...) {
    set_current_exception_error();
    return -1;
}

MT5BRIDGE_EXPORT int mt5bridge_query_rates(const Mt5RatesRequest *request,
                                            Mt5RateBuffer **result) try {
    if (result)
        *result = nullptr;
    clear_fetch_diagnostics();
    if (!result || !request || request->reserved != 0 ||
        !valid_range(request->symbol_utf8, request->from_msc, request->to_msc))
        return -1;
    clear_error();
    RuntimeCallAdmission call;
    if (!call)
        return -1;
    GilScope gil(true);
    const int status = [&]() {
        PyRef mt5(PyImport_ImportModule("MetaTrader5"));
        if (!mt5) {
            set_python_error();
            Mt5FetchDiagnostics diagnostics{};
            diagnostics.status = MT5_FETCH_FATAL_ERROR;
            save_fetch_diagnostics(diagnostics);
            return -1;
        }
        auto buffer = std::make_unique<Mt5RateBuffer>();
        buffer->coverage.version = MT5BRIDGE_RATE_COVERAGE_V1_VERSION;
        buffer->coverage.state = MT5_RATE_COVERAGE_UNPROVEN;
        buffer->coverage.requested_from_msc = request->from_msc;
        buffer->coverage.requested_to_msc = request->to_msc;
        buffer->coverage.observed_from_msc = -1;
        buffer->coverage.observed_to_msc = -1;
        constexpr uint32_t kRateRecoveryFailures = 3;
        constexpr uint32_t kRateConfirmationProbes = 2;
        std::vector<Mt5Rate> candidate_values;
        Mt5RateCoverageV1 candidate_coverage{};
        bool have_candidate = false;
        bool saw_values = false;
        uint32_t recovery_failures = 0;
        uint32_t confirmation_probes = 0;
        for (;;) {
            if (recovery_failures >= kRateRecoveryFailures ||
                confirmation_probes >= kRateConfirmationProbes)
                break;
            ++buffer->diagnostics.attempts;
            PyRef from(make_datetime(request->from_msc));
            PyRef to(make_datetime(request->to_msc));
            if (!from || !to) {
                set_python_error();
                save_fetch_diagnostics(buffer->diagnostics);
                return -1;
            }
            PyRef rates(PyObject_CallMethod(mt5.get(), "copy_rates_range", "siOO",
                                             request->symbol_utf8, request->timeframe,
                                             from.get(), to.get()));
            if (rates && rates.get() != Py_None) {
                std::vector<Mt5Rate> current_values;
                if (!copy_rates_array(rates.get(), &current_values)) {
                    set_error("MetaTrader5 returned an unsupported rate layout");
                    buffer->diagnostics.status = MT5_FETCH_FATAL_ERROR;
                    save_fetch_diagnostics(buffer->diagnostics);
                    return -1;
                }
                std::vector<Mt5Rate> filtered_values;
                Mt5RateCoverageV1 current_coverage = buffer->coverage;
                if (!prepare_rate_observation(current_values, request, &filtered_values,
                                              &current_coverage)) {
                    buffer->diagnostics.status = MT5_FETCH_FATAL_ERROR;
                    save_fetch_diagnostics(buffer->diagnostics);
                    return -1;
                }
                const int code = mt5_last_error_code(mt5.get());
                if (code != 1)
                    buffer->diagnostics.last_mt5_error = code;
                if (is_partial_read_error(code)) {
                    saw_values = saw_values || !current_values.empty();
                    buffer->diagnostics.history_warmup_detected = 1;
                    ++recovery_failures;
                    if (recovery_failures >= kRateRecoveryFailures)
                        break;
                    if (is_ipc_error(code) && reinitialize_terminal(mt5.get()))
                        ++buffer->diagnostics.reconnects;
                    ++buffer->diagnostics.retries;
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(50u << (recovery_failures - 1)));
                    continue;
                }
                saw_values = saw_values || !current_values.empty();
                if (!have_candidate) {
                    candidate_values = std::move(filtered_values);
                    candidate_coverage = current_coverage;
                    buffer->values = candidate_values;
                    buffer->coverage = current_coverage;
                    have_candidate = true;
                    ++buffer->diagnostics.retries;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50u));
                    continue;
                }
                const bool stable = candidate_values.size() == filtered_values.size() &&
                    same_rate_coverage(candidate_coverage, current_coverage) &&
                    (filtered_values.empty() ||
                     std::memcmp(candidate_values.data(), filtered_values.data(),
                                 filtered_values.size() * sizeof(Mt5Rate)) == 0);
                buffer->values = std::move(filtered_values);
                buffer->coverage = current_coverage;
                if (stable) {
                    const bool covered = rate_coverage_proven(request, buffer->values,
                                                               buffer->coverage);
                    buffer->coverage.state = covered ? MT5_RATE_COVERAGE_PROVEN
                                                      : MT5_RATE_COVERAGE_UNPROVEN;
                    buffer->diagnostics.status = covered ? MT5_FETCH_COMPLETE
                                                          : MT5_FETCH_PARTIAL;
                    buffer->diagnostics.complete = covered ? 1u : 0u;
                    save_fetch_diagnostics(buffer->diagnostics);
                    *result = buffer.release();
                    return 0;
                }
                candidate_values = buffer->values;
                candidate_coverage = buffer->coverage;
                ++confirmation_probes;
                if (confirmation_probes < kRateConfirmationProbes) {
                    ++buffer->diagnostics.retries;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50u));
                }
                continue;
            }
            if (PyErr_Occurred())
                PyErr_Clear();
            const int code = mt5_last_error_code(mt5.get());
            if (code != 1)
                buffer->diagnostics.last_mt5_error = code;
            if (!is_transient_read_error(code)) {
                set_error("MetaTrader5 rate request failed");
                buffer->diagnostics.status = MT5_FETCH_FATAL_ERROR;
                save_fetch_diagnostics(buffer->diagnostics);
                return -1;
            }
            buffer->diagnostics.history_warmup_detected = 1;
            ++recovery_failures;
            if (recovery_failures >= kRateRecoveryFailures)
                break;
            if (is_ipc_error(code) && reinitialize_terminal(mt5.get()))
                ++buffer->diagnostics.reconnects;
            ++buffer->diagnostics.retries;
            std::this_thread::sleep_for(
                std::chrono::milliseconds(50u << (recovery_failures - 1)));
        }
        buffer->diagnostics.status = saw_values ? MT5_FETCH_PARTIAL : MT5_FETCH_RETRY_EXHAUSTED;
        save_fetch_diagnostics(buffer->diagnostics);
        set_error("MetaTrader5 history is unavailable after retries");
        return -1;
    }();
    return status;
} catch (...) {
    set_current_exception_error();
    return -1;
}

MT5BRIDGE_EXPORT const Mt5Rate *mt5bridge_rate_buffer_data(const Mt5RateBuffer *buffer) {
    return buffer && !buffer->values.empty() ? buffer->values.data() : nullptr;
}

MT5BRIDGE_EXPORT size_t mt5bridge_rate_buffer_size(const Mt5RateBuffer *buffer) {
    return buffer ? buffer->values.size() : 0;
}

MT5BRIDGE_EXPORT void mt5bridge_rate_buffer_free(Mt5RateBuffer *buffer) { delete buffer; }

MT5BRIDGE_EXPORT int mt5bridge_rate_buffer_diagnostics(const Mt5RateBuffer *buffer,
                                                       Mt5FetchDiagnostics *diagnostics) {
    if (!buffer || !diagnostics)
        return -1;
    *diagnostics = buffer->diagnostics;
    return 0;
}

MT5BRIDGE_EXPORT int mt5bridge_rate_buffer_coverage_v1(const Mt5RateBuffer *buffer,
                                                       Mt5RateCoverageV1 *coverage) {
    if (!buffer || !coverage)
        return -1;
    *coverage = buffer->coverage;
    return 0;
}

MT5BRIDGE_API void mt5bridge_free(char *response_json) { std::free(response_json); }

MT5BRIDGE_API const char *mt5bridge_last_error() {
    return g_last_error.empty() ? nullptr : g_last_error.c_str();
}

MT5BRIDGE_EXPORT int mt5bridge_last_fetch_diagnostics(Mt5FetchDiagnostics *diagnostics) {
    if (!diagnostics)
        return -1;
    *diagnostics = g_last_fetch_diagnostics;
    return 0;
}

MT5BRIDGE_EXPORT int mt5bridge_subscribe_ticks(const Mt5SubscriptionRequest *request,
                                                Mt5SubscriptionHandle *handle) try {
    if (handle)
        *handle = Mt5SubscriptionHandle{};
    if (!request || !handle || !request->sources || request->source_count == 0 ||
        request->source_count > 1024 || request->reserved[0] != 0 || request->reserved[1] != 0) {
        set_error("valid subscription request and handle are required");
        return -1;
    }
    clear_error();
    const uint32_t interval = request->interval_ms ? request->interval_ms : 250u;
    const uint32_t max_batch = request->max_batch ? request->max_batch : 1024u;
    const uint32_t capacity = request->ring_capacity ? request->ring_capacity : 64u;
    struct PreparedSource { std::string symbol; uint32_t flags; };
    std::vector<PreparedSource> prepared;
    bool needs_default_flags = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_runtime_state != RuntimeState::running) {
            set_error(g_runtime_state == RuntimeState::shutting_down
                          ? "bridge is shutting down"
                          : "bridge not initialized");
            return -1;
        }
        if (interval < 10u || max_batch == 0 || max_batch > 1000000u ||
            max_batch > static_cast<uint32_t>(std::numeric_limits<int>::max()) ||
            capacity == 0 || capacity > 65536u ||
            static_cast<std::uint64_t>(max_batch) * capacity > kRealtimeMaxRetainedTicks ||
            (request->delivery_flags & ~(MT5_DELIVERY_TICK_BATCH | MT5_DELIVERY_COHERENT_SNAPSHOT)) != 0) {
            set_error("invalid subscription limits");
            return -1;
        }
        if (request->stale_after_ms != 0 &&
            (request->delivery_flags & MT5_DELIVERY_COHERENT_SNAPSHOT) == 0) {
            set_error("stale_after_ms requires coherent snapshot delivery");
            return -1;
        }
        if ((request->delivery_flags & MT5_DELIVERY_COHERENT_SNAPSHOT) != 0) {
            set_error("coherent snapshots are not implemented yet");
            return -1;
        }
        prepared.reserve(request->source_count);
        for (size_t index = 0; index < request->source_count; ++index) {
            const auto &source_request = request->sources[index];
            if (!source_request.symbol_utf8 || !*source_request.symbol_utf8 ||
                source_request.reserved != 0 ||
                source_request.flags > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
                set_error("invalid subscription source");
                return -1;
            }
            prepared.push_back(PreparedSource{source_request.symbol_utf8, source_request.flags});
            needs_default_flags = needs_default_flags || source_request.flags == 0;
        }
    }

    // Resolve the default COPY_TICKS_ALL flag without extending the lifecycle
    // mutex across the Python import.  A shutdown racing this phase rejects
    // the admission; the mutation phase below checks the state again.
    if (needs_default_flags) {
        RuntimeCallAdmission call;
        if (!call)
            return -1;
        GilScope gil(true);
        PyRef mt5(PyImport_ImportModule("MetaTrader5"));
        if (!mt5) {
            set_python_error();
            return -1;
        }
        for (auto &prepared_source : prepared)
            if (prepared_source.flags == 0)
                prepared_source.flags = static_cast<uint32_t>(resolve_tick_flags(mt5.get(), 0));
    }

    // All validation and flag resolution above is complete before mutating any
    // shared source or subscription state.
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_runtime_state != RuntimeState::running) {
        set_error(g_runtime_state == RuntimeState::shutting_down
                      ? "bridge is shutting down"
                      : "bridge not initialized");
        return -1;
    }
    for (const auto &prepared_source : prepared) {
        const auto it = g_realtime_sources.find(
            realtime_source_key(prepared_source.symbol.c_str(), prepared_source.flags));
        if (it != g_realtime_sources.end()) {
            const uint32_t effective_batch = std::min(it->second->max_batch, max_batch);
            const uint32_t effective_capacity = std::max(it->second->capacity, capacity);
            if (static_cast<std::uint64_t>(effective_batch) * effective_capacity >
                kRealtimeMaxRetainedTicks) {
                set_error("subscription would exceed retained tick budget");
                return -1;
            }
        }
    }
    RealtimeSubscription subscription;
    const uint64_t subscription_id = g_next_subscription_id++;
    subscription.handle = Mt5SubscriptionHandle{g_runtime_generation, subscription_id};
    subscription.members.reserve(prepared.size());
    const auto reschedule_at = std::chrono::steady_clock::now();
    for (const auto &prepared_source : prepared) {
        const std::string key = realtime_source_key(prepared_source.symbol.c_str(), prepared_source.flags);
        auto source_it = g_realtime_sources.find(key);
        std::shared_ptr<RealtimeSource> source;
        if (source_it == g_realtime_sources.end()) {
            source = std::make_shared<RealtimeSource>();
            source->symbol = prepared_source.symbol;
            source->flags = prepared_source.flags;
            source->interval_ms = interval;
            source->max_batch = max_batch;
            source->capacity = capacity;
            source->initial_from_msc = static_cast<int64_t>(std::chrono::duration_cast<
                std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            source->next_poll = std::chrono::steady_clock::now();
            g_realtime_sources.emplace(key, source);
        } else {
            source = source_it->second;
            source->interval_ms = std::min(source->interval_ms, interval);
            source->max_batch = std::min(source->max_batch, max_batch);
            source->capacity = std::max(source->capacity, capacity);
            source->next_poll = std::min(source->next_poll,
                                         reschedule_at + std::chrono::milliseconds(interval));
        }
        RealtimeMember member{source, source->next_sequence,
                              static_cast<Mt5SubscriptionStatus>(-1),
                              interval, max_batch, capacity};
        member.delivered_inconsistency_epoch = source->inconsistency_epoch;
        subscription.members.push_back(std::move(member));
    }
    g_realtime_subscriptions.emplace(subscription.handle.id, std::move(subscription));
    g_subscription_order.push_back(subscription_id);
    if (!g_poller.joinable()) {
        g_poller = std::thread(realtime_poller);
    }
    *handle = Mt5SubscriptionHandle{g_runtime_generation, subscription_id};
    g_poller_cv.notify_all();
    return 0;
} catch (...) {
    set_current_exception_error();
    return -1;
}

MT5BRIDGE_EXPORT int mt5bridge_unsubscribe(Mt5SubscriptionHandle handle) try {
    std::lock_guard<std::mutex> lock(g_mutex);
    clear_error();
    if (handle.generation != g_runtime_generation || handle.id == 0) {
        set_error("stale subscription handle");
        return -1;
    }
    const auto it = g_realtime_subscriptions.find(handle.id);
    if (it == g_realtime_subscriptions.end()) {
        set_error("unknown subscription handle");
        return -1;
    }
    g_realtime_subscriptions.erase(it);
    g_subscription_order.erase(std::remove(g_subscription_order.begin(), g_subscription_order.end(), handle.id),
                               g_subscription_order.end());
    if (!g_subscription_order.empty())
        g_rr_cursor %= g_subscription_order.size();
    else
        g_rr_cursor = 0;
    // A source may have no remaining member waiting for its latest rewrite.
    // Drop the coalesced recovery window once all surviving members ack it.
    for (auto &source_entry : g_realtime_sources) {
        auto &source = source_entry.second;
        bool pending = false;
        for (const auto &subscription_entry : g_realtime_subscriptions)
            for (const auto &member : subscription_entry.second.members)
                if (member.source == source &&
                    member.delivered_inconsistency_epoch < source->inconsistency_epoch)
                    pending = true;
        if (!pending) {
            source->recovery_from_msc = -1;
            source->recovery_to_msc = -1;
        }
    }
    for (auto &source_entry : g_realtime_sources) {
        auto &source = source_entry.second;
        uint32_t min_interval = std::numeric_limits<uint32_t>::max();
        uint32_t min_batch = std::numeric_limits<uint32_t>::max();
        uint32_t capacity = 0;
        for (const auto &subscription_entry : g_realtime_subscriptions) {
            for (const auto &member : subscription_entry.second.members) {
                if (member.source != source)
                    continue;
                min_interval = std::min(min_interval, member.interval_ms);
                min_batch = std::min(min_batch, member.max_batch);
                capacity = std::max(capacity, member.capacity);
            }
        }
        if (min_interval != std::numeric_limits<uint32_t>::max()) {
            source->interval_ms = min_interval;
            source->max_batch = min_batch == std::numeric_limits<uint32_t>::max() ? 1024u : min_batch;
            source->capacity = capacity;
        }
    }
    for (auto it_source = g_realtime_sources.begin(); it_source != g_realtime_sources.end();) {
        bool used = false;
        for (const auto &entry : g_realtime_subscriptions)
            for (const auto &member : entry.second.members)
                if (member.source == it_source->second) { used = true; break; }
        if (!used)
            it_source = g_realtime_sources.erase(it_source);
        else
            ++it_source;
    }
    return 0;
} catch (...) {
    set_current_exception_error();
    return -1;
}

MT5BRIDGE_EXPORT int mt5bridge_unsubscribe_all(void) try {
    std::lock_guard<std::mutex> lock(g_mutex);
    clear_error();
    g_realtime_subscriptions.clear();
    g_subscription_order.clear();
    g_rr_cursor = 0;
    g_realtime_sources.clear();
    return 0;
} catch (...) {
    set_current_exception_error();
    return -1;
}

MT5BRIDGE_EXPORT int mt5bridge_process_events(size_t max_events,
                                               Mt5SubscriptionEventCallback callback,
                                               void *user_data) try {
    if (!callback) {
        set_error("subscription callback is required");
        return -1;
    }
    size_t limit = max_events;
    if (limit == 0) {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto &entry : g_realtime_subscriptions)
            for (const auto &member : entry.second.members)
                limit += member.source->ring.size() + 1u;
    }
    int delivered = 0;
    while (static_cast<size_t>(delivered) < limit) {
        Mt5SubscriptionEvent event{};
        std::vector<Mt5Tick> tick_copy;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const std::size_t subscription_count = g_subscription_order.size();
            for (std::size_t offset = 0; offset < subscription_count; ++offset) {
                const std::size_t index = (g_rr_cursor + offset) % subscription_count;
                const auto sub_it = g_realtime_subscriptions.find(g_subscription_order[index]);
                if (sub_it == g_realtime_subscriptions.end())
                    continue;
                auto &subscription = sub_it->second;
                const std::size_t member_count = subscription.members.size();
                for (std::size_t member_offset = 0; member_offset < member_count; ++member_offset) {
                    const std::size_t member_index =
                        (subscription.member_cursor + member_offset) % member_count;
                    auto &member = subscription.members[member_index];
                    auto &source = member.source;
                    if (member.delivered_status != source->status) {
                        member.delivered_status = source->status;
                        event.type = MT5_SUBSCRIPTION_STATUS;
                        event.status = source->status;
                        event.handle = subscription.handle;
                        event.source_index = static_cast<uint32_t>(member_index);
                        found = true;
                        subscription.member_cursor = (member_index + 1) % member_count;
                        g_rr_cursor = (index + 1) % subscription_count;
                        break;
                    }
                    if (member.delivered_inconsistency_epoch < source->inconsistency_epoch) {
                        event.type = MT5_SUBSCRIPTION_GAP;
                        event.status = source->status;
                        event.handle = subscription.handle;
                        event.source_index = static_cast<uint32_t>(member_index);
                        event.sequence = member.next_sequence;
                        event.gap_reason = MT5_GAP_SOURCE_INCONSISTENCY;
                        event.recovery_from_msc = source->recovery_from_msc;
                        event.recovery_to_msc = source->recovery_to_msc;
                        member.delivered_inconsistency_epoch = source->inconsistency_epoch;
                        member.last_gap_reason = MT5_GAP_SOURCE_INCONSISTENCY;
                        bool all_members_ack = true;
                        for (const auto &subscription_entry : g_realtime_subscriptions)
                            for (const auto &other_member : subscription_entry.second.members)
                                if (other_member.source == source &&
                                    other_member.delivered_inconsistency_epoch < source->inconsistency_epoch)
                                    all_members_ack = false;
                        if (all_members_ack) {
                            source->recovery_from_msc = -1;
                            source->recovery_to_msc = -1;
                        }
                        found = true;
                        subscription.member_cursor = (member_index + 1) % member_count;
                        g_rr_cursor = (index + 1) % subscription_count;
                        break;
                    }
                    if (source->ring.empty())
                        continue;
                    const uint64_t oldest = source->ring.front().sequence;
                    if (member.next_sequence < oldest) {
                        event.type = MT5_SUBSCRIPTION_GAP;
                        event.status = source->status;
                        event.handle = subscription.handle;
                        event.source_index = static_cast<uint32_t>(member_index);
                        event.sequence = oldest;
                        event.dropped = oldest - member.next_sequence;
                        event.gap_reason = MT5_GAP_CONSUMER_OVERFLOW;
                        member.dropped_batches += event.dropped;
                        member.last_gap_reason = MT5_GAP_CONSUMER_OVERFLOW;
                        member.next_sequence = oldest;
                        found = true;
                        subscription.member_cursor = (member_index + 1) % member_count;
                        g_rr_cursor = (index + 1) % subscription_count;
                        break;
                    }
                    for (const auto &batch : source->ring) {
                        if (batch.sequence < member.next_sequence)
                            continue;
                        tick_copy = batch.ticks;
                        event.type = MT5_SUBSCRIPTION_TICK_BATCH;
                        event.status = source->status;
                        event.handle = subscription.handle;
                        event.source_index = static_cast<uint32_t>(member_index);
                        event.sequence = batch.sequence;
                        event.ticks = tick_copy.data();
                        event.count = tick_copy.size();
                        member.next_sequence = batch.sequence + 1;
                        found = true;
                        subscription.member_cursor = (member_index + 1) % member_count;
                        g_rr_cursor = (index + 1) % subscription_count;
                        break;
                    }
                    if (found)
                        break;
                }
                if (found)
                    break;
            }
        }
        if (!found)
            break;
        ++delivered;
        try {
            if (callback(&event, user_data) != 0)
                break;
        } catch (...) {
            set_error("subscription callback threw an exception");
            return -1;
        }
    }
    return delivered;
} catch (...) {
    set_current_exception_error();
    return -1;
}

MT5BRIDGE_EXPORT int mt5bridge_subscription_diagnostics(
    Mt5SubscriptionHandle handle, Mt5SubscriptionDiagnostics *diagnostics) try {
    if (!diagnostics) {
        set_error("diagnostics is required");
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (handle.generation != g_runtime_generation) {
        set_error("stale subscription handle");
        return -1;
    }
    const auto it = g_realtime_subscriptions.find(handle.id);
    if (it == g_realtime_subscriptions.end()) {
        set_error("unknown subscription handle");
        return -1;
    }
    if (it->second.members.empty()) {
        set_error("subscription has no sources");
        return -1;
    }
    *diagnostics = it->second.members.front().source->diagnostics;
    return 0;
} catch (...) {
    set_current_exception_error();
    return -1;
}

MT5BRIDGE_EXPORT int mt5bridge_subscription_source_diagnostics(
    Mt5SubscriptionHandle handle, uint32_t source_index,
    Mt5SubscriptionDiagnostics *diagnostics) try {
    if (!diagnostics) { set_error("diagnostics is required"); return -1; }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (handle.generation != g_runtime_generation) { set_error("stale subscription handle"); return -1; }
    const auto it = g_realtime_subscriptions.find(handle.id);
    if (it == g_realtime_subscriptions.end()) { set_error("unknown subscription handle"); return -1; }
    if (source_index >= it->second.members.size()) { set_error("source index out of range"); return -1; }
    *diagnostics = it->second.members[source_index].source->diagnostics;
    return 0;
} catch (...) {
    set_current_exception_error();
    return -1;
}

} // extern "C"
