#pragma once

/// \file file_journal_store_test_support.hpp
/// \brief Declares the test-build-only observer for actual account-lock contention.

#include <mt5bridge/dispatch/file_journal_store.hpp>

#include <functional>

namespace mt5bridge {

/// \brief Observes contention without providing or replacing the lease mutex.
struct WindowsSingleWriterLeaseTestAccess {
    /// \brief Installs a notification after failed try_lock, before blocking lock.
    /// \param lease Real Windows lease used by the concurrency regression.
    /// \param hook Notification only; must not invoke lease or journal operations.
    /// \note Test builds only. Set/reset before starting or after joining workers.
    static void set_contention_hook(WindowsSingleWriterLease &lease,
                                    std::function<void()> hook);
};

} // namespace mt5bridge
