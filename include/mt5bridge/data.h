#pragma once

/// \file data.h
/// \brief Declares POD market-data types and the high-throughput C data plane.

#include <stdint.h>
#include <stddef.h>

#include "abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/// \struct Mt5Tick
/// \brief Plain tick representation suitable for transfer across the DLL boundary.
typedef struct Mt5Tick {
    int64_t time_msc;  ///< Unix timestamp in milliseconds.
    double bid;        ///< Best bid price.
    double ask;        ///< Best ask price.
    double last;       ///< Last trade price.
    uint64_t volume;    ///< Integer tick volume reported by MetaTrader 5.
    double volume_real; ///< Exchange-reported real volume when available.
    uint32_t flags;     ///< MetaTrader 5 tick flags bit mask.
    uint32_t reserved;  ///< Reserved for ABI-compatible extensions; must be zero.
} Mt5Tick;

/// \struct Mt5Rate
/// \brief Plain OHLC bar representation suitable for transfer across the DLL boundary.
typedef struct Mt5Rate {
    int64_t time;        ///< Bar open time as a Unix timestamp in seconds.
    double open;         ///< Opening price.
    double high;         ///< Highest price.
    double low;          ///< Lowest price.
    double close;        ///< Closing price.
    int64_t tick_volume; ///< Number of ticks in the bar.
    int32_t spread;      ///< Spread recorded for the bar, in points.
    int32_t reserved;    ///< Reserved for ABI-compatible extensions; must be zero.
    int64_t real_volume; ///< Exchange-reported volume when available.
} Mt5Rate;

/// \struct Mt5TicksRequest
/// \brief Defines an inclusive tick-history query.
typedef struct Mt5TicksRequest {
    const char *symbol_utf8; ///< Borrowed null-terminated UTF-8 symbol name.
    int64_t from_msc;        ///< Inclusive range start as Unix milliseconds.
    int64_t to_msc;          ///< Inclusive range end as Unix milliseconds.
    uint32_t flags;          ///< COPY_TICKS_* mask, or zero for COPY_TICKS_ALL.
    uint32_t reserved;       ///< Reserved for ABI-compatible extensions; must be zero.
} Mt5TicksRequest;

/// \struct Mt5RatesRequest
/// \brief Defines an inclusive bar-history query.
typedef struct Mt5RatesRequest {
    const char *symbol_utf8; ///< Borrowed null-terminated UTF-8 symbol name.
    int64_t from_msc;        ///< Inclusive range start as Unix milliseconds.
    int64_t to_msc;          ///< Inclusive range end as Unix milliseconds.
    int32_t timeframe;       ///< MetaTrader 5 TIMEFRAME_* numeric value.
    int32_t reserved;        ///< Reserved for ABI-compatible extensions; must be zero.
} Mt5RatesRequest;

/// \typedef Mt5FetchStatus
/// \brief Fixed-width semantic outcome of a market-data read.
typedef int32_t Mt5FetchStatus;

/// \name Mt5FetchStatus values
/// \{
/// \brief The requested read completed with data.
#define MT5_FETCH_COMPLETE ((Mt5FetchStatus)0)
/// \brief The read completed successfully without data.
#define MT5_FETCH_EMPTY ((Mt5FetchStatus)1)
/// \brief Transient recovery attempts were exhausted.
#define MT5_FETCH_RETRY_EXHAUSTED ((Mt5FetchStatus)2)
/// \brief A non-recoverable error stopped the read.
#define MT5_FETCH_FATAL_ERROR ((Mt5FetchStatus)3)
/// \brief Data was returned but the terminal could not prove range completeness.
#define MT5_FETCH_PARTIAL ((Mt5FetchStatus)4)
/// \}

/// \struct Mt5FetchDiagnostics
/// \brief Reports recovery activity and completeness for a market-data read.
typedef struct Mt5FetchDiagnostics {
    uint32_t attempts;               ///< Total calls made to the underlying history API.
    uint32_t retries;                ///< Calls made after an initial transient failure.
    uint32_t reconnects;             ///< Terminal reconnects performed during recovery.
    int32_t last_mt5_error;          ///< Last classified MetaTrader error code, or zero.
    uint8_t history_warmup_detected; ///< Non-zero when history synchronization was observed.
    uint8_t complete;                ///< Non-zero only when the requested read is complete.
    uint8_t reserved[2];             ///< Reserved for ABI-compatible extensions; must be zero.
    Mt5FetchStatus status;           ///< Final semantic status of the read.
} Mt5FetchDiagnostics;

/// \struct Mt5TickSourceRequest
/// \brief Describes one physical symbol source in a subscription group.
typedef struct Mt5TickSourceRequest {
    const char *symbol_utf8; ///< Borrowed UTF-8 symbol name.
    uint32_t flags;          ///< COPY_TICKS_* mask, or zero for all ticks.
    uint32_t reserved;       ///< Reserved; must be zero.
} Mt5TickSourceRequest;

/// \struct Mt5SubscriptionRequest
/// \brief Describes a single- or multi-symbol realtime subscription group.
typedef struct Mt5SubscriptionRequest {
    const Mt5TickSourceRequest *sources; ///< Borrowed source array.
    size_t source_count;                 ///< Number of source entries; must be non-zero.
    uint32_t interval_ms;                ///< Polling cadence; zero selects 250 ms.
    uint32_t max_batch;                  ///< Maximum ticks per poll; zero selects 1024.
    uint32_t ring_capacity;              ///< Maximum retained batches; zero selects 64.
    uint32_t delivery_flags;             ///< MT5_DELIVERY_* bit mask.
    uint32_t stale_after_ms;             ///< Snapshot staleness threshold; zero disables.
    uint32_t reserved[2];                ///< Reserved; must be zero.
} Mt5SubscriptionRequest;

#define MT5_DELIVERY_TICK_BATCH 0x01u
#define MT5_DELIVERY_COHERENT_SNAPSHOT 0x02u

/// \struct Mt5SubscriptionHandle
/// \brief Generation-qualified identity of a logical subscription.
typedef struct Mt5SubscriptionHandle {
    uint64_t generation; ///< Runtime generation that created the handle.
    uint64_t id;         ///< Monotonically increasing subscription id.
} Mt5SubscriptionHandle;

/// \typedef Mt5SubscriptionStatus
/// \brief Lifecycle state reported for a realtime source.
typedef int32_t Mt5SubscriptionStatus;
#define MT5_SUBSCRIPTION_STARTING ((Mt5SubscriptionStatus)0)
#define MT5_SUBSCRIPTION_READY ((Mt5SubscriptionStatus)1)
#define MT5_SUBSCRIPTION_RECONNECTING ((Mt5SubscriptionStatus)2)
#define MT5_SUBSCRIPTION_FAILED ((Mt5SubscriptionStatus)3)
#define MT5_SUBSCRIPTION_STOPPED ((Mt5SubscriptionStatus)4)

/// \typedef Mt5SubscriptionEventType
/// \brief Kind of event delivered by mt5bridge_process_events().
typedef int32_t Mt5SubscriptionEventType;
#define MT5_SUBSCRIPTION_TICK_BATCH ((Mt5SubscriptionEventType)0)
#define MT5_SUBSCRIPTION_STATUS ((Mt5SubscriptionEventType)1)
#define MT5_SUBSCRIPTION_GAP ((Mt5SubscriptionEventType)2)
#define MT5_SUBSCRIPTION_SNAPSHOT ((Mt5SubscriptionEventType)3)

/// \typedef Mt5GapReason
/// \brief Classifies why a realtime consumer observed a discontinuity.
typedef int32_t Mt5GapReason;
#define MT5_GAP_NONE ((Mt5GapReason)0)
#define MT5_GAP_CONSUMER_OVERFLOW ((Mt5GapReason)1)
#define MT5_GAP_SOURCE_INCONSISTENCY ((Mt5GapReason)2)

typedef struct Mt5SnapshotView Mt5SnapshotView;

/// \struct Mt5SubscriptionEvent
/// \brief Borrowed event view valid only during the process-events callback.
typedef struct Mt5SubscriptionEvent {
    Mt5SubscriptionEventType type; ///< Event kind.
    Mt5SubscriptionStatus status;  ///< Source status for status events.
    Mt5SubscriptionHandle handle;  ///< Logical subscription identity.
    uint64_t sequence;             ///< Batch sequence, or last observed sequence.
    uint32_t source_index;         ///< Index in the request source array.
    const Mt5Tick *ticks;          ///< Borrowed tick batch for TICK_BATCH events.
    size_t count;                  ///< Number of elements in \p ticks.
    uint64_t dropped;              ///< Number of batches lost before a GAP event.
    Mt5GapReason gap_reason;       ///< Reason for GAP events, or MT5_GAP_NONE.
    int64_t recovery_from_msc;     ///< Inclusive recovery range for source GAP events.
    int64_t recovery_to_msc;       ///< Inclusive recovery range for source GAP events.
    const Mt5SnapshotView *snapshot; ///< Snapshot payload, or NULL for non-snapshot events.
} Mt5SubscriptionEvent;

/// \struct Mt5SubscriptionDiagnostics
/// \brief Latest source health and reconciliation counters.
typedef struct Mt5SubscriptionDiagnostics {
    int32_t last_mt5_error;      ///< Last MT5 error observed by the source.
    uint32_t reconnects;         ///< Successful terminal reconnects.
    uint32_t consecutive_failures; ///< Consecutive failed polls.
    int64_t history_lag_ms;      ///< Age of the newest observed tick.
    uint64_t poll_duration_us;    ///< Duration of the latest poll call.
    uint32_t history_rewrites;   ///< Observable overlap rewrite count.
    uint32_t gap_reason;          ///< Upstream inconsistency reason; consumer overflow is per member.
    Mt5SubscriptionStatus status; ///< Current source status.
} Mt5SubscriptionDiagnostics;

/// \brief Callback invoked by mt5bridge_process_events().
typedef int (*Mt5SubscriptionEventCallback)(const Mt5SubscriptionEvent *event,
                                            void *user_data);

/// \struct Mt5SnapshotItem
/// \brief Reserved coherent-snapshot item representation for ABI-compatible evolution.
typedef struct Mt5SnapshotItem {
    uint32_t source_index; ///< Source index within the subscription request.
    uint32_t flags;        ///< Item flags; reserved for future snapshot states.
    Mt5Tick tick;          ///< Latest tick observed for the source.
} Mt5SnapshotItem;

/// \struct Mt5SnapshotView
/// \brief Reserved borrowed view for future coherent multi-source snapshots.
struct Mt5SnapshotView {
    const Mt5SnapshotItem *items; ///< Borrowed item array.
    size_t count;                 ///< Number of items in \p items.
    uint64_t watermark_msc;       ///< Common watermark in Unix milliseconds.
    uint32_t stale_after_ms;      ///< Configured staleness threshold.
    uint32_t stale_count;         ///< Number of stale sources.
    uint32_t reserved[2];         ///< Reserved; must be zero.
};

#if defined(__cplusplus)
static_assert(sizeof(Mt5Tick) == 56, "Mt5Tick ABI size changed");
static_assert(offsetof(Mt5Tick, time_msc) == 0, "Mt5Tick::time_msc ABI offset changed");
static_assert(offsetof(Mt5Tick, bid) == 8, "Mt5Tick::bid ABI offset changed");
static_assert(offsetof(Mt5Tick, ask) == 16, "Mt5Tick::ask ABI offset changed");
static_assert(offsetof(Mt5Tick, last) == 24, "Mt5Tick::last ABI offset changed");
static_assert(offsetof(Mt5Tick, volume) == 32, "Mt5Tick::volume ABI offset changed");
static_assert(offsetof(Mt5Tick, volume_real) == 40,
              "Mt5Tick::volume_real ABI offset changed");
static_assert(offsetof(Mt5Tick, flags) == 48, "Mt5Tick::flags ABI offset changed");
static_assert(offsetof(Mt5Tick, reserved) == 52, "Mt5Tick::reserved ABI offset changed");
static_assert(sizeof(Mt5Rate) == 64, "Mt5Rate ABI size changed");
static_assert(offsetof(Mt5Rate, time) == 0, "Mt5Rate::time ABI offset changed");
static_assert(offsetof(Mt5Rate, tick_volume) == 40,
              "Mt5Rate::tick_volume ABI offset changed");
static_assert(offsetof(Mt5Rate, spread) == 48, "Mt5Rate::spread ABI offset changed");
static_assert(offsetof(Mt5Rate, real_volume) == 56,
              "Mt5Rate::real_volume ABI offset changed");
static_assert(sizeof(Mt5TicksRequest) == 32, "Mt5TicksRequest ABI size changed");
static_assert(offsetof(Mt5TicksRequest, flags) == 24,
              "Mt5TicksRequest::flags ABI offset changed");
static_assert(offsetof(Mt5TicksRequest, reserved) == 28,
              "Mt5TicksRequest::reserved ABI offset changed");
static_assert(sizeof(Mt5RatesRequest) == 32, "Mt5RatesRequest ABI size changed");
static_assert(offsetof(Mt5RatesRequest, timeframe) == 24,
              "Mt5RatesRequest::timeframe ABI offset changed");
static_assert(sizeof(Mt5FetchStatus) == 4, "Mt5FetchStatus ABI size changed");
static_assert(sizeof(Mt5FetchDiagnostics) == 24,
              "Mt5FetchDiagnostics ABI size changed");
static_assert(offsetof(Mt5FetchDiagnostics, status) == 20,
              "Mt5FetchDiagnostics::status ABI offset changed");
static_assert(sizeof(Mt5TickSourceRequest) == 16,
              "Mt5TickSourceRequest ABI size changed");
static_assert(sizeof(Mt5SubscriptionRequest) == 48,
              "Mt5SubscriptionRequest ABI size changed");
static_assert(sizeof(Mt5SubscriptionHandle) == 16,
              "Mt5SubscriptionHandle ABI size changed");
static_assert(sizeof(Mt5SubscriptionEvent) == 96,
              "Mt5SubscriptionEvent ABI size changed");
static_assert(sizeof(Mt5SubscriptionDiagnostics) == 48,
              "Mt5SubscriptionDiagnostics ABI size changed");
static_assert(sizeof(Mt5SnapshotItem) == 64,
              "Mt5SnapshotItem ABI size changed");
static_assert(sizeof(Mt5SnapshotView) == 40,
              "Mt5SnapshotView ABI size changed");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(Mt5Tick) == 56, "Mt5Tick ABI size changed");
_Static_assert(offsetof(Mt5Tick, volume) == 32, "Mt5Tick ABI offsets changed");
_Static_assert(offsetof(Mt5Tick, volume_real) == 40, "Mt5Tick ABI offsets changed");
_Static_assert(sizeof(Mt5Rate) == 64, "Mt5Rate ABI size changed");
_Static_assert(offsetof(Mt5Rate, real_volume) == 56, "Mt5Rate ABI offsets changed");
_Static_assert(sizeof(Mt5TicksRequest) == 32, "Mt5TicksRequest ABI size changed");
_Static_assert(offsetof(Mt5TicksRequest, reserved) == 28,
               "Mt5TicksRequest ABI offsets changed");
_Static_assert(sizeof(Mt5RatesRequest) == 32, "Mt5RatesRequest ABI size changed");
_Static_assert(sizeof(Mt5FetchStatus) == 4, "Mt5FetchStatus ABI size changed");
_Static_assert(sizeof(Mt5FetchDiagnostics) == 24,
               "Mt5FetchDiagnostics ABI size changed");
_Static_assert(sizeof(Mt5TickSourceRequest) == 16,
               "Mt5TickSourceRequest ABI size changed");
_Static_assert(sizeof(Mt5SubscriptionRequest) == 48,
               "Mt5SubscriptionRequest ABI size changed");
_Static_assert(sizeof(Mt5SubscriptionHandle) == 16,
               "Mt5SubscriptionHandle ABI size changed");
_Static_assert(sizeof(Mt5SubscriptionEvent) == 96,
               "Mt5SubscriptionEvent ABI size changed");
_Static_assert(sizeof(Mt5SubscriptionDiagnostics) == 48,
               "Mt5SubscriptionDiagnostics ABI size changed");
_Static_assert(sizeof(Mt5SnapshotItem) == 64,
               "Mt5SnapshotItem ABI size changed");
_Static_assert(sizeof(Mt5SnapshotView) == 40,
               "Mt5SnapshotView ABI size changed");
#endif


/// \struct Mt5TickBuffer
/// \brief Opaque DLL-owned collection of Mt5Tick values.
typedef struct Mt5TickBuffer Mt5TickBuffer;

/// \struct Mt5RateBuffer
/// \brief Opaque DLL-owned collection of Mt5Rate values.
typedef struct Mt5RateBuffer Mt5RateBuffer;

/// \brief Receives a transient chunk of ticks during bounded delivery.
/// \param ticks Borrowed array valid only for the duration of the callback.
/// \param count Number of elements in \p ticks.
/// \param user_data Opaque context supplied to mt5bridge_copy_ticks_range().
/// \return Zero to continue delivery; non-zero to cancel it.
typedef int (*Mt5TickChunkCallback)(const Mt5Tick *ticks, size_t count, void *user_data);

/// \brief Loads ticks into a DLL-owned result buffer.
/// \param request Tick range and filter flags.
/// \param[out] result Receives a buffer that must be released with mt5bridge_tick_buffer_free().
/// \return Zero on success; non-zero on failure.
MT5BRIDGE_EXPORT int mt5bridge_query_ticks(const Mt5TicksRequest *request,
                                            Mt5TickBuffer **result);

/// \brief Returns the immutable data stored in a tick buffer.
/// \param buffer Buffer returned by mt5bridge_query_ticks().
/// \return Borrowed contiguous data, or NULL when the buffer is empty or invalid.
MT5BRIDGE_EXPORT const Mt5Tick *mt5bridge_tick_buffer_data(const Mt5TickBuffer *buffer);

/// \brief Returns the number of ticks stored in a buffer.
/// \param buffer Buffer returned by mt5bridge_query_ticks().
/// \return Element count, or zero for a NULL buffer.
MT5BRIDGE_EXPORT size_t mt5bridge_tick_buffer_size(const Mt5TickBuffer *buffer);

/// \brief Releases a tick buffer allocated by mt5_bridge.dll.
/// \param buffer Buffer returned by mt5bridge_query_ticks(); NULL is permitted.
MT5BRIDGE_EXPORT void mt5bridge_tick_buffer_free(Mt5TickBuffer *buffer);

/// \brief Copies the diagnostics associated with a tick buffer.
/// \param buffer Buffer returned by mt5bridge_query_ticks().
/// \param[out] diagnostics Destination for the diagnostic snapshot.
/// \return Zero on success; non-zero when an argument is NULL.
MT5BRIDGE_EXPORT int mt5bridge_tick_buffer_diagnostics(const Mt5TickBuffer *buffer,
                                                       Mt5FetchDiagnostics *diagnostics);

/// \brief Retrieves ticks in bounded chunks without retaining the full range.
/// \param request Tick range and filter flags.
/// \param chunk_size Maximum number of ticks passed to one callback invocation.
/// \param callback Consumer invoked outside the Python GIL critical section.
/// \param user_data Opaque context passed unchanged to \p callback.
/// \return Zero after complete delivery; non-zero on query failure or cancellation.
/// \note The callback runs without the Python GIL or runtime mutex and may re-enter the bridge.
/// \warning Shutting down the bridge from a callback cancels the outer traversal.
MT5BRIDGE_EXPORT int mt5bridge_copy_ticks_range(const Mt5TicksRequest *request,
                                                size_t chunk_size,
                                                Mt5TickChunkCallback callback,
                                                void *user_data);

/// \brief Loads rates into a DLL-owned result buffer.
/// \param request Rate range and timeframe.
/// \param[out] result Receives a buffer that must be released with mt5bridge_rate_buffer_free().
/// \return Zero on success; non-zero on failure.
MT5BRIDGE_EXPORT int mt5bridge_query_rates(const Mt5RatesRequest *request,
                                            Mt5RateBuffer **result);

/// \brief Returns the immutable data stored in a rate buffer.
/// \param buffer Buffer returned by mt5bridge_query_rates().
/// \return Borrowed contiguous data, or NULL when the buffer is empty or invalid.
MT5BRIDGE_EXPORT const Mt5Rate *mt5bridge_rate_buffer_data(const Mt5RateBuffer *buffer);

/// \brief Returns the number of rates stored in a buffer.
/// \param buffer Buffer returned by mt5bridge_query_rates().
/// \return Element count, or zero for a NULL buffer.
MT5BRIDGE_EXPORT size_t mt5bridge_rate_buffer_size(const Mt5RateBuffer *buffer);

/// \brief Releases a rate buffer allocated by mt5_bridge.dll.
/// \param buffer Buffer returned by mt5bridge_query_rates(); NULL is permitted.
MT5BRIDGE_EXPORT void mt5bridge_rate_buffer_free(Mt5RateBuffer *buffer);

/// \brief Copies the diagnostics associated with a rate buffer.
/// \param buffer Buffer returned by mt5bridge_query_rates().
/// \param[out] diagnostics Destination for the diagnostic snapshot.
/// \return Zero on success; non-zero when an argument is NULL.
MT5BRIDGE_EXPORT int mt5bridge_rate_buffer_diagnostics(const Mt5RateBuffer *buffer,
                                                       Mt5FetchDiagnostics *diagnostics);

/// \brief Retrieves diagnostics for the most recent market-data call on this thread.
/// \param[out] diagnostics Destination for the diagnostic snapshot.
/// \return Zero on success; non-zero when \p diagnostics is NULL.
/// \note This accessor is useful when a query fails before returning a buffer.
MT5BRIDGE_EXPORT int mt5bridge_last_fetch_diagnostics(Mt5FetchDiagnostics *diagnostics);

/// \brief Creates a logical realtime tick subscription.
MT5BRIDGE_EXPORT int mt5bridge_subscribe_ticks(const Mt5SubscriptionRequest *request,
                                                Mt5SubscriptionHandle *handle);
/// \brief Removes one logical realtime subscription.
MT5BRIDGE_EXPORT int mt5bridge_unsubscribe(Mt5SubscriptionHandle handle);
/// \brief Removes all realtime subscriptions and stops polling when idle.
MT5BRIDGE_EXPORT int mt5bridge_unsubscribe_all(void);
/// \brief Delivers queued realtime events on the caller's thread.
MT5BRIDGE_EXPORT int mt5bridge_process_events(size_t max_events,
                                               Mt5SubscriptionEventCallback callback,
                                               void *user_data);
/// \brief Copies diagnostics for one realtime source.
MT5BRIDGE_EXPORT int mt5bridge_subscription_diagnostics(Mt5SubscriptionHandle handle,
                                                         Mt5SubscriptionDiagnostics *diagnostics);

/// \brief Returns diagnostics for one source in a grouped subscription.
/// \param handle Generation-qualified logical subscription handle.
/// \param source_index Zero-based source index from Mt5SubscriptionRequest::sources.
/// \param[out] diagnostics Receives a source health snapshot.
/// \return Zero on success; non-zero for stale handles or invalid indexes.
MT5BRIDGE_EXPORT int mt5bridge_subscription_source_diagnostics(
    Mt5SubscriptionHandle handle, uint32_t source_index,
    Mt5SubscriptionDiagnostics *diagnostics);

#ifdef __cplusplus
}
#endif
