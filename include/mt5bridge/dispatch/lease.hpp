#pragma once

/// \file dispatch/lease.hpp
/// \brief Defines the account-scoped single-writer lease contract.

#include <mt5bridge/reconciliation/graph.hpp>

#include <cstdint>
#include <functional>
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

    /// \brief Runs one publication while the fencing capability remains authoritative.
    /// \param account Account identity being published.
    /// \param token Fencing token previously acquired for the account.
    /// \param publication Atomic durable publication callback.
    /// \return False when the capability cannot be held for the callback.
    /// \note Allocation stores must use this seam for their final publication;
    /// a token read followed by an unrelated write is not an authoritative fence.
    virtual bool with_fencing_capability(
        const AccountKey &account, std::uint64_t token,
        const std::function<void()> &publication) const {
        (void)account;
        (void)token;
        (void)publication;
        return false;
    }
};

} // namespace mt5bridge
