/// \file private_trade_manager_concurrency_test.cpp
/// \brief Exercises real Windows account capabilities and competing OPEN managers.

#include "private_trade_manager.hpp"
#include "file_journal_store_test_support.hpp"

#include <mt5bridge/dispatch/file_journal_store.hpp>

#include "support/fake_observation_provider.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

/// \brief Fails the regression even in Release builds.
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

/// \brief Returns one immutable account scope, optionally on another login.
mt5bridge::AccountKey account(std::uint64_t login = 77) {
    return {"Concurrency-Server", login};
}

/// \class Signal
/// \brief Provides bounded event synchronization without scheduling sleeps.
class Signal final {
public:
    /// \brief Releases every waiter.
    void set() {
        std::lock_guard<std::mutex> lock(mutex_);
        set_ = true;
        changed_.notify_all();
    }

    /// \brief Waits for an explicit event or fails within the CTest deadline.
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        require(changed_.wait_for(lock, std::chrono::seconds(5),
                                  [this] { return set_; }),
                "concurrency synchronization event was not reached");
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool set_ = false;
};

/// \class LockAttempt
/// \brief Distinguishes actual production-lock contention from callback entry.
class LockAttempt final {
public:
    /// \brief Records the production mutex's failed try_lock, before blocking.
    void contended() { record(true); }

    /// \brief Records callback entry, which must follow a contention event.
    void entered() { record(false); }

    /// \brief Returns the first observed event after the contender runs.
    bool wait_for_contention() {
        changed_.wait();
        std::lock_guard<std::mutex> lock(mutex_);
        return *first_was_contention_;
    }

private:
    void record(bool contention) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!first_was_contention_) {
            first_was_contention_ = contention;
            changed_.set();
        }
    }

    std::mutex mutex_;
    Signal changed_;
    std::optional<bool> first_was_contention_;
};

/// \class TestRoot
/// \brief Owns one uniquely created temporary journal directory.
class TestRoot final {
public:
    TestRoot() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned int attempt = 0; attempt != 100; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    ("mt5bridge-manager-concurrency-" + std::to_string(stamp) + "-" +
                     std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directories(path_, error) && !error)
                return;
        }
        throw std::runtime_error("could not create concurrency test directory");
    }

    ~TestRoot() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    /// \brief Returns the uniquely owned directory.
    const std::filesystem::path &path() const { return path_; }

private:
    std::filesystem::path path_;
};

/// \brief Creates complete empty observations sufficient for dispatch admission.
mt5bridge::ObservationBatch observation_batch(const mt5bridge::AccountKey &key) {
    mt5bridge::ObservationBatch batch;
    batch.account = key;
    batch.observed_domains = mt5bridge::ObservationDomain::active_orders |
                             mt5bridge::ObservationDomain::positions;
    return batch;
}

/// \class ControlledStore
/// \brief Delegates every durable operation to a real Windows store.
/// \note The optional scan callback runs after the store released its file lock.
class ControlledStore final : public mt5bridge::DurableJournalStore {
public:
    explicit ControlledStore(const std::filesystem::path &path) : durable(path) {
        require(durable.ready(), "Windows journal store was not ready");
    }

    mt5bridge::StoreCommitStatus commit(
        const mt5bridge::OperationRecord &record,
        std::optional<std::uint64_t> expected_revision) override {
        return durable.commit(record, expected_revision);
    }

    mt5bridge::StoreLoadResult load(const mt5bridge::OperationKey &key) const override {
        return durable.load(key);
    }

    mt5bridge::StoreScanResult scan() const override {
        auto result = durable.scan();
        if (after_scan) {
            auto callback = std::move(after_scan);
            after_scan = {};
            callback();
        }
        return result;
    }

    mt5bridge::WindowsFileJournalStore durable;
    mutable std::function<void()> after_scan;
};

/// \class AccountProbe
/// \brief Keeps broker account probing deterministic and local.
class AccountProbe final : public mt5bridge::runtime::CurrentAccountProbe {
public:
    explicit AccountProbe(mt5bridge::AccountKey owned) : owned_(std::move(owned)) {}

    std::optional<mt5bridge::AccountKey> current_account() override { return owned_; }

private:
    mt5bridge::AccountKey owned_;
};

/// \class Transport
/// \brief Counts fake broker calls without contacting a terminal.
class Transport final : public mt5bridge::runtime::DispatchTransport {
public:
    mt5bridge::runtime::BackendCallResult submit_once(
        const mt5bridge::OperationRecord &) override {
        ++calls;
        return result;
    }

    mt5bridge::runtime::BackendCallResult result{
        mt5bridge::runtime::BackendCallStatus::transport_failure,
        mt5bridge::runtime::BrokerResultDisposition::reconciling, 0, {}};
    std::size_t calls = 0;
};

/// \struct ManagerFixture
/// \brief Opens independent journals and observations for one private manager.
struct ManagerFixture {
    ManagerFixture(const std::filesystem::path &path,
                   const mt5bridge::WindowsSingleWriterLease &lease,
                   std::uint64_t trade_id)
        : store(path), journal(store), provider({observation_batch(lease.account()),
                                               observation_batch(lease.account())}),
          coordinator(provider, lease.account()), admission(journal, coordinator.graph(), {}),
          probe(lease.account()),
          manager(make_state(trade_id), lease.account(), journal, admission, backend,
                  probe, lease, transport, coordinator, collection) {
        const auto first = coordinator.refresh(collection);
        const auto second = coordinator.refresh(collection);
        require(first.sample && second.sample, "fixture observations were not accepted");
        const auto consistency = mt5bridge::EnvironmentConsistencyPolicy::evaluate(
            {*first.sample, *second.sample}, {});
        require(consistency.consistent(), "fixture did not establish a fresh proof");
        request.current_account = lease.account();
        request.environment_proof = consistency.proof;
        require(manager.start().status ==
                    mt5bridge::dispatch::PrivateTradeManagerStatus::ready,
                "private manager startup failed");
    }

    /// \brief Creates the initially empty, bounded logical managed state.
    static mt5bridge::managed_trade::ManagedTradeState make_state(std::uint64_t trade_id) {
        mt5bridge::managed_trade::ManagedTradeState state;
        require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                    &state, mt5bridge::managed_trade::TradeId{trade_id}, 1, 1, 2),
                "fixture state initialization failed");
        return state;
    }

    /// \brief Creates a valid retained request and observation descriptor.
    mt5bridge::dispatch::ManagedTradeIntent intent() {
        return {{'{', '}'},
                {request.current_account, coordinator.capture_baseline(),
                 {mt5bridge::require_active_order(7001)},
                 mt5bridge::OperationState::filled, manager.state()->trade_id.value,
                 1, 1}};
    }

    ControlledStore store;
    mt5bridge::OperationJournal journal;
    mt5bridge_test_support::FakeObservationProvider provider;
    mt5bridge::ObservationCoordinator coordinator;
    mt5bridge::DispatchAdmissionBarrier admission;
    mt5bridge::runtime::OneShotDispatchBackend backend;
    AccountProbe probe;
    Transport transport;
    mt5bridge::ObservationCollectionRequest collection;
    mt5bridge::dispatch::PrivateTradeManager manager;
    mt5bridge::DispatchAdmissionRequest request;
};

/// \brief Proves real Windows callback exclusion and independent-account progress.
void test_real_capability_exclusion_and_account_scope() {
    TestRoot root;
    mt5bridge::WindowsSingleWriterLease lease(root.path(), account());
    mt5bridge::WindowsSingleWriterLease other(root.path(), account(78));
    const auto token = lease.held_fencing_token(account());
    const auto other_token = other.held_fencing_token(account(78));
    require(token && other_token, "Windows account leases were not ready");

    Signal holding;
    Signal release;
    LockAttempt attempt;
    std::atomic<bool> first_completed = false;
    // The hook reports actual production mutex contention, not thread startup.
    mt5bridge::WindowsSingleWriterLeaseTestAccess::set_contention_hook(
        lease, [&] { attempt.contended(); });
    auto first = std::async(std::launch::async, [&] {
        return lease.with_fencing_capability(account(), *token, [&] {
            holding.set();
            release.wait();
            first_completed = true;
        });
    });
    holding.wait();
    auto second = std::async(std::launch::async, [&] {
        return lease.with_fencing_capability(account(), *token, [&] {
            attempt.entered();
            require(first_completed.load(), "real Windows capability callbacks overlapped");
        });
    });
    const bool contended = attempt.wait_for_contention();
    // A second account must complete while the first account is still held.
    auto independent = std::async(std::launch::async, [&] {
        return other.with_fencing_capability(account(78), *other_token, [] {});
    });
    const bool independent_ready = independent.wait_for(std::chrono::seconds(5)) ==
                                   std::future_status::ready;
    release.set();
    require(first.get() && second.get() && independent.get(), "capability callback failed");
    mt5bridge::WindowsSingleWriterLeaseTestAccess::set_contention_hook(lease, {});
    require(contended, "contender entered without contending on the real Windows mutex");
    require(independent_ready, "different accounts shared one capability mutex");
}

/// \brief Exercises a competing prepare while real dispatch eligibility is paused.
void test_dispatch_and_competing_prepare_are_serialized() {
    TestRoot root;
    const mt5bridge::OperationKey first_key{account(), 7, 1};
    const mt5bridge::OperationKey second_key{account(), 8, 1};
    {
        mt5bridge::WindowsSingleWriterLease lease(root.path(), account());
        require(lease.ready(), "Windows dispatch lease was not ready");
        ManagerFixture first(root.path(), lease, 7);
        ManagerFixture second(root.path(), lease, 8);
        const auto prepared = first.manager.prepare_open(1, first.intent());
        require(prepared.owner && prepared.owner->status ==
                    mt5bridge::dispatch::OwnerStepStatus::prepared,
                "first OPEN was not durably prepared");

        Signal eligibility_scanned;
        Signal release_dispatch;
        LockAttempt attempt;
        first.store.after_scan = [&] {
            eligibility_scanned.set();
            release_dispatch.wait();
        };
        second.store.after_scan = [&] { attempt.entered(); };
        mt5bridge::WindowsSingleWriterLeaseTestAccess::set_contention_hook(
            lease, [&] { attempt.contended(); });
        auto dispatch = std::async(std::launch::async, [&] {
            return first.manager.execute_pending(first.request);
        });
        eligibility_scanned.wait();
        auto competing_prepare = std::async(std::launch::async, [&] {
            return second.manager.prepare_open(1, second.intent());
        });
        const bool contended = attempt.wait_for_contention();
        release_dispatch.set();
        const auto dispatched = dispatch.get();
        const auto competing = competing_prepare.get();
        mt5bridge::WindowsSingleWriterLeaseTestAccess::set_contention_hook(lease, {});
        require(contended, "competing preparation bypassed the real dispatch capability");
        require(dispatched.owner && dispatched.owner->status ==
                    mt5bridge::dispatch::OwnerStepStatus::ambiguous &&
                    first.transport.calls == 1,
                "dispatch did not cross the durable barrier exactly once");
        require(competing.status == mt5bridge::dispatch::PrivateTradeManagerStatus::blocked &&
                    !competing.owner && second.transport.calls == 0 &&
                    second.store.durable.load(second_key).status ==
                        mt5bridge::StoreLoadStatus::not_found,
                "competing OPEN escaped the account-wide unresolved blocker");
        require(first.manager.execute_pending(first.request).status ==
                    mt5bridge::dispatch::PrivateTradeManagerStatus::blocked &&
                    second.manager.execute_pending(second.request).status ==
                        mt5bridge::dispatch::PrivateTradeManagerStatus::blocked &&
                    first.transport.calls == 1 && second.transport.calls == 0,
                "blocked OPEN acquired a resend permission");
    }

    // Destroy every original store, journal, manager and lease before reopening.
    mt5bridge::WindowsSingleWriterLease reopened_lease(root.path(), account());
    ManagerFixture recovered(root.path(), reopened_lease, 7);
    ManagerFixture competing(root.path(), reopened_lease, 8);
    const auto durable = recovered.store.durable.load(first_key);
    require(durable.record && durable.record->journal_state == mt5bridge::JournalState::reconciling &&
                recovered.manager.execute_pending(recovered.request).status ==
                    mt5bridge::dispatch::PrivateTradeManagerStatus::blocked &&
                competing.manager.prepare_open(1, competing.intent()).status ==
                    mt5bridge::dispatch::PrivateTradeManagerStatus::blocked &&
                recovered.transport.calls == 0 && competing.transport.calls == 0,
            "reopened Windows journal lost its unresolved non-resendable blocker");
}

/// \brief Keeps a terminal broker rejection non-resendable after reopening stores.
void test_rejected_open_cannot_be_resent() {
    TestRoot root;
    const mt5bridge::OperationKey key{account(), 7, 1};
    {
        mt5bridge::WindowsSingleWriterLease lease(root.path(), account());
        ManagerFixture fixture(root.path(), lease, 7);
        fixture.transport.result = {
            mt5bridge::runtime::BackendCallStatus::broker_result,
            mt5bridge::runtime::BrokerResultDisposition::rejected, 10018, {0xA0}};
        require(fixture.manager.prepare_open(1, fixture.intent()).owner.has_value(),
                "rejection fixture could not prepare OPEN");
        const auto rejected = fixture.manager.execute_pending(fixture.request);
        require(rejected.owner && rejected.owner->status ==
                    mt5bridge::dispatch::OwnerStepStatus::completed &&
                    rejected.owner->record && rejected.owner->record->operation_state ==
                        mt5bridge::OperationState::rejected &&
                    fixture.manager.execute_pending(fixture.request).status ==
                        mt5bridge::dispatch::PrivateTradeManagerStatus::blocked &&
                    fixture.transport.calls == 1,
                "broker rejection gained a second dispatch permission");
    }
    mt5bridge::WindowsFileJournalStore store(root.path());
    mt5bridge::OperationJournal journal(store);
    mt5bridge::WindowsSingleWriterLease lease(root.path(), account());
    mt5bridge::dispatch::PrivateOwnerLoop loop(journal, account(), lease);
    require(loop.start().ready() && loop.step().status ==
                mt5bridge::dispatch::PrivateOwnerLoopStepStatus::idle,
            "reopened broker rejection was not terminal");
    mt5bridge::ObservationGraph graph;
    mt5bridge::DispatchAdmissionBarrier barrier(journal, graph, {});
    mt5bridge::DispatchAdmissionRequest request;
    request.current_account = account();
    const auto admitted = barrier.admit(key, request, lease);
    require(admitted.status == mt5bridge::DispatchAdmissionStatus::invalid_state &&
                !admitted.permit,
            "reopened broker rejection regained a dispatch permit");
}

/// \brief Keeps nested durable publication ordered and fenced on one thread.
void test_nested_capability_preserves_durable_owner_state() {
    TestRoot root;
    mt5bridge::WindowsSingleWriterLease lease(root.path(), account());
    mt5bridge::WindowsFileJournalStore store(root.path());
    mt5bridge::OperationJournal journal(store);
    const mt5bridge::OperationKey key{account(), 7, 1};
    const auto token = lease.held_fencing_token(account());
    require(token && journal.create(key, {'{', '}'}, mt5bridge::OperationKind::open).accepted(),
            "nested publication fixture was not durable");
    bool invalid_callback_called = false;
    require(lease.with_fencing_capability(account(), *token, [&] {
        require(lease.with_fencing_capability(account(), *token, [&] {
            require(journal.transition_operation(key, mt5bridge::OperationState::prechecking)
                        .accepted(),
                    "nested publication violated the owner lifecycle");
        }), "same-thread nested publication lost the account capability");
        const auto before_invalid = journal.find(key);
        const auto invalid_callback = [&] { invalid_callback_called = true; };
        require(!lease.with_fencing_capability(account(78), *token, invalid_callback) &&
                    !lease.with_fencing_capability(account(), *token + 1, invalid_callback),
                "nested publication accepted a foreign account or mismatched generation");
        const auto after_invalid = journal.find(key);
        require(before_invalid && after_invalid &&
                    before_invalid->revision == after_invalid->revision &&
                    journal.transition_journal(key, mt5bridge::JournalState::prechecked)
                        .accepted(),
                "nested rejection mutated the durable owner state");
    }), "outer publication lost the account capability");
    mt5bridge::WindowsFileJournalStore reopened(root.path());
    const auto durable = reopened.load(key);
    require(!invalid_callback_called && durable.record && durable.record->revision == 3 &&
                durable.record->operation_state == mt5bridge::OperationState::prechecking &&
                durable.record->journal_state == mt5bridge::JournalState::prechecked,
            "nested publication did not preserve exact durable lifecycle ordering");
}

} // namespace

int main() {
    try {
        test_real_capability_exclusion_and_account_scope();
        test_dispatch_and_competing_prepare_are_serialized();
        test_rejected_open_cannot_be_resent();
        test_nested_capability_preserves_durable_owner_state();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
