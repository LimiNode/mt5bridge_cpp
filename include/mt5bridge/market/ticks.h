#pragma once

/// \file market/ticks.h
/// \brief Declares tick-history data and query operations.

#include <mt5bridge/market/common.h>

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

/// \struct Mt5TicksRequest
/// \brief Defines an inclusive tick-history query.
typedef struct Mt5TicksRequest {
    const char *symbol_utf8; ///< Borrowed null-terminated UTF-8 symbol name.
    int64_t from_msc;        ///< Inclusive range start as Unix milliseconds.
    int64_t to_msc;          ///< Inclusive range end as Unix milliseconds.
    uint32_t flags;          ///< COPY_TICKS_* mask, or zero for COPY_TICKS_ALL.
    uint32_t reserved;       ///< Reserved for ABI-compatible extensions; must be zero.
} Mt5TicksRequest;

/// \struct Mt5TickBuffer
/// \brief Opaque DLL-owned collection of Mt5Tick values.
typedef struct Mt5TickBuffer Mt5TickBuffer;

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
static_assert(sizeof(Mt5TicksRequest) == 32, "Mt5TicksRequest ABI size changed");
static_assert(offsetof(Mt5TicksRequest, flags) == 24,
              "Mt5TicksRequest::flags ABI offset changed");
static_assert(offsetof(Mt5TicksRequest, reserved) == 28,
              "Mt5TicksRequest::reserved ABI offset changed");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(Mt5Tick) == 56, "Mt5Tick ABI size changed");
_Static_assert(offsetof(Mt5Tick, volume) == 32, "Mt5Tick ABI offsets changed");
_Static_assert(offsetof(Mt5Tick, volume_real) == 40, "Mt5Tick ABI offsets changed");
_Static_assert(sizeof(Mt5TicksRequest) == 32, "Mt5TicksRequest ABI size changed");
_Static_assert(offsetof(Mt5TicksRequest, reserved) == 28,
               "Mt5TicksRequest ABI offsets changed");
#endif

#ifdef __cplusplus
}
#endif
