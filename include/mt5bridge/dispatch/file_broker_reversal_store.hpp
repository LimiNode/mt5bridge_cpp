#pragma once

/// \file dispatch/file_broker_reversal_store.hpp
/// \brief Defines the Windows-backed broker reversal store.

#include "broker_reversal.hpp"

#include <filesystem>
#include <string>

namespace mt5bridge {

/// \class WindowsFileBrokerReversalStore
/// \brief Persists immutable broker-level reversal proofs as atomic files.
///
/// The store uses a dedicated lock and record namespace so broker decomposition
/// remains independent from managed operation records. A duplicate identical
/// record is idempotent; a different decomposition for the same deal conflicts.
class WindowsFileBrokerReversalStore final : public DurableBrokerReversalStore {
public:
    /// \brief Opens or creates a broker-reversal directory.
    /// \param directory Directory containing reversal records and the lock file.
    explicit WindowsFileBrokerReversalStore(std::filesystem::path directory);

    WindowsFileBrokerReversalStore(const WindowsFileBrokerReversalStore &) = delete;
    WindowsFileBrokerReversalStore &operator=(const WindowsFileBrokerReversalStore &) = delete;
    WindowsFileBrokerReversalStore(WindowsFileBrokerReversalStore &&) = delete;
    WindowsFileBrokerReversalStore &operator=(WindowsFileBrokerReversalStore &&) = delete;

    /// \brief Commits one immutable broker-level reversal durably.
    /// \param record Complete broker decomposition and provenance proof.
    /// \return Commit, idempotent replay, conflict, validation, or I/O status.
    BrokerReversalCommitStatus commit(
        const BrokerReversalRecord &record) override;

    /// \brief Loads one broker reversal after validating its envelope.
    /// \param key Account-scoped deal identity.
    /// \return Status-bearing result distinguishing absence from corruption/I/O.
    BrokerReversalLoadResult load(const BrokerReversalKey &key) const override;

    /// \brief Enumerates every broker reversal for restart recovery.
    /// \return Complete records, or a failure with no usable partial set.
    BrokerReversalScanResult scan() const override;

    /// \brief Tests whether the directory is available.
    /// \return True when store operations can acquire the lock.
    bool ready() const { return ready_; }

    /// \brief Returns the latest diagnostic captured by this store.
    /// \return Stable diagnostic text until the next store operation.
    const std::string &last_error() const { return last_error_; }

    /// \brief Returns the configured storage directory.
    /// \return Directory used for broker reversal records.
    const std::filesystem::path &directory() const { return directory_; }

private:
    std::filesystem::path directory_;
    bool ready_ = false;
    mutable std::string last_error_;
};

} // namespace mt5bridge
