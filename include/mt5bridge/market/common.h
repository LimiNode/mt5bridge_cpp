#pragma once

/// \file market/common.h
/// \brief Declares diagnostics shared by market-data operations.

#include <stddef.h>
#include <stdint.h>

#include <mt5bridge/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

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
/// \brief A stable observation was returned but range completeness is unproven.
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

/// \brief Retrieves diagnostics for the most recent market-data call on this thread.
/// \param[out] diagnostics Destination for the diagnostic snapshot.
/// \return Zero on success; non-zero when \p diagnostics is NULL.
/// \note This accessor is useful when a query fails before returning a buffer.
MT5BRIDGE_EXPORT int mt5bridge_last_fetch_diagnostics(Mt5FetchDiagnostics *diagnostics);

#if defined(__cplusplus)
static_assert(sizeof(Mt5FetchStatus) == 4, "Mt5FetchStatus ABI size changed");
static_assert(sizeof(Mt5FetchDiagnostics) == 24,
              "Mt5FetchDiagnostics ABI size changed");
static_assert(offsetof(Mt5FetchDiagnostics, status) == 20,
              "Mt5FetchDiagnostics::status ABI offset changed");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(Mt5FetchStatus) == 4, "Mt5FetchStatus ABI size changed");
_Static_assert(sizeof(Mt5FetchDiagnostics) == 24,
               "Mt5FetchDiagnostics ABI size changed");
#endif

#ifdef __cplusplus
}
#endif
