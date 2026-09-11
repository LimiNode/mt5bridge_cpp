/// \file mt5_bridge.cpp
/// \brief Implements the CPython-backed mt5_bridge.dll runtime.

#include "mt5bridge/mt5bridge.hpp"
#include "mt5bridge/data.h"

#include <Python.h>

#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
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
};

namespace {

std::mutex g_mutex;
bool g_initialized = false;
bool g_owns_interpreter = false;
PyThreadState *g_main_thread_state = nullptr;
std::thread::id g_owner_thread;
thread_local std::string g_last_error;
thread_local Mt5FetchDiagnostics g_last_fetch_diagnostics{};

struct RealtimeBatch {
    uint64_t sequence = 0;
    std::vector<Mt5Tick> ticks;
};
struct RealtimeSource {
    std::string symbol;
    uint32_t flags = 0;
    uint32_t interval_ms = 250;
    uint32_t max_batch = 1024;
    uint32_t capacity = 64;
    std::chrono::steady_clock::time_point next_poll{};
    uint64_t next_sequence = 1;
    int64_t cursor_time_msc = -1;
    std::size_t cursor_ordinal = 0;
    Mt5SubscriptionStatus status = MT5_SUBSCRIPTION_STARTING;
    std::deque<RealtimeBatch> ring;
};
struct RealtimeSubscription {
    Mt5SubscriptionHandle handle{};
    std::shared_ptr<RealtimeSource> source;
    uint64_t next_sequence = 1;
    Mt5SubscriptionStatus delivered_status = static_cast<Mt5SubscriptionStatus>(-1);
};
std::unordered_map<std::string, std::shared_ptr<RealtimeSource>> g_realtime_sources;
std::unordered_map<uint64_t, RealtimeSubscription> g_realtime_subscriptions;
uint64_t g_next_subscription_id = 1;
uint64_t g_runtime_generation = 0;
std::thread g_poller;
std::condition_variable g_poller_cv;
bool g_poller_stop = false;


/// \brief Clears the calling thread's latest market-data diagnostics.
void clear_fetch_diagnostics() { g_last_fetch_diagnostics = Mt5FetchDiagnostics{}; }

/// \brief Saves market-data diagnostics for retrieval after a failed query.
void save_fetch_diagnostics(const Mt5FetchDiagnostics &diagnostics) {
    g_last_fetch_diagnostics = diagnostics;
}

/// \brief Replaces the calling thread's bridge diagnostic.
/// \param message Null-terminated message, or nullptr for a generic fallback.
void set_error(const char *message) { g_last_error = message ? message : "unknown error"; }

/// \brief Clears the calling thread's bridge diagnostic.
void clear_error() { g_last_error.clear(); }

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
    return code == 0 || code == -4 || (code <= -10000 && code >= -10005);
}

/// \brief Tests whether returned data must be treated as a recoverable partial page.
/// \param code MetaTrader Python API status code.
/// \return True when the result carries a recognized timeout or IPC error.
bool is_partial_read_error(int code) {
    return code == -4 || (code <= -10000 && code >= -10005);
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
                     bool *partial) {
    if (partial)
        *partial = false;
    for (uint32_t attempt = 1; attempt <= 3; ++attempt) {
        if (diagnostics)
            ++diagnostics->attempts;
        PyRef from(make_datetime(from_msc));
        if (!from) {
            set_python_error();
            return false;
        }
        PyRef ticks(PyObject_CallMethod(mt5, "copy_ticks_from", "sOii", symbol, from.get(),
                                        count, flags));
        if (ticks && ticks.get() != Py_None) {
            if (!copy_ticks_array(ticks.get(), page)) {
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
                if (attempt < 3) {
                    if (diagnostics)
                        ++diagnostics->retries;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50u << (attempt - 1)));
                    continue;
                }
            }
            return true;
        }
        if (PyErr_Occurred())
            PyErr_Clear();
        const int code = mt5_last_error_code(mt5);
        if (diagnostics && code != 1)
            diagnostics->last_mt5_error = code;
        if (!is_transient_read_error(code)) {
            set_error("MetaTrader5 tick request failed");
            return false;
        }
        if (diagnostics)
            diagnostics->history_warmup_detected = 1;
        if (attempt < 3) {
            if (is_ipc_error(code) && reinitialize_terminal(mt5) && diagnostics)
                ++diagnostics->reconnects;
            if (diagnostics)
                ++diagnostics->retries;
            std::this_thread::sleep_for(std::chrono::milliseconds(50u << (attempt - 1)));
        }
    }
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
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_initialized) {
            set_error("bridge not initialized");
            return false;
        }
        GilScope gil(true);
        PyRef mt5(PyImport_ImportModule("MetaTrader5"));
        if (!mt5) {
            set_python_error();
            return false;
        }
        flags = resolve_tick_flags(mt5.get(), request->flags);
    }

    int64_t cursor_msc = request->from_msc;
    std::size_t skipped_at_cursor = 0;
    uint32_t short_page_confirmations = 0;
    uint32_t partial_page_count = 0;
    bool confirming_short_page = false;
    for (;;) {
        std::vector<Mt5Tick> page;
        bool page_ok = false;
        bool partial_page = false;
        int page_size = kTickPageSize;
        if (skipped_at_cursor >
            static_cast<std::size_t>(std::numeric_limits<int>::max() - kTickPageSize)) {
            set_error("too many ticks share one timestamp");
            return false;
        }
        page_size += static_cast<int>(skipped_at_cursor);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_initialized) {
                set_error("bridge not initialized");
                return false;
            }
            GilScope gil(true);
            PyRef mt5(PyImport_ImportModule("MetaTrader5"));
            if (!mt5) {
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
            confirming_short_page = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50u));
            continue;
        }

        const std::size_t previously_consumed_at_cursor = skipped_at_cursor;
        std::vector<Mt5Tick> deliver;
        deliver.reserve(page.size());
        const int64_t last_timestamp = page.back().time_msc;
        std::size_t last_timestamp_count = 0;
        for (auto it = page.rbegin(); it != page.rend() && it->time_msc == last_timestamp; ++it)
            ++last_timestamp_count;
        for (const Mt5Tick &tick : page) {
            if (tick.time_msc < request->from_msc || tick.time_msc > request->to_msc)
                continue;
            if (tick.time_msc < cursor_msc)
                continue;
            if (tick.time_msc == cursor_msc && skipped_at_cursor != 0) {
                --skipped_at_cursor;
                continue;
            }
            deliver.push_back(tick);
        }
        if (!deliver.empty() && consume(deliver.data(), deliver.size()) != 0)
            return false;

        // copy_ticks_from is inclusive.  Keep the timestamp and remember how
        // many records at that timestamp have already been consumed; adding
        // one millisecond would lose tied ticks.
        if (last_timestamp < cursor_msc ||
            (last_timestamp == cursor_msc &&
             last_timestamp_count <= previously_consumed_at_cursor)) {
            if (confirming_short_page && !partial_page) {
                if (short_page_confirmations >= kShortPageProbes)
                    break;
                ++short_page_confirmations;
                std::this_thread::sleep_for(std::chrono::milliseconds(50u));
                continue;
            }
            set_error("MetaTrader returned a non-progressing tick page");
            return false;
        }
        cursor_msc = last_timestamp;
        skipped_at_cursor = last_timestamp_count;
        if (partial_page) {
            ++partial_page_count;
            confirming_short_page = false;
            if (partial_page_count > kPartialPageLimit) {
                set_error("MetaTrader history remained partial after recovery");
                return false;
            }
            continue;
        }
        partial_page_count = 0;
        const bool short_page = page.size() < static_cast<std::size_t>(page_size);
        if (short_page) {
            if (last_timestamp > request->to_msc || short_page_confirmations >= kShortPageProbes)
                break;
            ++short_page_confirmations;
            confirming_short_page = true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50u));
        } else {
            short_page_confirmations = 0;
            confirming_short_page = false;
        }
    }
    return true;
}

/// \brief Polls all active physical realtime sources while the runtime is alive.
std::string realtime_source_key(const char *symbol, uint32_t flags) {
    return std::string(symbol) + "\n" + std::to_string(flags);
}

void realtime_poller() try {
    for (;;) {
        std::unique_lock<std::mutex> lock(g_mutex);
        g_poller_cv.wait_for(lock, std::chrono::milliseconds(20), [] {
            return g_poller_stop || !g_initialized;
        });
        if (g_poller_stop || !g_initialized)
            return;
        std::vector<std::shared_ptr<RealtimeSource>> sources;
        for (const auto &entry : g_realtime_sources)
            sources.push_back(entry.second);
        if (sources.empty())
            continue;
        GilScope gil(true);
        PyRef mt5(PyImport_ImportModule("MetaTrader5"));
        if (!mt5) {
            PyErr_Clear();
            for (auto &source : sources)
                source->status = MT5_SUBSCRIPTION_FAILED;
            continue;
        }
        const auto now = std::chrono::steady_clock::now();
        for (auto &source : sources) {
            if (now < source->next_poll)
                continue;
            source->next_poll = now + std::chrono::milliseconds(source->interval_ms);
            const int64_t now_msc = static_cast<int64_t>(std::chrono::duration_cast<
                std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
            const int64_t from_msc = source->cursor_time_msc >= 0
                ? source->cursor_time_msc
                : now_msc - static_cast<int64_t>(source->interval_ms) * 4;
            PyRef from(make_datetime(std::max<int64_t>(0, from_msc)));
            if (!from)
                continue;
            PyRef ticks(PyObject_CallMethod(mt5.get(), "copy_ticks_from", "sOii",
                                            source->symbol.c_str(), from.get(),
                                            static_cast<int>(source->max_batch),
                                            static_cast<int>(source->flags)));
            if (!ticks || ticks.get() == Py_None) {
                PyErr_Clear();
                source->status = MT5_SUBSCRIPTION_RECONNECTING;
                continue;
            }
            std::vector<Mt5Tick> values;
            if (!copy_ticks_array(ticks.get(), &values)) {
                PyErr_Clear();
                source->status = MT5_SUBSCRIPTION_FAILED;
                continue;
            }
            source->status = MT5_SUBSCRIPTION_READY;
            std::vector<Mt5Tick> fresh;
            fresh.reserve(values.size());
            std::size_t skipped_at_cursor = 0;
            for (const auto &tick : values) {
                if (source->cursor_time_msc >= 0 && tick.time_msc < source->cursor_time_msc)
                    continue;
                if (source->cursor_time_msc >= 0 && tick.time_msc == source->cursor_time_msc &&
                    skipped_at_cursor++ < source->cursor_ordinal)
                    continue;
                fresh.push_back(tick);
            }
            if (fresh.empty())
                continue;
            source->cursor_time_msc = fresh.back().time_msc;
            source->cursor_ordinal = 0;
            for (auto it = fresh.rbegin(); it != fresh.rend() && it->time_msc == source->cursor_time_msc; ++it)
                ++source->cursor_ordinal;
            RealtimeBatch batch;
            batch.sequence = source->next_sequence++;
            batch.ticks = std::move(fresh);
            source->ring.push_back(std::move(batch));
            while (source->ring.size() > source->capacity)
                source->ring.pop_front();
        }
    }
} catch (...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (auto &entry : g_realtime_sources)
        entry.second->status = MT5_SUBSCRIPTION_FAILED;
}

} // namespace

extern "C" {

MT5BRIDGE_EXPORT uint32_t mt5bridge_abi_version() { return MT5BRIDGE_ABI_VERSION; }

MT5BRIDGE_API int mt5bridge_initialize(const wchar_t *python_home) try {
    std::lock_guard<std::mutex> lock(g_mutex);
    clear_error();
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
    if (!g_initialized)
        return 0;

    const bool wrong_owner_thread =
        g_owns_interpreter && std::this_thread::get_id() != g_owner_thread;
    if (wrong_owner_thread) {
        set_error("runtime shutdown requires initialize owner thread");
        return -1;
    }

    g_poller_stop = true;
    g_poller_cv.notify_all();
    poller = std::move(g_poller);
    }
    if (poller.joinable())
        poller.join();
    std::unique_lock<std::mutex> lock(g_mutex);

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
    g_realtime_subscriptions.clear();
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

    std::lock_guard<std::mutex> lock(g_mutex);
    clear_error();
    if (!g_initialized) {
        set_error("bridge not initialized");
        return -1;
    }

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
            const char *symbol = nullptr;
            double volume = 0.0;
            if (read_string(request.get(), "symbol", &symbol) &&
                read_double(request.get(), "volume", &volume)) {
                PyRef order(Py_BuildValue("{s:s,s:d,s:i}", "symbol", symbol, "volume", volume,
                                          "type", 0));
                result = PyRef(PyObject_CallMethod(mt5.get(), "order_send", "O", order.get()));
            }
        } else {
            set_error("unknown method");
        }

        if (!result) {
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
    if (!result || !request || !valid_range(request->symbol_utf8, request->from_msc,
                                            request->to_msc))
        return -1;
    clear_error();
    auto buffer = std::make_unique<Mt5TickBuffer>();
    const auto append = [&buffer](const Mt5Tick *ticks, std::size_t count) {
        buffer->values.insert(buffer->values.end(), ticks, ticks + count);
        return 0;
    };
    if (!visit_ticks_range(request, &buffer->diagnostics, append)) {
        buffer->diagnostics.status = buffer->diagnostics.history_warmup_detected
                                         ? (buffer->values.empty() ? MT5_FETCH_RETRY_EXHAUSTED
                                                                    : MT5_FETCH_PARTIAL)
                                         : MT5_FETCH_FATAL_ERROR;
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
    if (!request || !valid_range(request->symbol_utf8, request->from_msc, request->to_msc) ||
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
    diagnostics.status = diagnostics.history_warmup_detected ? MT5_FETCH_RETRY_EXHAUSTED
                                                               : MT5_FETCH_FATAL_ERROR;
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
    if (!result || !request || !valid_range(request->symbol_utf8, request->from_msc,
                                            request->to_msc))
        return -1;
    std::lock_guard<std::mutex> lock(g_mutex);
    clear_error();
    if (!g_initialized) {
        set_error("bridge not initialized");
        return -1;
    }
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
        constexpr uint32_t kRateRecoveryFailures = 3;
        constexpr uint32_t kRateConfirmationProbes = 2;
        std::vector<Mt5Rate> candidate_values;
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
                    candidate_values = std::move(current_values);
                    buffer->values = candidate_values;
                    have_candidate = true;
                    ++buffer->diagnostics.retries;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50u));
                    continue;
                }
                const bool stable = candidate_values.size() == current_values.size() &&
                    (current_values.empty() ||
                     std::memcmp(candidate_values.data(), current_values.data(),
                                 current_values.size() * sizeof(Mt5Rate)) == 0);
                buffer->values = std::move(current_values);
                if (stable) {
                    buffer->diagnostics.status = buffer->values.empty() ? MT5_FETCH_EMPTY
                                                                          : MT5_FETCH_COMPLETE;
                    buffer->diagnostics.complete = 1;
                    save_fetch_diagnostics(buffer->diagnostics);
                    *result = buffer.release();
                    return 0;
                }
                candidate_values = buffer->values;
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
    if (!request || !handle || !request->symbol_utf8 || !*request->symbol_utf8 ||
        request->reserved != 0) {
        set_error("valid subscription request and handle are required");
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    clear_error();
    if (!g_initialized) {
        set_error("bridge not initialized");
        return -1;
    }
    const uint32_t interval = request->interval_ms ? request->interval_ms : 250u;
    const uint32_t max_batch = request->max_batch ? request->max_batch : 1024u;
    const uint32_t capacity = request->queue_capacity ? request->queue_capacity : 64u;
    if (interval < 10u || max_batch == 0 || capacity == 0) {
        set_error("invalid subscription limits");
        return -1;
    }
    uint32_t effective_flags = request->flags;
    if (effective_flags == 0) {
        GilScope gil(true);
        PyRef mt5(PyImport_ImportModule("MetaTrader5"));
        if (mt5)
            effective_flags = static_cast<uint32_t>(resolve_tick_flags(mt5.get(), 0));
        else
            PyErr_Clear();
    }
    const std::string key = realtime_source_key(request->symbol_utf8, effective_flags);
    auto source_it = g_realtime_sources.find(key);
    std::shared_ptr<RealtimeSource> source;
    if (source_it == g_realtime_sources.end()) {
        source = std::make_shared<RealtimeSource>();
        source->symbol = request->symbol_utf8;
        source->flags = effective_flags;
        source->interval_ms = interval;
        source->max_batch = max_batch;
        source->capacity = capacity;
        source->next_poll = std::chrono::steady_clock::now();
        g_realtime_sources.emplace(key, source);
    } else {
        source = source_it->second;
        source->interval_ms = std::min(source->interval_ms, interval);
        source->max_batch = std::max(source->max_batch, max_batch);
        source->capacity = std::max(source->capacity, capacity);
    }
    RealtimeSubscription subscription;
    subscription.handle = Mt5SubscriptionHandle{g_runtime_generation, g_next_subscription_id++};
    subscription.source = std::move(source);
    subscription.next_sequence = subscription.source->next_sequence;
    g_realtime_subscriptions.emplace(subscription.handle.id, std::move(subscription));
    if (!g_poller.joinable()) {
        g_poller_stop = false;
        g_poller = std::thread(realtime_poller);
    }
    *handle = Mt5SubscriptionHandle{g_runtime_generation, g_next_subscription_id - 1};
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
    for (auto it_source = g_realtime_sources.begin(); it_source != g_realtime_sources.end();) {
        bool used = false;
        for (const auto &entry : g_realtime_subscriptions)
            if (entry.second.source == it_source->second) { used = true; break; }
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
            limit += entry.second.source->ring.size() + 1u;
    }
    int delivered = 0;
    while (static_cast<size_t>(delivered) < limit) {
        Mt5SubscriptionEvent event{};
        std::vector<Mt5Tick> tick_copy;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (auto &entry : g_realtime_subscriptions) {
                auto &subscription = entry.second;
                auto &source = subscription.source;
                if (subscription.delivered_status != source->status) {
                    subscription.delivered_status = source->status;
                    event.type = MT5_SUBSCRIPTION_STATUS;
                    event.status = source->status;
                    event.handle = subscription.handle;
                    found = true;
                    break;
                }
                if (source->ring.empty())
                    continue;
                const uint64_t oldest = source->ring.front().sequence;
                if (subscription.next_sequence < oldest) {
                    event.type = MT5_SUBSCRIPTION_GAP;
                    event.status = source->status;
                    event.handle = subscription.handle;
                    event.sequence = oldest;
                    event.dropped = oldest - subscription.next_sequence;
                    subscription.next_sequence = oldest;
                    found = true;
                    break;
                }
                for (const auto &batch : source->ring) {
                    if (batch.sequence < subscription.next_sequence)
                        continue;
                    tick_copy = batch.ticks;
                    event.type = MT5_SUBSCRIPTION_TICK_BATCH;
                    event.status = source->status;
                    event.handle = subscription.handle;
                    event.sequence = batch.sequence;
                    event.ticks = tick_copy.data();
                    event.count = tick_copy.size();
                    subscription.next_sequence = batch.sequence + 1;
                    found = true;
                    break;
                }
                if (found)
                    break;
            }
        }
        if (!found)
            break;
        try {
            if (callback(&event, user_data) != 0)
                break;
        } catch (...) {
            set_error("subscription callback threw an exception");
            return -1;
        }
        ++delivered;
    }
    return delivered;
} catch (...) {
    set_current_exception_error();
    return -1;
}

} // extern "C"
