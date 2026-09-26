#pragma once

/// \file client.hpp
/// \brief Defines the lightweight C++ facade for dynamically loaded mt5_bridge.dll.

#include <mt5bridge/market.h>
#include <mt5bridge/trade.h>

#if !defined(_WIN32)
#  error "mt5bridge::Client is only supported on Windows"
#endif

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

/// \namespace mt5bridge
/// \brief Contains the lightweight C++ consumer API.
namespace mt5bridge {

/// \struct RateQueryResult
/// \brief Owns a best-effort rate result together with completeness evidence.
struct RateQueryResult {
    std::vector<Mt5Rate> values;       ///< Rows copied from the DLL-owned buffer.
    Mt5FetchDiagnostics diagnostics{}; ///< Recovery and semantic status.
    Mt5RateCoverageV1 coverage{};      ///< Immutable V1 range-coverage evidence.

    /// \brief Reports whether the requested range is safe to consume as complete.
    /// \return True only when both diagnostics and coverage prove completion.
    bool complete() const noexcept {
        return diagnostics.status == MT5_FETCH_COMPLETE && diagnostics.complete != 0 &&
               coverage.version == MT5BRIDGE_RATE_COVERAGE_V1_VERSION &&
               coverage.state == MT5_RATE_COVERAGE_PROVEN;
    }
};

/// \class Client
/// \brief Owns a dynamically loaded bridge runtime and exposes RAII C++ operations.
///
/// The client depends only on C++17 and WinAPI. Python, NumPy, MetaTrader5, and
/// the DLL import library remain runtime implementation details.
/// Only one initialized Client may own the process-global runtime at a time.
class Client {
public:
    struct SubscriptionState {
        std::atomic<bool> alive{true};
        int (*unsubscribe)(Mt5SubscriptionHandle) = nullptr;
        std::thread::id owner_thread{};
    };
    /// \class Subscription
    /// \brief Move-only RAII owner of a realtime tick subscription.
    class Subscription {
    public:
        Subscription() = default;
        ~Subscription() { reset(); }
        Subscription(const Subscription &) = delete;
        Subscription &operator=(const Subscription &) = delete;
        Subscription(Subscription &&other) noexcept : state_(std::move(other.state_)), handle_(other.handle_) {
            other.handle_ = {};
        }
        Subscription &operator=(Subscription &&other) noexcept {
            if (this != &other) { reset(); state_ = std::move(other.state_); handle_ = other.handle_; other.handle_ = {}; }
            return *this;
        }
        /// \brief Returns the underlying generation-qualified handle.
        Mt5SubscriptionHandle handle() const noexcept { return handle_; }
        /// \brief Cancels the subscription when still attached to its client.
        /// \warning Reset must run on the Client owner thread; cross-thread
        /// reset defers cancellation until Client::unload().
        void reset() noexcept {
            if (auto state = state_.lock()) {
                // The DLL function pointer is valid only while Client is loaded.
                // Cross-thread reset leaves cancellation to Client::unload().
                if (state->owner_thread == std::this_thread::get_id()) {
                    try { if (state->alive && state->unsubscribe) state->unsubscribe(handle_); } catch (...) {}
                }
                state_.reset();
            }
            handle_ = {};
        }
        explicit operator bool() const noexcept {
            const auto state = state_.lock();
            return state && state->alive;
        }
    private:
        friend class Client;
        Subscription(std::shared_ptr<SubscriptionState> state, Mt5SubscriptionHandle handle)
            : state_(std::move(state)), handle_(handle) {}
        std::weak_ptr<SubscriptionState> state_;
        Mt5SubscriptionHandle handle_{};
    };
    /// \brief Constructs an unloaded client.
    Client() = default;

    /// \brief Constructs a client and loads the requested runtime DLL.
    /// \param path Path to mt5_bridge.dll.
    /// \throws std::runtime_error If the DLL is missing or ABI-incompatible.
    explicit Client(const wchar_t *path) { load(path); }

    /// \brief Shuts down an initialized runtime and unloads the DLL.
    ~Client() { unload(); }

    /// \brief Client instances cannot share ownership of one DLL handle.
    Client(const Client &) = delete;

    /// \brief Client instances cannot share ownership of one DLL handle.
    Client &operator=(const Client &) = delete;

    /// \brief Transfers the loaded runtime and its initialization state.
    /// \param other Client whose state is transferred.
    Client(Client &&other) noexcept { move_from(other); }

    /// \brief Replaces this client with the state owned by another client.
    /// \param other Client whose state is transferred.
    /// \return Reference to this client.
    Client &operator=(Client &&other) noexcept {
        if (this != &other) {
            // A failed shutdown deliberately keeps the runtime alive.  Do not
            // overwrite this object's ownership in that case.
            if (initialized_)
                return *this;
            unload();
            move_from(other);
        }
        return *this;
    }

    /// \brief Loads a runtime DLL, resolves its API, and validates its ABI version.
    /// \param path DLL path; defaults to mt5_bridge.dll resolved from the working directory.
    /// \throws std::runtime_error If loading, symbol resolution, or ABI validation fails.
    void load(const wchar_t *path = L"mt5_bridge.dll") {
        if (module_)
            return;
        module_ = load_module(path);
        if (!module_)
            throw std::runtime_error("failed to load mt5_bridge.dll");
        abi_version_ = resolve<AbiVersion>("mt5bridge_abi_version");
        trade_api_version_ = resolve<TradeApiVersion>("mt5bridge_trade_api_version");
        initialize_ = resolve<Initialize>("mt5bridge_initialize");
        shutdown_ = resolve<Shutdown>("mt5bridge_shutdown");
        eval_json_ = resolve<EvalJson>("mt5bridge_eval_json");
        free_ = resolve<Free>("mt5bridge_free");
        last_error_ = resolve<LastError>("mt5bridge_last_error");
        query_ticks_ = resolve<QueryTicks>("mt5bridge_query_ticks");
        tick_data_ = resolve<TickData>("mt5bridge_tick_buffer_data");
        tick_size_ = resolve<TickSize>("mt5bridge_tick_buffer_size");
        tick_free_ = resolve<TickFree>("mt5bridge_tick_buffer_free");
        tick_diagnostics_ = resolve<TickDiagnostics>("mt5bridge_tick_buffer_diagnostics");
        copy_ticks_chunks_ = resolve<CopyTicksChunks>("mt5bridge_copy_ticks_range");
        query_rates_ = resolve<QueryRates>("mt5bridge_query_rates");
        rate_data_ = resolve<RateData>("mt5bridge_rate_buffer_data");
        rate_size_ = resolve<RateSize>("mt5bridge_rate_buffer_size");
        rate_free_ = resolve<RateFree>("mt5bridge_rate_buffer_free");
        rate_diagnostics_ = resolve<RateDiagnostics>("mt5bridge_rate_buffer_diagnostics");
        rate_coverage_ = resolve<RateCoverage>("mt5bridge_rate_buffer_coverage_v1");
        last_fetch_diagnostics_ =
            resolve<LastFetchDiagnostics>("mt5bridge_last_fetch_diagnostics");
        subscribe_ticks_ = resolve<SubscribeTicks>("mt5bridge_subscribe_ticks");
        unsubscribe_ = resolve<Unsubscribe>("mt5bridge_unsubscribe");
        unsubscribe_all_ = resolve<UnsubscribeAll>("mt5bridge_unsubscribe_all");
        process_events_ = resolve<ProcessEvents>("mt5bridge_process_events");
        subscription_diagnostics_ = resolve<SubscriptionDiagnostics>("mt5bridge_subscription_diagnostics");
        subscription_source_diagnostics_ = resolve<SubscriptionSourceDiagnostics>("mt5bridge_subscription_source_diagnostics");
        account_info_ = resolve<AccountInfo>("mt5bridge_account_info");
        symbol_capabilities_ = resolve<SymbolCapabilities>("mt5bridge_symbol_capabilities");
        order_check_ = resolve<OrderCheck>("mt5bridge_order_check");
        query_orders_ = resolve<QueryOrders>("mt5bridge_query_orders");
        order_buffer_data_ = resolve<OrderBufferData>("mt5bridge_order_buffer_data");
        order_buffer_size_ = resolve<OrderBufferSize>("mt5bridge_order_buffer_size");
        order_buffer_free_ = resolve<OrderBufferFree>("mt5bridge_order_buffer_free");
        query_positions_ = resolve<QueryPositions>("mt5bridge_query_positions");
        position_buffer_data_ = resolve<PositionBufferData>("mt5bridge_position_buffer_data");
        position_buffer_size_ = resolve<PositionBufferSize>("mt5bridge_position_buffer_size");
        position_buffer_free_ = resolve<PositionBufferFree>("mt5bridge_position_buffer_free");
        query_history_orders_ = resolve<QueryHistoryOrders>("mt5bridge_query_history_orders");
        history_order_buffer_data_ =
            resolve<HistoryOrderBufferData>("mt5bridge_history_order_buffer_data");
        history_order_buffer_size_ =
            resolve<HistoryOrderBufferSize>("mt5bridge_history_order_buffer_size");
        history_order_buffer_free_ =
            resolve<HistoryOrderBufferFree>("mt5bridge_history_order_buffer_free");
        query_history_deals_ = resolve<QueryHistoryDeals>("mt5bridge_query_history_deals");
        deal_buffer_data_ = resolve<DealBufferData>("mt5bridge_deal_buffer_data");
        deal_buffer_size_ = resolve<DealBufferSize>("mt5bridge_deal_buffer_size");
        deal_buffer_free_ = resolve<DealBufferFree>("mt5bridge_deal_buffer_free");
        if (!abi_version_ || !trade_api_version_ || !initialize_ || !shutdown_ || !eval_json_ || !free_ ||
            !last_error_ || !query_ticks_ || !tick_data_ || !tick_size_ || !tick_free_ ||
            !tick_diagnostics_ || !query_rates_ || !rate_data_ || !rate_size_ ||
            !rate_free_ || !rate_diagnostics_ || !rate_coverage_ || !last_fetch_diagnostics_ ||
            !copy_ticks_chunks_ || !subscribe_ticks_ || !unsubscribe_ || !unsubscribe_all_ ||
            !process_events_ || !subscription_diagnostics_ || !subscription_source_diagnostics_ ||
            !account_info_ || !symbol_capabilities_ || !order_check_ ||
            !query_orders_ || !order_buffer_data_ || !order_buffer_size_ || !order_buffer_free_ ||
            !query_positions_ || !position_buffer_data_ || !position_buffer_size_ ||
            !position_buffer_free_ || !query_history_orders_ || !history_order_buffer_data_ ||
            !history_order_buffer_size_ || !history_order_buffer_free_ || !query_history_deals_ ||
            !deal_buffer_data_ || !deal_buffer_size_ || !deal_buffer_free_ ||
            abi_version_() != MT5BRIDGE_ABI_VERSION ||
            trade_api_version_() != MT5BRIDGE_TRADE_API_VERSION) {
            unload();
            throw std::runtime_error("incompatible mt5_bridge.dll ABI");
        }
        subscription_state_ = std::make_shared<SubscriptionState>();
        subscription_state_->unsubscribe = unsubscribe_;
        subscription_state_->owner_thread = std::this_thread::get_id();
    }

    /// \brief Shuts down the runtime when needed and releases the DLL handle.
    /// \warning Destroying or unloading an initialized client from another thread terminates the process.
    void unload() noexcept {
        if (initialized_ && std::this_thread::get_id() != owner_thread_)
            std::terminate();
        if (initialized_ && shutdown_ && shutdown_() != 0) {
            // Keep ownership claimed: the runtime may still be alive after a
            // failed finalization and must not be taken over by another client.
            return;
        }
        Client *expected = this;
        active_client_.compare_exchange_strong(expected, nullptr);
        if (initialized_)
            runtime_claimed_ = false;
        if (subscription_state_)
            subscription_state_->alive = false;
        subscription_state_.reset();
        if (module_)
            FreeLibrary(module_);
        module_ = nullptr;
        abi_version_ = nullptr;
        trade_api_version_ = nullptr;
        initialize_ = nullptr;
        shutdown_ = nullptr;
        eval_json_ = nullptr;
        free_ = nullptr;
        last_error_ = nullptr;
        query_ticks_ = nullptr;
        tick_data_ = nullptr;
        tick_size_ = nullptr;
        tick_free_ = nullptr;
        tick_diagnostics_ = nullptr;
        copy_ticks_chunks_ = nullptr;
        query_rates_ = nullptr;
        rate_data_ = nullptr;
        rate_size_ = nullptr;
        rate_free_ = nullptr;
        rate_diagnostics_ = nullptr;
        rate_coverage_ = nullptr;
        last_fetch_diagnostics_ = nullptr;
        subscribe_ticks_ = nullptr;
        unsubscribe_ = nullptr;
        unsubscribe_all_ = nullptr;
        process_events_ = nullptr;
        subscription_diagnostics_ = nullptr;
        subscription_source_diagnostics_ = nullptr;
        account_info_ = nullptr;
        symbol_capabilities_ = nullptr;
        order_check_ = nullptr;
        query_orders_ = nullptr;
        order_buffer_data_ = nullptr;
        order_buffer_size_ = nullptr;
        order_buffer_free_ = nullptr;
        query_positions_ = nullptr;
        position_buffer_data_ = nullptr;
        position_buffer_size_ = nullptr;
        position_buffer_free_ = nullptr;
        query_history_orders_ = nullptr;
        history_order_buffer_data_ = nullptr;
        history_order_buffer_size_ = nullptr;
        history_order_buffer_free_ = nullptr;
        query_history_deals_ = nullptr;
        deal_buffer_data_ = nullptr;
        deal_buffer_size_ = nullptr;
        deal_buffer_free_ = nullptr;
        initialized_ = false;
    }

    /// \brief Tests whether a compatible runtime DLL is loaded.
    /// \return True when this client owns a DLL handle.
    bool loaded() const noexcept { return module_ != nullptr; }

    /// \brief Initializes the embedded Python and MetaTrader 5 runtime.
    /// \param python_home Optional Python home directory, or nullptr for the default.
    /// \throws std::runtime_error If no DLL is loaded or initialization fails.
    void initialize(const wchar_t *python_home = nullptr) {
        check_loaded();
        std::lock_guard<std::mutex> lock(ownership_mutex_);
        if (initialized_) {
            if (std::this_thread::get_id() != owner_thread_)
                throw std::runtime_error("mt5bridge initialize requires the existing owner thread");
            return;
        }
        if (active_client_ && active_client_ != this)
            throw std::runtime_error("another mt5bridge::Client already owns the runtime");
        if (runtime_claimed_ && active_client_ != this)
            throw std::runtime_error("another mt5bridge::Client already owns the runtime");
        if (initialize_(python_home) != 0)
            throw std::runtime_error(error_message());
        initialized_ = true;
        owner_thread_ = std::this_thread::get_id();
        if (!subscription_state_ || !subscription_state_->alive)
            subscription_state_ = std::make_shared<SubscriptionState>();
        subscription_state_->unsubscribe = unsubscribe_;
        subscription_state_->owner_thread = owner_thread_;
        active_client_ = this;
        runtime_claimed_ = true;
    }

    /// \brief Shuts down an initialized runtime while keeping the DLL loaded.
    /// \throws std::runtime_error If called from a different thread than initialize().
    void shutdown() {
        if (!initialized_)
            return;
        if (std::this_thread::get_id() != owner_thread_)
            throw std::runtime_error("mt5bridge shutdown requires initialize owner thread");
        if (shutdown_ && shutdown_() != 0)
            throw std::runtime_error(error_message());
        initialized_ = false;
        if (subscription_state_)
            subscription_state_->alive = false;
        Client *expected = this;
        active_client_.compare_exchange_strong(expected, nullptr);
        runtime_claimed_ = false;
    }

    /// \brief Executes a control-plane JSON request.
    /// \param request_json UTF-8 JSON object describing the operation.
    /// \return UTF-8 JSON response copied into application-owned storage.
    /// \throws std::runtime_error If the request fails or returns no response.
    std::string eval(const std::string &request_json) {
        check_loaded();
        char *response = nullptr;
        const int status = eval_json_(request_json.c_str(), &response);
        struct ResponseGuard {
            void (*release)(char *);
            void operator()(char *value) const noexcept {
                if (value && release)
                    release(value);
            }
        };
        std::unique_ptr<char, ResponseGuard> owned(response, ResponseGuard{free_});
        if (status != 0)
            throw std::runtime_error(error_message());
        if (!owned)
            throw std::runtime_error("mt5bridge returned an empty response");
        return std::string(owned.get());
    }

    /// \brief Returns diagnostic text from the loaded runtime.
    /// \return Borrowed UTF-8 text, or nullptr when unavailable.
    const char *last_error() const noexcept { return last_error_ ? last_error_() : nullptr; }

    /// \brief Copies the current account identity and capabilities.
    /// \return Typed account snapshot owned by the caller.
    /// \throws std::runtime_error If the runtime is unavailable or the snapshot cannot be read.
    Mt5AccountInfo account_info() {
        check_loaded();
        Mt5AccountInfo info{};
        if (account_info_(&info) != 0)
            throw std::runtime_error(error_message());
        return info;
    }

    /// \brief Copies execution and sizing capabilities for one symbol.
    /// \param request Symbol request and reserved-field contract.
    /// \return Typed symbol capability snapshot owned by the caller.
    /// \throws std::runtime_error If the runtime is unavailable or the symbol cannot be read.
    Mt5SymbolCapabilities symbol_capabilities(const Mt5SymbolRequest &request) {
        check_loaded();
        Mt5SymbolCapabilities capabilities{};
        if (symbol_capabilities_(&request, &capabilities) != 0)
            throw std::runtime_error(error_message());
        return capabilities;
    }

    /// \brief Convenience overload for symbol capability lookup.
    /// \param symbol MetaTrader symbol encoded as UTF-8.
    /// \return Typed symbol capability snapshot owned by the caller.
    /// \throws std::runtime_error If the runtime is unavailable or the symbol cannot be read.
    Mt5SymbolCapabilities symbol_capabilities(const std::string &symbol) {
        return symbol_capabilities(Mt5SymbolRequest{symbol.c_str(), 0});
    }

    /// \brief Runs the advisory MetaTrader order_check operation.
    /// \param request Plain-C order-check request; no order is submitted.
    /// \return Raw advisory result, including rejection retcodes.
    /// \throws std::runtime_error If the runtime call fails before returning a result.
    Mt5OrderCheckResult order_check(const Mt5OrderCheckRequest &request) {
        check_loaded();
        Mt5OrderCheckResult result{};
        if (order_check_(&request, &result) != 0)
            throw std::runtime_error(error_message());
        return result;
    }

    /// \brief Retrieves active orders into application-owned storage.
    /// \param request Optional symbol/group/ticket filters.
    /// \return Typed order evidence copied from the DLL.
    /// \throws std::runtime_error If the snapshot cannot be read.
    std::vector<Mt5OrderSnapshot> orders(const Mt5OrdersRequest &request = {}) {
        check_loaded();
        Mt5OrderBuffer *buffer = nullptr;
        if (query_orders_(&request, &buffer) != 0)
            throw std::runtime_error(error_message());
        try {
            const auto *data = order_buffer_data_(buffer);
            const auto count = order_buffer_size_(buffer);
            std::vector<Mt5OrderSnapshot> result;
            if (count)
                result.assign(data, data + count);
            order_buffer_free_(buffer);
            return result;
        } catch (...) {
            order_buffer_free_(buffer);
            throw;
        }
    }

    /// \brief Retrieves active positions into application-owned storage.
    /// \param request Optional symbol/group/ticket/identifier filters.
    /// \return Typed position evidence copied from the DLL.
    /// \throws std::runtime_error If the snapshot cannot be read.
    std::vector<Mt5PositionSnapshot> positions(const Mt5PositionsRequest &request = {}) {
        check_loaded();
        Mt5PositionBuffer *buffer = nullptr;
        if (query_positions_(&request, &buffer) != 0)
            throw std::runtime_error(error_message());
        try {
            const auto *data = position_buffer_data_(buffer);
            const auto count = position_buffer_size_(buffer);
            std::vector<Mt5PositionSnapshot> result;
            if (count)
                result.assign(data, data + count);
            position_buffer_free_(buffer);
            return result;
        } catch (...) {
            position_buffer_free_(buffer);
            throw;
        }
    }

    /// \brief Retrieves history orders for an inclusive UTC millisecond range.
    /// \param request Time range and optional group/ticket/position filters.
    /// \return Typed history-order evidence copied from the DLL.
    /// \throws std::runtime_error If the snapshot cannot be read.
    std::vector<Mt5HistoryOrderSnapshot> history_orders(const Mt5HistoryOrdersRequest &request) {
        check_loaded();
        Mt5HistoryOrderBuffer *buffer = nullptr;
        if (query_history_orders_(&request, &buffer) != 0)
            throw std::runtime_error(error_message());
        try {
            const auto *data = history_order_buffer_data_(buffer);
            const auto count = history_order_buffer_size_(buffer);
            std::vector<Mt5HistoryOrderSnapshot> result;
            if (count)
                result.assign(data, data + count);
            history_order_buffer_free_(buffer);
            return result;
        } catch (...) {
            history_order_buffer_free_(buffer);
            throw;
        }
    }

    /// \brief Retrieves history deals for an inclusive UTC millisecond range.
    /// \param request Time range and optional group/ticket/position filters.
    /// \return Typed deal evidence copied from the DLL.
    /// \throws std::runtime_error If the snapshot cannot be read.
    std::vector<Mt5DealSnapshot> history_deals(const Mt5HistoryDealsRequest &request) {
        check_loaded();
        Mt5DealBuffer *buffer = nullptr;
        if (query_history_deals_(&request, &buffer) != 0)
            throw std::runtime_error(error_message());
        try {
            const auto *data = deal_buffer_data_(buffer);
            const auto count = deal_buffer_size_(buffer);
            std::vector<Mt5DealSnapshot> result;
            if (count)
                result.assign(data, data + count);
            deal_buffer_free_(buffer);
            return result;
        } catch (...) {
            deal_buffer_free_(buffer);
            throw;
        }
    }

    /// \brief Copies diagnostics from the most recent market-data call on this thread.
    /// \param[out] diagnostics Destination for the diagnostic snapshot.
    /// \return True when the runtime supplied a diagnostic snapshot.
    bool last_fetch_diagnostics(Mt5FetchDiagnostics *diagnostics) const noexcept {
        return last_fetch_diagnostics_ && last_fetch_diagnostics_(diagnostics) == 0;
    }

    /// \brief Retrieves an inclusive tick range into an application-owned vector.
    /// \param request Symbol, range, and tick flags.
    /// \param[out] diagnostics Optional destination for the runtime diagnostic snapshot.
    /// \return Tick values copied from the DLL-owned result buffer.
    /// \throws std::runtime_error If the query fails.
    std::vector<Mt5Tick> copy_ticks_range(const Mt5TicksRequest &request,
                                          Mt5FetchDiagnostics *diagnostics = nullptr) {
        check_loaded();
        Mt5TickBuffer *buffer = nullptr;
        if (query_ticks_(&request, &buffer) != 0)
            throw std::runtime_error(error_message());
        try {
            if (diagnostics)
                tick_diagnostics_(buffer, diagnostics);
            const Mt5Tick *data = tick_data_(buffer);
            const auto count = tick_size_(buffer);
            std::vector<Mt5Tick> result;
            if (count)
                result.assign(data, data + count);
            tick_free_(buffer);
            return result;
        } catch (...) {
            tick_free_(buffer);
            throw;
        }
    }

    /// \brief Retrieves an inclusive tick range using individual query arguments.
    /// \param symbol MetaTrader symbol encoded as UTF-8.
    /// \param from_msc Inclusive range start as Unix milliseconds.
    /// \param to_msc Inclusive range end as Unix milliseconds.
    /// \param flags COPY_TICKS_* mask, or zero for COPY_TICKS_ALL.
    /// \param[out] diagnostics Optional destination for the runtime diagnostic snapshot.
    /// \return Tick values copied into application-owned storage.
    /// \throws std::runtime_error If the query fails.
    std::vector<Mt5Tick> copy_ticks_range(const std::string &symbol, std::int64_t from_msc,
                                          std::int64_t to_msc, std::uint32_t flags = 0,
                                          Mt5FetchDiagnostics *diagnostics = nullptr) {
        return copy_ticks_range(Mt5TicksRequest{symbol.c_str(), from_msc, to_msc, flags, 0},
                                diagnostics);
    }

    /// \brief Retrieves a best-effort inclusive rate range and its evidence.
    /// \param request Symbol, range, and timeframe.
    /// \return Values and diagnostics; partial/unproven history is returned explicitly.
    /// \throws std::runtime_error If the query fails or the V1 coverage extension is absent.
    RateQueryResult query_rates_range(const Mt5RatesRequest &request) {
        check_loaded();
        Mt5RateBuffer *buffer = nullptr;
        if (query_rates_(&request, &buffer) != 0)
            throw std::runtime_error(error_message());
        try {
            RateQueryResult result;
            if (rate_diagnostics_(buffer, &result.diagnostics) != 0 ||
                rate_coverage_(buffer, &result.coverage) != 0 ||
                result.coverage.version != MT5BRIDGE_RATE_COVERAGE_V1_VERSION)
                throw std::runtime_error("rate coverage V1 extension unavailable");
            const Mt5Rate *data = rate_data_(buffer);
            const auto count = rate_size_(buffer);
            if (count)
                result.values.assign(data, data + count);
            rate_free_(buffer);
            return result;
        } catch (...) {
            rate_free_(buffer);
            throw;
        }
    }

    /// \brief Retrieves a best-effort rate range using individual arguments.
    /// \param symbol MetaTrader symbol encoded as UTF-8.
    /// \param timeframe MetaTrader 5 TIMEFRAME_* numeric value.
    /// \param from_msc Inclusive range start as Unix milliseconds.
    /// \param to_msc Inclusive range end as Unix milliseconds.
    /// \return Values and diagnostics; partial/unproven history is returned explicitly.
    RateQueryResult query_rates_range(const std::string &symbol, std::int32_t timeframe,
                                      std::int64_t from_msc, std::int64_t to_msc) {
        return query_rates_range(Mt5RatesRequest{symbol.c_str(), from_msc, to_msc, timeframe, 0});
    }

    /// \brief Retrieves an inclusive rate range into an application-owned vector.
    /// \param request Symbol, range, and timeframe.
    /// \param[out] diagnostics Optional destination for the runtime diagnostic snapshot.
    /// \param[out] coverage Optional versioned range-coverage evidence.
    /// \return Rate values copied from the DLL-owned result buffer.
    /// \throws std::runtime_error If the query fails.
    std::vector<Mt5Rate> copy_rates_range(const Mt5RatesRequest &request,
                                          Mt5FetchDiagnostics *diagnostics = nullptr,
                                          Mt5RateCoverageV1 *coverage = nullptr) {
        const RateQueryResult result = query_rates_range(request);
        if (diagnostics)
            *diagnostics = result.diagnostics;
        if (coverage)
            *coverage = result.coverage;
        if (!result.complete())
            throw std::runtime_error(
                "rate range is partial or coverage is unproven; use query_rates_range() "
                "for best-effort data");
        return result.values;
    }

    /// \brief Retrieves an inclusive rate range using individual query arguments.
    /// \param symbol MetaTrader symbol encoded as UTF-8.
    /// \param timeframe MetaTrader 5 TIMEFRAME_* numeric value.
    /// \param from_msc Inclusive range start as Unix milliseconds.
    /// \param to_msc Inclusive range end as Unix milliseconds.
    /// \param[out] diagnostics Optional destination for the runtime diagnostic snapshot.
    /// \param[out] coverage Optional versioned range-coverage evidence.
    /// \return Rate values copied into application-owned storage.
    /// \throws std::runtime_error If the query fails.
    std::vector<Mt5Rate> copy_rates_range(const std::string &symbol, std::int32_t timeframe,
                                          std::int64_t from_msc, std::int64_t to_msc,
                                          Mt5FetchDiagnostics *diagnostics = nullptr,
                                          Mt5RateCoverageV1 *coverage = nullptr) {
        return copy_rates_range(Mt5RatesRequest{symbol.c_str(), from_msc, to_msc, timeframe, 0},
                                diagnostics, coverage);
    }

    /// \brief Streams an inclusive tick range through bounded callback chunks.
    /// \param request Symbol, range, and tick flags.
    /// \param chunk_size Maximum number of ticks delivered per callback.
    /// \param callback Consumer callback; returned pointers are transient.
    /// \param user_data Opaque context forwarded to \p callback.
    /// \return Zero after complete delivery.
    /// \throws std::runtime_error If the query fails or the callback cancels delivery.
    int copy_ticks_range(const Mt5TicksRequest &request, std::size_t chunk_size,
                         Mt5TickChunkCallback callback, void *user_data) {
        check_loaded();
        const int status = copy_ticks_chunks_(&request, chunk_size, callback, user_data);
        if (status != 0)
            throw std::runtime_error(error_message());
        return status;
    }

    /// \brief Creates a host-driven realtime tick subscription.
    Subscription subscribe_ticks(const Mt5SubscriptionRequest &request) {
        check_loaded();
        Mt5SubscriptionHandle handle{};
        if (subscribe_ticks_(&request, &handle) != 0)
            throw std::runtime_error(error_message());
        return Subscription(subscription_state_, handle);
    }

    /// \brief Convenience overload for one symbol while retaining the group ABI.
    Subscription subscribe_ticks(const std::string &symbol, std::uint32_t flags = 0,
                                 std::uint32_t interval_ms = 0,
                                 std::uint32_t max_batch = 0,
                                 std::uint32_t ring_capacity = 0) {
        Mt5TickSourceRequest source{symbol.c_str(), flags, 0};
        Mt5SubscriptionRequest request{&source, 1, interval_ms, max_batch,
                                       ring_capacity, MT5_DELIVERY_TICK_BATCH, 0, {0, 0}};
        return subscribe_ticks(request);
    }

    /// \brief Removes a subscription by handle.
    void unsubscribe(Mt5SubscriptionHandle handle) {
        check_loaded();
        if (unsubscribe_(handle) != 0)
            throw std::runtime_error(error_message());
    }

    /// \brief Removes all realtime subscriptions.
    void unsubscribe_all() {
        check_loaded();
        if (unsubscribe_all_() != 0)
            throw std::runtime_error(error_message());
    }

    /// \brief Delivers queued realtime events on the calling thread.
    int process_events(std::size_t max_events, Mt5SubscriptionEventCallback callback,
                       void *user_data) {
        check_loaded();
        const int status = process_events_(max_events, callback, user_data);
        if (status < 0)
            throw std::runtime_error(error_message());
        return status;
    }

    /// \brief Copies health diagnostics for a realtime subscription.
    bool subscription_diagnostics(Mt5SubscriptionHandle handle,
                                  Mt5SubscriptionDiagnostics *diagnostics) const noexcept {
        return subscription_diagnostics_ &&
               subscription_diagnostics_(handle, diagnostics) == 0;
    }

    /// \brief Copies diagnostics for one source in a grouped subscription.
    bool subscription_source_diagnostics(Mt5SubscriptionHandle handle, std::uint32_t source_index,
                                         Mt5SubscriptionDiagnostics *diagnostics) const noexcept {
        return subscription_source_diagnostics_ &&
               subscription_source_diagnostics_(handle, source_index, diagnostics) == 0;
    }

private:
    using AbiVersion = std::uint32_t (*)();
    using TradeApiVersion = std::uint32_t (*)();
    using Initialize = int (*)(const wchar_t *);
    using Shutdown = int (*)();
    using EvalJson = int (*)(const char *, char **);
    using Free = void (*)(char *);
    using LastError = const char *(*)();
    using QueryTicks = int (*)(const Mt5TicksRequest *, Mt5TickBuffer **);
    using TickData = const Mt5Tick *(*)(const Mt5TickBuffer *);
    using TickSize = std::size_t (*)(const Mt5TickBuffer *);
    using TickFree = void (*)(Mt5TickBuffer *);
    using TickDiagnostics = int (*)(const Mt5TickBuffer *, Mt5FetchDiagnostics *);
    using CopyTicksChunks = int (*)(const Mt5TicksRequest *, std::size_t,
                                    Mt5TickChunkCallback, void *);
    using QueryRates = int (*)(const Mt5RatesRequest *, Mt5RateBuffer **);
    using RateData = const Mt5Rate *(*)(const Mt5RateBuffer *);
    using RateSize = std::size_t (*)(const Mt5RateBuffer *);
    using RateFree = void (*)(Mt5RateBuffer *);
    using RateDiagnostics = int (*)(const Mt5RateBuffer *, Mt5FetchDiagnostics *);
    using RateCoverage = int (*)(const Mt5RateBuffer *, Mt5RateCoverageV1 *);
    using LastFetchDiagnostics = int (*)(Mt5FetchDiagnostics *);
    using SubscribeTicks = int (*)(const Mt5SubscriptionRequest *, Mt5SubscriptionHandle *);
    using Unsubscribe = int (*)(Mt5SubscriptionHandle);
    using UnsubscribeAll = int (*)();
    using ProcessEvents = int (*)(std::size_t, Mt5SubscriptionEventCallback, void *);
    using SubscriptionDiagnostics = int (*)(Mt5SubscriptionHandle, Mt5SubscriptionDiagnostics *);
    using SubscriptionSourceDiagnostics = int (*)(Mt5SubscriptionHandle, std::uint32_t, Mt5SubscriptionDiagnostics *);
    using AccountInfo = int (*)(Mt5AccountInfo *);
    using SymbolCapabilities = int (*)(const Mt5SymbolRequest *, Mt5SymbolCapabilities *);
    using OrderCheck = int (*)(const Mt5OrderCheckRequest *, Mt5OrderCheckResult *);
    using QueryOrders = int (*)(const Mt5OrdersRequest *, Mt5OrderBuffer **);
    using OrderBufferData = const Mt5OrderSnapshot *(*)(const Mt5OrderBuffer *);
    using OrderBufferSize = std::size_t (*)(const Mt5OrderBuffer *);
    using OrderBufferFree = void (*)(Mt5OrderBuffer *);
    using QueryPositions = int (*)(const Mt5PositionsRequest *, Mt5PositionBuffer **);
    using PositionBufferData = const Mt5PositionSnapshot *(*)(const Mt5PositionBuffer *);
    using PositionBufferSize = std::size_t (*)(const Mt5PositionBuffer *);
    using PositionBufferFree = void (*)(Mt5PositionBuffer *);
    using QueryHistoryOrders = int (*)(const Mt5HistoryOrdersRequest *, Mt5HistoryOrderBuffer **);
    using HistoryOrderBufferData = const Mt5HistoryOrderSnapshot *(*)(const Mt5HistoryOrderBuffer *);
    using HistoryOrderBufferSize = std::size_t (*)(const Mt5HistoryOrderBuffer *);
    using HistoryOrderBufferFree = void (*)(Mt5HistoryOrderBuffer *);
    using QueryHistoryDeals = int (*)(const Mt5HistoryDealsRequest *, Mt5DealBuffer **);
    using DealBufferData = const Mt5DealSnapshot *(*)(const Mt5DealBuffer *);
    using DealBufferSize = std::size_t (*)(const Mt5DealBuffer *);
    using DealBufferFree = void (*)(Mt5DealBuffer *);

    /// \brief Resolves a full DLL path and loads it with a restricted dependency search.
    /// \param path Caller-provided DLL path.
    /// \return Loaded module handle, or nullptr when the module cannot be loaded.
    HMODULE load_module(const wchar_t *path) const {
        if (!path || !*path)
            return nullptr;
        const DWORD capacity = GetFullPathNameW(path, 0, nullptr, nullptr);
        if (!capacity)
            return nullptr;
        std::vector<wchar_t> absolute_path(static_cast<std::size_t>(capacity) + 1);
        const DWORD length = GetFullPathNameW(path, static_cast<DWORD>(absolute_path.size()),
                                               absolute_path.data(), nullptr);
        if (!length || length >= absolute_path.size())
            return nullptr;
        return LoadLibraryExW(absolute_path.data(), nullptr,
                              LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                  LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    }

    /// \brief Resolves a DLL symbol and converts it to the requested function type.
    /// \tparam Function Function-pointer type associated with the symbol.
    /// \param name Null-terminated exported symbol name.
    /// \return Resolved function pointer, or nullptr when the symbol is absent.
    template <typename Function>
    Function resolve(const char *name) const noexcept {
        return reinterpret_cast<Function>(GetProcAddress(module_, name));
    }

    /// \brief Ensures an operation has a loaded runtime available.
    /// \throws std::runtime_error If this client is unloaded.
    void check_loaded() const {
        if (!module_)
            throw std::runtime_error("mt5bridge client is not loaded");
    }

    /// \brief Copies the current runtime error or creates a generic fallback.
    /// \return Application-owned diagnostic text.
    std::string error_message() const {
        const char *message = last_error();
        return message ? message : "mt5bridge operation failed";
    }

    /// \brief Transfers all resolved symbols and lifecycle state from another client.
    /// \param other Client whose ownership is transferred.
    void move_from(Client &other) noexcept {
        module_ = other.module_;
        abi_version_ = other.abi_version_;
        trade_api_version_ = other.trade_api_version_;
        initialize_ = other.initialize_;
        shutdown_ = other.shutdown_;
        eval_json_ = other.eval_json_;
        free_ = other.free_;
        last_error_ = other.last_error_;
        query_ticks_ = other.query_ticks_;
        tick_data_ = other.tick_data_;
        tick_size_ = other.tick_size_;
        tick_free_ = other.tick_free_;
        tick_diagnostics_ = other.tick_diagnostics_;
        copy_ticks_chunks_ = other.copy_ticks_chunks_;
        query_rates_ = other.query_rates_;
        rate_data_ = other.rate_data_;
        rate_size_ = other.rate_size_;
        rate_free_ = other.rate_free_;
        rate_diagnostics_ = other.rate_diagnostics_;
        rate_coverage_ = other.rate_coverage_;
        last_fetch_diagnostics_ = other.last_fetch_diagnostics_;
        subscribe_ticks_ = other.subscribe_ticks_;
        unsubscribe_ = other.unsubscribe_;
        unsubscribe_all_ = other.unsubscribe_all_;
        process_events_ = other.process_events_;
        subscription_diagnostics_ = other.subscription_diagnostics_;
        subscription_source_diagnostics_ = other.subscription_source_diagnostics_;
        account_info_ = other.account_info_;
        symbol_capabilities_ = other.symbol_capabilities_;
        order_check_ = other.order_check_;
        query_orders_ = other.query_orders_;
        order_buffer_data_ = other.order_buffer_data_;
        order_buffer_size_ = other.order_buffer_size_;
        order_buffer_free_ = other.order_buffer_free_;
        query_positions_ = other.query_positions_;
        position_buffer_data_ = other.position_buffer_data_;
        position_buffer_size_ = other.position_buffer_size_;
        position_buffer_free_ = other.position_buffer_free_;
        query_history_orders_ = other.query_history_orders_;
        history_order_buffer_data_ = other.history_order_buffer_data_;
        history_order_buffer_size_ = other.history_order_buffer_size_;
        history_order_buffer_free_ = other.history_order_buffer_free_;
        query_history_deals_ = other.query_history_deals_;
        deal_buffer_data_ = other.deal_buffer_data_;
        deal_buffer_size_ = other.deal_buffer_size_;
        deal_buffer_free_ = other.deal_buffer_free_;
        subscription_state_ = std::move(other.subscription_state_);
        initialized_ = other.initialized_;
        owner_thread_ = other.owner_thread_;
        if (other.initialized_)
            runtime_claimed_ = true;
        Client *expected = &other;
        active_client_.compare_exchange_strong(expected, this);
        other.module_ = nullptr;
        other.abi_version_ = nullptr;
        other.trade_api_version_ = nullptr;
        other.initialize_ = nullptr;
        other.shutdown_ = nullptr;
        other.eval_json_ = nullptr;
        other.free_ = nullptr;
        other.last_error_ = nullptr;
        other.query_ticks_ = nullptr;
        other.tick_data_ = nullptr;
        other.tick_size_ = nullptr;
        other.tick_free_ = nullptr;
        other.tick_diagnostics_ = nullptr;
        other.copy_ticks_chunks_ = nullptr;
        other.query_rates_ = nullptr;
        other.rate_data_ = nullptr;
        other.rate_size_ = nullptr;
        other.rate_free_ = nullptr;
        other.rate_diagnostics_ = nullptr;
        other.rate_coverage_ = nullptr;
        other.last_fetch_diagnostics_ = nullptr;
        other.subscribe_ticks_ = nullptr;
        other.unsubscribe_ = nullptr;
        other.unsubscribe_all_ = nullptr;
        other.process_events_ = nullptr;
        other.subscription_diagnostics_ = nullptr;
        other.subscription_source_diagnostics_ = nullptr;
        other.account_info_ = nullptr;
        other.symbol_capabilities_ = nullptr;
        other.order_check_ = nullptr;
        other.query_orders_ = nullptr;
        other.order_buffer_data_ = nullptr;
        other.order_buffer_size_ = nullptr;
        other.order_buffer_free_ = nullptr;
        other.query_positions_ = nullptr;
        other.position_buffer_data_ = nullptr;
        other.position_buffer_size_ = nullptr;
        other.position_buffer_free_ = nullptr;
        other.query_history_orders_ = nullptr;
        other.history_order_buffer_data_ = nullptr;
        other.history_order_buffer_size_ = nullptr;
        other.history_order_buffer_free_ = nullptr;
        other.query_history_deals_ = nullptr;
        other.deal_buffer_data_ = nullptr;
        other.deal_buffer_size_ = nullptr;
        other.deal_buffer_free_ = nullptr;
        other.initialized_ = false;
        other.owner_thread_ = std::thread::id{};
    }

    HMODULE module_ = nullptr;
    AbiVersion abi_version_ = nullptr;
    TradeApiVersion trade_api_version_ = nullptr;
    Initialize initialize_ = nullptr;
    Shutdown shutdown_ = nullptr;
    EvalJson eval_json_ = nullptr;
    Free free_ = nullptr;
    LastError last_error_ = nullptr;
    QueryTicks query_ticks_ = nullptr;
    TickData tick_data_ = nullptr;
    TickSize tick_size_ = nullptr;
    TickFree tick_free_ = nullptr;
    TickDiagnostics tick_diagnostics_ = nullptr;
    CopyTicksChunks copy_ticks_chunks_ = nullptr;
    QueryRates query_rates_ = nullptr;
    RateData rate_data_ = nullptr;
    RateSize rate_size_ = nullptr;
    RateFree rate_free_ = nullptr;
    RateDiagnostics rate_diagnostics_ = nullptr;
    RateCoverage rate_coverage_ = nullptr;
    LastFetchDiagnostics last_fetch_diagnostics_ = nullptr;
    SubscribeTicks subscribe_ticks_ = nullptr;
    Unsubscribe unsubscribe_ = nullptr;
    UnsubscribeAll unsubscribe_all_ = nullptr;
    ProcessEvents process_events_ = nullptr;
    SubscriptionDiagnostics subscription_diagnostics_ = nullptr;
    SubscriptionSourceDiagnostics subscription_source_diagnostics_ = nullptr;
    AccountInfo account_info_ = nullptr;
    SymbolCapabilities symbol_capabilities_ = nullptr;
    OrderCheck order_check_ = nullptr;
    QueryOrders query_orders_ = nullptr;
    OrderBufferData order_buffer_data_ = nullptr;
    OrderBufferSize order_buffer_size_ = nullptr;
    OrderBufferFree order_buffer_free_ = nullptr;
    QueryPositions query_positions_ = nullptr;
    PositionBufferData position_buffer_data_ = nullptr;
    PositionBufferSize position_buffer_size_ = nullptr;
    PositionBufferFree position_buffer_free_ = nullptr;
    QueryHistoryOrders query_history_orders_ = nullptr;
    HistoryOrderBufferData history_order_buffer_data_ = nullptr;
    HistoryOrderBufferSize history_order_buffer_size_ = nullptr;
    HistoryOrderBufferFree history_order_buffer_free_ = nullptr;
    QueryHistoryDeals query_history_deals_ = nullptr;
    DealBufferData deal_buffer_data_ = nullptr;
    DealBufferSize deal_buffer_size_ = nullptr;
    DealBufferFree deal_buffer_free_ = nullptr;
    std::shared_ptr<SubscriptionState> subscription_state_;
    bool initialized_ = false;
    std::thread::id owner_thread_;
    inline static std::mutex ownership_mutex_;
    inline static std::atomic<Client *> active_client_{nullptr};
    inline static std::atomic<bool> runtime_claimed_{false};
};

} // namespace mt5bridge
