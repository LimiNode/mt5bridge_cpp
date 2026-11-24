#pragma once

/// \file managed_open_capability_collector.hpp
/// \brief Defines private runtime-owned capability collection, not OPEN admission.

#include <mt5bridge/dispatch/broker_reversal_reconciliation.hpp>
#include <mt5bridge/dispatch/file_journal_store.hpp>
#include <mt5bridge/trade/symbol.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

struct _object;

/// \brief Exports private C++ entry points only in repository test builds.
/// \note Not installed, not part of the public ABI, absent with BUILD_TESTING=OFF.
#if defined(MT5BRIDGE_INTERNAL_TESTING)
#define MT5BRIDGE_COLLECTOR_TEST_API MT5BRIDGE_EXPORT
#else
#define MT5BRIDGE_COLLECTOR_TEST_API
#endif

namespace mt5bridge::trade {

/// \brief Fail-closed collection outcomes; collected never means permission to send.
enum class ManagedOpenCollectionStatus {
    collected,
    invalid_request,
    lease_lost,
    runtime_unavailable,
    python_error,
    account_mismatch,
    permission_denied,
    incomplete_observation,
    invalid_capabilities,
    invalid_volume,
    unstable_observation,
    stale_observation,
};

/// \brief Decoded facts; this plain data alone is deliberately not source evidence.
struct ManagedOpenCapabilityData {
    Mt5AccountInfo account{}; ///< Strictly decoded account identity/permissions.
    Mt5SymbolCapabilities symbol{}; ///< Reported modes and limits, not selected policy.
    BrokerVolumeNormalization normalization; ///< Derived from runtime symbol step.
    std::uint64_t minimum_units = 0; ///< Exact positive minimum broker numerator.
    std::uint64_t maximum_units = 0; ///< Exact maximum broker numerator.
    std::uint64_t directional_limit_units = 0; ///< Zero means reported no numeric cap.
    std::uint64_t buy_exposure_units = 0; ///< Positions plus active BUY-side remainder.
    std::uint64_t sell_exposure_units = 0; ///< Positions plus active SELL-side remainder.
    std::size_t account_position_count = 0; ///< Complete unfiltered collection size.
    std::size_t account_order_count = 0; ///< Complete unfiltered collection size.
    std::size_t symbol_position_count = 0; ///< Positions in the requested symbol.
    std::size_t symbol_order_count = 0; ///< Active orders in the requested symbol.
};

struct ManagedOpenCollectionResult;
class ManagedOpenCapabilitySnapshot;

/// \brief Collects from the bridge's running MT5 module under its trade-critical lane.
/// \param expected Account that must match the live runtime and concrete lease.
/// \param symbol Exact bounded UTF-8 identity; no alias/truncation is accepted.
/// \param lease Actual Windows account lease, held across collection.
/// \return A runtime-created snapshot or explicit failure, without preparation/send.
/// \warning Call before taking the runtime lane/GIL. No caller-supplied module,
/// POD, normalization, clock, ready-made token or source callback is accepted.
MT5BRIDGE_COLLECTOR_TEST_API ManagedOpenCollectionResult collect_managed_open_capabilities(
    const AccountKey &expected, const std::string &symbol,
    const WindowsSingleWriterLease &lease);

/// \brief Checks local age, runtime lifecycle and fencing scope of retained facts.
/// \param snapshot Runtime-created observation, not caller-authored data.
/// \param lease Actual account lease that must retain the captured epoch.
/// \return False after age expiry, shutdown/reinitialize or ownership transfer.
/// \warning This is not a fresh broker observation, netting/exposure authority or
/// dispatch permit. Future pre-barrier admission must recollect live capabilities.
MT5BRIDGE_COLLECTOR_TEST_API bool managed_open_capabilities_current(
    const ManagedOpenCapabilitySnapshot &snapshot, const WindowsSingleWriterLease &lease);

/// \brief Immutable process-local facts constructible only by runtime collection.
class ManagedOpenCapabilitySnapshot {
public:
    /// \brief Returns immutable collected values; copying them does not copy authority.
    const ManagedOpenCapabilityData &data() const { return data_; }
    /// \brief Returns the account fencing epoch retained during collection.
    std::uint64_t fencing_token() const { return token_; }
    /// \brief Returns the bridge lifecycle generation that performed collection.
    std::uint64_t runtime_generation() const { return generation_; }
    /// \brief Conservatively blocks a new unencumbered OPEN on any symbol exposure.
    /// \note No ownership is inferred from magic, margin mode or position direction.
    /// False reports observed empty sets only; it never grants pure-OPEN admission
    /// or excludes external trading between observations and a later dispatch.
    bool blocks_unencumbered_open() const {
        return data_.symbol_position_count != 0 || data_.symbol_order_count != 0;
    }

private:
    ManagedOpenCapabilitySnapshot(ManagedOpenCapabilityData data, std::uint64_t token,
                                 std::uint64_t generation,
                                 std::chrono::steady_clock::time_point captured)
        : data_(std::move(data)), token_(token), generation_(generation), captured_(captured) {}
    ManagedOpenCapabilityData data_;
    std::uint64_t token_;
    std::uint64_t generation_;
    std::chrono::steady_clock::time_point captured_;
    friend MT5BRIDGE_COLLECTOR_TEST_API ManagedOpenCollectionResult collect_managed_open_capabilities(
        const AccountKey &, const std::string &, const WindowsSingleWriterLease &);
    friend MT5BRIDGE_COLLECTOR_TEST_API bool managed_open_capabilities_current(
        const ManagedOpenCapabilitySnapshot &, const WindowsSingleWriterLease &);
};

/// \brief A source snapshot, not a validator candidate, journal binding or permit.
struct ManagedOpenCollectionResult {
    ManagedOpenCollectionStatus status; ///< Explicit collection decision.
    std::optional<ManagedOpenCapabilitySnapshot> snapshot; ///< Present only when collected.
};

namespace detail {

/// \brief Plain decoder output; it cannot construct the sealed runtime snapshot.
struct ManagedOpenReadResult {
    ManagedOpenCollectionStatus status; ///< Strict decoder decision.
    std::optional<ManagedOpenCapabilityData> data; ///< Plain decoded values only.
};

/// \brief Reads bracketed runtime facts; caller must own admission and the GIL.
/// \param mt5 Borrowed runtime-owned module, never an argument of the collector API.
/// \param expected Account identity checked before/after all observations.
/// \param symbol Exact requested symbol.
/// \return Plain data for the runtime wrapper; not authenticated by this helper alone.
ManagedOpenReadResult read_managed_open_capabilities(
    _object *mt5, const AccountKey &expected, const std::string &symbol);

} // namespace detail
} // namespace mt5bridge::trade

#undef MT5BRIDGE_COLLECTOR_TEST_API
