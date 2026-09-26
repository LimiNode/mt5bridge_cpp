#pragma once

/// \file market/rates.h
/// \brief Declares rate-history data, coverage evidence, and query operations.

#include <mt5bridge/market/common.h>

#ifdef __cplusplus
extern "C" {
#endif

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

/// \struct Mt5RatesRequest
/// \brief Defines an inclusive bar-history query.
typedef struct Mt5RatesRequest {
    const char *symbol_utf8; ///< Borrowed null-terminated UTF-8 symbol name.
    int64_t from_msc;        ///< Inclusive range start as Unix milliseconds.
    int64_t to_msc;          ///< Inclusive range end as Unix milliseconds.
    int32_t timeframe;       ///< MetaTrader 5 TIMEFRAME_* numeric value.
    int32_t reserved;        ///< Reserved for ABI-compatible extensions; must be zero.
} Mt5RatesRequest;

/// \typedef Mt5RateCoverageState
/// \brief Versioned evidence state for a rate buffer's requested time range.
typedef int32_t Mt5RateCoverageState;

/// \name Mt5RateCoverageState values
/// \{
/// \brief The returned rows do not prove both requested range boundaries.
#define MT5_RATE_COVERAGE_UNPROVEN ((Mt5RateCoverageState)0)
/// \brief The returned rows prove the timeframe-aligned requested boundaries.
#define MT5_RATE_COVERAGE_PROVEN ((Mt5RateCoverageState)1)
/// \}

/// \brief Version of the immutable rate-coverage V1 extension.
#define MT5BRIDGE_RATE_COVERAGE_V1_VERSION 1u

/// \struct Mt5RateCoverageV1
/// \brief Describes range coverage without changing the ABI-8 diagnostics record.
typedef struct Mt5RateCoverageV1 {
    uint32_t version;                 ///< Must equal MT5BRIDGE_RATE_COVERAGE_V1_VERSION.
    Mt5RateCoverageState state;       ///< Proven or unproven coverage state.
    int64_t requested_from_msc;       ///< Original inclusive request start.
    int64_t requested_to_msc;         ///< Original inclusive request end.
    int64_t observed_from_msc;        ///< Earliest raw bar returned, or -1 when none.
    int64_t observed_to_msc;          ///< Latest raw bar returned, or -1 when none.
    uint32_t reserved[2];             ///< Reserved; must be zero.
} Mt5RateCoverageV1;

/// \struct Mt5RateBuffer
/// \brief Opaque DLL-owned collection of Mt5Rate values.
typedef struct Mt5RateBuffer Mt5RateBuffer;

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

/// \brief Copies additive range-coverage evidence for a rate buffer.
/// \param buffer Buffer returned by mt5bridge_query_rates().
/// \param[out] coverage Destination for the versioned coverage record.
/// \return Zero on success; non-zero when an argument is NULL or the extension is unavailable.
MT5BRIDGE_EXPORT int mt5bridge_rate_buffer_coverage_v1(const Mt5RateBuffer *buffer,
                                                       Mt5RateCoverageV1 *coverage);

#if defined(__cplusplus)
static_assert(sizeof(Mt5Rate) == 64, "Mt5Rate ABI size changed");
static_assert(offsetof(Mt5Rate, time) == 0, "Mt5Rate::time ABI offset changed");
static_assert(offsetof(Mt5Rate, tick_volume) == 40,
              "Mt5Rate::tick_volume ABI offset changed");
static_assert(offsetof(Mt5Rate, spread) == 48, "Mt5Rate::spread ABI offset changed");
static_assert(offsetof(Mt5Rate, real_volume) == 56,
              "Mt5Rate::real_volume ABI offset changed");
static_assert(sizeof(Mt5RatesRequest) == 32, "Mt5RatesRequest ABI size changed");
static_assert(offsetof(Mt5RatesRequest, timeframe) == 24,
              "Mt5RatesRequest::timeframe ABI offset changed");
static_assert(sizeof(Mt5RateCoverageV1) == 48, "Mt5RateCoverageV1 ABI size changed");
static_assert(offsetof(Mt5RateCoverageV1, requested_from_msc) == 8,
              "Mt5RateCoverageV1::requested_from_msc ABI offset changed");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(Mt5Rate) == 64, "Mt5Rate ABI size changed");
_Static_assert(offsetof(Mt5Rate, real_volume) == 56, "Mt5Rate ABI offsets changed");
_Static_assert(sizeof(Mt5RatesRequest) == 32, "Mt5RatesRequest ABI size changed");
_Static_assert(sizeof(Mt5RateCoverageV1) == 48, "Mt5RateCoverageV1 ABI size changed");
#endif

#ifdef __cplusplus
}
#endif
