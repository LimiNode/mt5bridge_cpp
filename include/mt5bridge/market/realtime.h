#pragma once

/// \file market/realtime.h
/// \brief Declares realtime tick subscription and delivery contracts.

#include <mt5bridge/market/ticks.h>

/// \brief Enables tick-batch delivery for a subscription.
#define MT5_DELIVERY_TICK_BATCH 0x01u
/// \brief Enables coherent-snapshot delivery for a subscription.
#define MT5_DELIVERY_COHERENT_SNAPSHOT 0x02u

#ifdef __cplusplus
extern "C" {
#endif

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
    uint32_t max_batch;                  ///< Maximum delivered ticks per batch; zero selects 1024.
    uint32_t ring_capacity;              ///< Maximum retained batches; zero selects 64 (combined bound applies).
    uint32_t delivery_flags;             ///< MT5_DELIVERY_* bit mask; zero defaults to MT5_DELIVERY_TICK_BATCH.
    uint32_t stale_after_ms;             ///< Snapshot staleness threshold; zero disables.
    uint32_t reserved[2];                ///< Reserved; must be zero.
} Mt5SubscriptionRequest;

/// \struct Mt5SubscriptionHandle
/// \brief Generation-qualified identity of a logical subscription.
typedef struct Mt5SubscriptionHandle {
    uint64_t generation; ///< Runtime generation that created the handle.
    uint64_t id;         ///< Monotonically increasing subscription id.
} Mt5SubscriptionHandle;

/// \typedef Mt5SubscriptionStatus
/// \brief Lifecycle state reported for a realtime source.
typedef int32_t Mt5SubscriptionStatus;
/// \brief The source is starting.
#define MT5_SUBSCRIPTION_STARTING ((Mt5SubscriptionStatus)0)
/// \brief The source is ready.
#define MT5_SUBSCRIPTION_READY ((Mt5SubscriptionStatus)1)
/// \brief The source is reconnecting.
#define MT5_SUBSCRIPTION_RECONNECTING ((Mt5SubscriptionStatus)2)
/// \brief The source has failed.
#define MT5_SUBSCRIPTION_FAILED ((Mt5SubscriptionStatus)3)
/// \brief Reserved stopped-state value for a future retained-handle lifecycle event.
#define MT5_SUBSCRIPTION_STOPPED ((Mt5SubscriptionStatus)4)

/// \typedef Mt5SubscriptionEventType
/// \brief Kind of event delivered by mt5bridge_process_events().
typedef int32_t Mt5SubscriptionEventType;
/// \brief A tick-batch event.
#define MT5_SUBSCRIPTION_TICK_BATCH ((Mt5SubscriptionEventType)0)
/// \brief A source-status event.
#define MT5_SUBSCRIPTION_STATUS ((Mt5SubscriptionEventType)1)
/// \brief A discontinuity event.
#define MT5_SUBSCRIPTION_GAP ((Mt5SubscriptionEventType)2)
/// \brief A coherent-snapshot event.
#define MT5_SUBSCRIPTION_SNAPSHOT ((Mt5SubscriptionEventType)3)

/// \typedef Mt5GapReason
/// \brief Classifies why a realtime consumer observed a discontinuity.
typedef int32_t Mt5GapReason;
/// \brief No discontinuity occurred.
#define MT5_GAP_NONE ((Mt5GapReason)0)
/// \brief The consumer queue overflowed.
#define MT5_GAP_CONSUMER_OVERFLOW ((Mt5GapReason)1)
/// \brief The upstream source produced inconsistent history.
#define MT5_GAP_SOURCE_INCONSISTENCY ((Mt5GapReason)2)

typedef struct Mt5SnapshotView Mt5SnapshotView;

/// \struct Mt5SubscriptionEvent
/// \brief Borrowed event view valid only during the process-events callback.
typedef struct Mt5SubscriptionEvent {
    Mt5SubscriptionEventType type; ///< Event kind.
    Mt5SubscriptionStatus status;  ///< Source status for status events.
    Mt5SubscriptionHandle handle;  ///< Logical subscription identity.
    uint64_t sequence;             ///< Monotonic batch/GAP sequence; zero for STATUS events.
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
    uint64_t poll_duration_us;   ///< Duration of the latest poll call.
    uint32_t history_rewrites;   ///< Observable overlap rewrite count.
    uint32_t gap_reason;         ///< Upstream inconsistency reason; consumer overflow is per member.
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
    int64_t watermark_msc;        ///< Common watermark in Unix milliseconds, or -1 when unset.
    uint32_t stale_after_ms;      ///< Configured staleness threshold.
    uint32_t stale_count;         ///< Number of stale sources.
    uint32_t reserved[2];         ///< Reserved; must be zero.
};

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
MT5BRIDGE_EXPORT int mt5bridge_subscription_diagnostics(
    Mt5SubscriptionHandle handle, Mt5SubscriptionDiagnostics *diagnostics);

/// \brief Returns diagnostics for one source in a grouped subscription.
/// \param handle Generation-qualified logical subscription handle.
/// \param source_index Zero-based source index from Mt5SubscriptionRequest::sources.
/// \param[out] diagnostics Receives a source health snapshot.
/// \return Zero on success; non-zero for stale handles or invalid indexes.
MT5BRIDGE_EXPORT int mt5bridge_subscription_source_diagnostics(
    Mt5SubscriptionHandle handle, uint32_t source_index,
    Mt5SubscriptionDiagnostics *diagnostics);

#if defined(__cplusplus)
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

#ifdef __cplusplus
}
#endif
