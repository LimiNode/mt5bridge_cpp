#pragma once

/// \file dispatch/file_broker_allocation_store.hpp
/// \brief Defines the Windows-backed broker-allocation envelope store.

#include "broker_allocation_envelope.hpp"

#include <filesystem>
#include <string>

namespace mt5bridge {

/// \class WindowsFileBrokerAllocationStore
/// \brief Persists one immutable broker-allocation envelope per reversal.
///
/// Records use a dedicated namespace and lock. A duplicate identical link is
/// idempotent; a different envelope for the same broker reversal conflicts.
/// The store contains no ownership policy.
class WindowsFileBrokerAllocationStore final : public DurableBrokerAllocationStore {
public:
    /// \brief Opens or creates a broker-allocation directory.
    /// \param directory Directory containing allocation records and the lock file.
    explicit WindowsFileBrokerAllocationStore(std::filesystem::path directory);

    WindowsFileBrokerAllocationStore(const WindowsFileBrokerAllocationStore &) = delete;
    WindowsFileBrokerAllocationStore &operator=(
        const WindowsFileBrokerAllocationStore &) = delete;
    WindowsFileBrokerAllocationStore(WindowsFileBrokerAllocationStore &&) = delete;
    WindowsFileBrokerAllocationStore &operator=(WindowsFileBrokerAllocationStore &&) = delete;

    /// \brief Commits one immutable broker-allocation envelope atomically.
    /// \param record Complete broker allocation envelope.
    /// \param broker_store Durable source whose proof must authorize the link.
    /// \return Commit, replay, conflict, validation, or I/O status.
    BrokerAllocationCommitStatus commit(
        const BrokerAllocationEnvelope &record,
        const DurableBrokerReversalStore &broker_store) override;

    /// \brief Loads an envelope by its broker reversal identity.
    /// \param key Broker reversal key.
    /// \return Status-bearing result distinguishing absence from corruption/I/O.
    BrokerAllocationLoadResult load(const BrokerReversalKey &key) const override;

    /// \brief Enumerates every broker envelope for restart recovery.
    /// \return Complete records, or a failure with no usable partial set.
    BrokerAllocationScanResult scan() const override;

    /// \brief Tests whether the directory was opened successfully.
    /// \return True when operations can acquire the store lock.
    bool ready() const { return ready_; }

    /// \brief Returns the latest storage diagnostic.
    /// \return Stable text until the next store operation.
    const std::string &last_error() const { return last_error_; }

    /// \brief Returns the configured storage directory.
    /// \return Directory used for records and the lock file.
    const std::filesystem::path &directory() const { return directory_; }

private:
    std::filesystem::path directory_;
    bool ready_ = false;
    mutable std::string last_error_;
};

} // namespace mt5bridge
