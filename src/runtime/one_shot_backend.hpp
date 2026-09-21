#pragma once

/// \file one_shot_backend.hpp
/// \brief Defines the private one-shot dispatch execution seam.

#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <mt5bridge/dispatch_journal.hpp>

#include <cstdint>
#include <optional>
#include <vector>

namespace mt5bridge::runtime {

/// \brief Broker retcode for a closed trading session.
constexpr std::uint32_t kTradeRetcodeMarketClosed = 10018;

/// \enum BackendCallStatus
/// \brief Distinguishes a broker result from a transport/backend failure.
enum class BackendCallStatus {
    broker_result,      ///< The backend returned a complete raw trade result.
    transport_failure,  ///< No trustworthy broker result was returned.
};

/// \enum BrokerResultDisposition
/// \brief Classifies a persisted broker result for lifecycle progression.
enum class BrokerResultDisposition {
    accepted,       ///< Result is sufficient to enter `OperationState::accepted`.
    rejected,       ///< Broker deterministically rejected the operation.
    reconciling,    ///< Result requires observation-based outcome resolution.
};

/// \struct BackendCallResult
/// \brief Carries a backend result without exposing Python or MT5 objects.
struct BackendCallResult {
    BackendCallStatus status = BackendCallStatus::transport_failure;
    BrokerResultDisposition disposition = BrokerResultDisposition::reconciling;
    std::uint32_t retcode = 0; ///< MqlTradeResult.retcode when available.
    std::vector<std::uint8_t> raw_result; ///< Complete opaque result payload.
};

/// \class CurrentAccountProbe
/// \brief Reads the terminal account at a live execution boundary.
class CurrentAccountProbe {
public:
    virtual ~CurrentAccountProbe() = default;

    /// \brief Reads the currently selected terminal account.
    /// \return A valid account identity, or empty when it cannot be trusted.
    virtual std::optional<AccountKey> current_account() = 0;
};

/// \class DispatchTransport
/// \brief Private adapter seam for one synchronous broker submission.
class DispatchTransport {
public:
    virtual ~DispatchTransport() = default;

    /// \brief Performs exactly one backend call for an already-submitting record.
    /// \param record Durable operation record containing the opaque request payload.
    /// \return Broker result or a transport failure; the adapter must not retry.
    virtual BackendCallResult submit_once(const OperationRecord &record) = 0;
};

/// \enum OneShotExecutionStatus
/// \brief Reports the outcome of guarded one-shot backend execution.
enum class OneShotExecutionStatus {
    completed,                 ///< Result persisted and lifecycle state advanced.
    invalid_permit,            ///< Permit is malformed or belongs to another operation.
    operation_not_found,       ///< Owner loop does not contain the operation.
    stale_permit,              ///< Journal revision/token no longer match the permit.
    account_mismatch,          ///< Current account differs from immutable operation scope.
    lease_lost,                ///< Fencing lease is absent or changed before the call.
    transition_failed,         ///< Durable transition to `submitting` failed.
    transport_failure,         ///< Backend returned no trustworthy broker result.
    result_not_durable,         ///< Raw broker result could not be durably persisted.
    lifecycle_transition_failed, ///< Result persisted but final state transition failed.
};

/// \struct OneShotExecutionResult
/// \brief Returns a guarded execution outcome and the latest journal record.
struct OneShotExecutionResult {
    OneShotExecutionStatus status = OneShotExecutionStatus::invalid_permit;
    std::uint32_t retcode = 0;
    std::optional<OperationRecord> record;

    /// \brief Tests whether a broker result was durably recorded.
    /// \return True only when execution completed with a recovered record.
    bool completed() const { return status == OneShotExecutionStatus::completed; }
};

/// \class OneShotDispatchBackend
/// \brief Consumes one durable permit and invokes an injected backend once.
///
/// This class is private runtime infrastructure. It does not create a public
/// `order_send` API and never retries a transport call. A transport failure
/// leaves the operation unresolved after the durable `dispatching` barrier;
/// recovery must reconcile it rather than resend it.
class OneShotDispatchBackend {
public:
    /// \brief Revalidates ownership, enters `submitting`, and calls once.
    /// \param journal Owner-loop journal containing the admitted operation.
    /// \param key Immutable operation identity covered by the permit.
    /// \param permit Move-only admission evidence; passing it consumes the caller copy.
    /// \param account_probe Live account reader used at both execution boundaries.
    /// \param lease Continuously-held writer lease for the operation account.
    /// \param transport Private adapter that performs the single broker call.
    /// \return Guard outcome and the latest durable operation record when available.
    OneShotExecutionResult execute(OperationJournal &journal, const OperationKey &key,
                                   DispatchPermit permit,
                                   CurrentAccountProbe &account_probe,
                                   const SingleWriterLease &lease,
                                   DispatchTransport &transport) const;
};

} // namespace mt5bridge::runtime
