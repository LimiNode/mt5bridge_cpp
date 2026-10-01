#pragma once

/// \file dispatch/lease.hpp
/// \brief Defines the account-scoped single-writer lease contract.

#include <mt5bridge/reconciliation/graph.hpp>

#include <cstdint>
#include <optional>

namespace mt5bridge {

/// \class SingleWriterLease
/// \brief Verifies continuous ownership for one account-scoped dispatch.
class SingleWriterLease {
public:
    virtual ~SingleWriterLease() = default;

    /// \brief Reads the held fencing token for the immutable operation account.
    /// \param account Account that the caller is about to dispatch.
    /// \return Non-zero token only while this owner holds the exclusive lease.
    /// \note Implementations must perform the ownership check and token read as
    /// one indivisible operation, or use a fencing protocol that makes the
    /// returned token authoritative for the following durable commit.
    virtual std::optional<std::uint64_t> held_fencing_token(
        const AccountKey &account) const = 0;
};

} // namespace mt5bridge
