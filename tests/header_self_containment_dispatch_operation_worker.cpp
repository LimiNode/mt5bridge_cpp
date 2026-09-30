/// \file header_self_containment_dispatch_operation_worker.cpp
/// \brief Compiles the journal-aware operation worker header standalone.

#include <mt5bridge/dispatch/operation_worker.hpp>

extern "C" int mt5bridge_probe_dispatch_operation_worker_header() {
    using RecoveryAction = mt5bridge::OperationRecoveryAction;
    const auto classify = &mt5bridge::OperationRecoveryCoordinator::classify;
    (void)classify;
    return RecoveryAction::terminal == RecoveryAction::terminal ? 0 : 1;
}
