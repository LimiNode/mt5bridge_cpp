#pragma once

/// \file typed_managed_open_validation.hpp
/// \brief Checks supplied OPEN quantities and policy without granting authority.

#include <mt5bridge/dispatch/broker_reversal_reconciliation.hpp>
#include <mt5bridge/reconciliation/graph.hpp>
#include <mt5bridge/trade/symbol.h>

#include <cstdint>
#include <optional>
#include <string>

namespace mt5bridge::dispatch {

/// \brief The only order sides representable by the narrow OPEN intent.
enum class ManagedOpenSide { unspecified = -1, buy = 0, sell = 1 };

/// \brief Explicit market-execution policies; values are ORDER_FILLING enums.
enum class ManagedOpenFilling { unspecified = -1, fok = 0, ioc = 1 };

/// \brief Desired whole-lot OPEN, not an authenticated or dispatchable request.
struct TypedManagedOpenIntent {
    AccountKey account; ///< Desired server/login scope.
    std::string symbol; ///< Exact, bounded UTF-8 symbol identity.
    ManagedOpenSide side = ManagedOpenSide::unspecified; ///< Required direction.
    std::uint64_t logical_volume = 0; ///< Legacy units: one unit is one lot.
    ManagedOpenFilling filling = ManagedOpenFilling::unspecified; ///< No default.
};

/// \brief Unauthenticated inputs used only for pure consistency checks.
/// \warning Every field can be caller-authored. This is not a runtime snapshot,
/// ownership proof, evidence of a pure opening, or a fresh environment proof.
struct SuppliedManagedOpenMetadata {
    Mt5AccountInfo account{}; ///< Supplied account identity and permissions.
    AccountKey symbol_account; ///< Claimed account scope of symbol metadata.
    Mt5SymbolCapabilities symbol{}; ///< Supplied symbol policy and limits.
    BrokerVolumeNormalization normalization; ///< Explicit decimal step domain.
    std::optional<double> directional_volume; ///< Claimed complete same-side
        ///< positions/orders aggregate in lots; required for a positive limit.
};

/// \brief Pure-validation outcomes, none of which authorizes preparation/send.
enum class ManagedOpenValidationStatus {
    candidate,                 ///< Consistent supplied values only.
    invalid_intent,            ///< Invalid identity, side, filling or zero units.
    account_mismatch,          ///< Missing or inconsistent account identities.
    permission_denied,         ///< Missing or denied account trading permission.
    invalid_symbol,            ///< Missing/mismatched/badly encoded symbol.
    unsupported_policy,        ///< Unknown/missing or unsupported symbol policy.
    invalid_quantity,          ///< Invalid domain, bounds, alignment or precision.
    incomplete_directional_volume, ///< Positive limit lacks a supplied aggregate.
    volume_limit_exceeded,     ///< Request plus supplied aggregate exceeds limit.
};

/// \brief Copyable numeric candidate, deliberately not sealed or authoritative.
/// \note Editing or constructing this value cannot grant any capability. It has
/// no request JSON, journal binding, DispatchPermit or implicit OPEN admission.
struct ManagedOpenCandidate {
    TypedManagedOpenIntent intent; ///< Retained desired values, not source proof.
    BrokerVolume broker_volume; ///< Canonical whole-lot volume/step conversion.
};

/// \brief Returns either a numeric candidate or a precise validation failure.
struct ManagedOpenValidationResult {
    ManagedOpenValidationStatus status; ///< Pure-validation decision.
    std::optional<ManagedOpenCandidate> candidate; ///< Present only on success.
};

/// \brief Validates supplied values for market-execution BUY/SELL FOK/IOC only.
/// \param intent Desired whole-lot quantity and explicit policy.
/// \param supplied Unauthenticated account/symbol/aggregate values.
/// \return Consistent candidate or rejection; no durable or broker side effects.
/// \warning Does not check lease/freshness, netting exposure, sessions, source
/// authority or Python decoding. No production component consumes the result.
ManagedOpenValidationResult validate_typed_managed_open(
    const TypedManagedOpenIntent &intent,
    const SuppliedManagedOpenMetadata &supplied);

} // namespace mt5bridge::dispatch
