/// \file private_trade_manager_concurrency_test.cpp
/// \brief Verifies account-scoped preparation/dispatch serialization.

#include "private_trade_manager.hpp"

#include <mt5bridge/dispatch/file_journal_store.hpp>

#include "support/fake_observation_provider.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

mt5bridge::AccountKey account() { return {"Concurrency-Server", 77}; }

mt5bridge::ObservationBatch observation_batch(
    const mt5bridge::AccountKey &key, std::uint64_t ticket) {
    mt5bridge::ObservationBatch batch;
    batch.account = key;
    batch.observed_domains = mt5bridge::ObservationDomain::active_orders |
                             mt5bridge::ObservationDomain::positions;
    Mt5OrderSnapshot order{};
    order.ticket = ticket;
    order.known_fields = MT5BRIDGE_ORDER_KNOWN_TICKET |
                         MT5BRIDGE_ORDER_KNOWN_POSITION_ID;
    batch.active_orders.push_back(order);
    return batch;
}

class SharedRecords final {
public:
    std::map<mt5bridge::OperationKey, mt5bridge::OperationRecord> records;
    mutable std::mutex mutex;
};

class SharedStore final : public mt5bridge::DurableJournalStore {
public:
    explicit SharedStore(std::shared_ptr<SharedRecords> records)
        : records_(std::move(records)) {}

    mt5bridge::StoreCommitStatus commit(
        const mt5bridge::OperationRecord &record,
        std::optional<std::uint64_t> expected_revision) override {
        std::lock_guard<std::mutex> guard(records_->mutex);
        const auto it = records_->records.find(record.key);
        if (expected_revision) {
            if (it == records_->records.end() ||
                it->second.revision != *expected_revision)
                return mt5bridge::StoreCommitStatus::conflict;
        } else if (it != records_->records.end()) {
            return mt5bridge::StoreCommitStatus::conflict;
        }
        records_->records[record.key] = record;
        return mt5bridge::StoreCommitStatus::committed;
    }

    mt5bridge::StoreLoadResult load(
        const mt5bridge::OperationKey &key) const override {
        std::lock_guard<std::mutex> guard(records_->mutex);
        const auto it = records_->records.find(key);
        if (it == records_->records.end())
            return {mt5bridge::StoreLoadStatus::not_found, std::nullopt};
        return {mt5bridge::StoreLoadStatus::found, it->second};
    }

    mt5bridge::StoreScanResult scan() const override {
        std::lock_guard<std::mutex> guard(records_->mutex);
        mt5bridge::StoreScanResult result;
        result.status = mt5bridge::StoreScanStatus::complete;
        for (const auto &entry : records_->records)
            result.records.push_back(entry.second);
        return result;
    }

private:
    std::shared_ptr<SharedRecords> records_;
};

class SerializedLease final : public mt5bridge::SingleWriterLease {
public:
    explicit SerializedLease(mt5bridge::AccountKey owned)
        : owned_(std::move(owned)) {}

    std::optional<std::uint64_t> held_fencing_token(
        const mt5bridge::AccountKey &requested) const override {
        if (requested != owned_)
            return std::nullopt;
        return 77;
    }

    bool with_fencing_capability(
        const mt5bridge::AccountKey &requested, std::uint64_t token,
        const std::function<void()> &publication) const override {
        std::unique_lock<std::mutex> lock(mutex_);
        if (requested != owned_ || token != 77 || !publication)
            return false;
        capability_calls_.fetch_add(1, std::memory_order_relaxed);
        if (pause_first_ && !first_entered_) {
            first_entered_.store(true, std::memory_order_release);
            first_entered_signal_.notify_all();
            while (!release_first_.load(std::memory_order_acquire))
                std::this_thread::yield();
        } else if (first_entered_) {
            second_entered_.store(true, std::memory_order_release);
        }
        publication();
        return true;
    }

    void pause_first_callback() {
        std::lock_guard<std::mutex> guard(mutex_);
        pause_first_ = true;
    }

    void wait_first_entered() const {
        std::unique_lock<std::mutex> lock(signal_mutex_);
        first_entered_signal_.wait(lock, [this] {
            return first_entered_.load(std::memory_order_acquire);
        });
    }

    void release_first() {
        release_first_.store(true, std::memory_order_release);
    }

    bool second_entered() const {
        return second_entered_.load(std::memory_order_acquire);
    }

private:
    mt5bridge::AccountKey owned_;
    mutable std::mutex mutex_;
    mutable std::mutex signal_mutex_;
    mutable std::condition_variable first_entered_signal_;
    mutable bool pause_first_ = false;
    mutable std::atomic<bool> first_entered_ = false;
    mutable std::atomic<bool> release_first_ = false;
    mutable std::atomic<bool> second_entered_ = false;
    mutable std::atomic<std::size_t> capability_calls_ = 0;
};

class AccountProbe final : public mt5bridge::runtime::CurrentAccountProbe {
public:
    std::optional<mt5bridge::AccountKey> current_account() override {
        return account();
    }
};

class Transport final : public mt5bridge::runtime::DispatchTransport {
public:
    mt5bridge::runtime::BackendCallResult submit_once(
        const mt5bridge::OperationRecord &) override {
        ++calls;
        return {mt5bridge::runtime::BackendCallStatus::broker_result,
                mt5bridge::runtime::BrokerResultDisposition::accepted, 10009,
                {0xA0}};
    }

    std::size_t calls = 0;
};

struct ManagerFixture {
    ManagerFixture(std::shared_ptr<SharedRecords> records,
                   SerializedLease &lease, std::uint64_t trade_id,
                   std::uint64_t ticket)
        : store(std::move(records)), journal(store), provider(std::make_unique<
              mt5bridge_test_support::FakeObservationProvider>(
              std::vector<mt5bridge::ObservationBatch>{
                  observation_batch(account(), ticket),
                  observation_batch(account(), ticket)})),
          coordinator(*provider, account()), admission(journal, coordinator.graph(), {}),
          manager(make_state(trade_id), account(), journal, admission, backend,
                  account_probe, lease, transport, coordinator, collection) {
        require(coordinator.refresh(collection).sample &&
                    coordinator.refresh(collection).sample,
                "concurrency fixture observation failed");
        require(manager.start().status ==
                    mt5bridge::dispatch::PrivateTradeManagerStatus::ready,
                "concurrency fixture manager startup failed");
    }

    static mt5bridge::managed_trade::ManagedTradeState make_state(
        std::uint64_t trade_id) {
        mt5bridge::managed_trade::ManagedTradeState state;
        require(mt5bridge::managed_trade::ManagedTradeState::initialize(
                    &state, mt5bridge::managed_trade::TradeId{trade_id}, 1, 1, 2),
                "concurrency fixture state initialization failed");
        return state;
    }

    mt5bridge::dispatch::ManagedTradeIntent intent() {
        return {{'{', '}'},
                {account(), coordinator.capture_baseline(),
                 {mt5bridge::require_active_order(7000)},
                 mt5bridge::OperationState::filled, manager.state()->trade_id.value, 1,
                 1}};
    }

    SharedStore store;
    mt5bridge::OperationJournal journal;
    std::unique_ptr<mt5bridge_test_support::FakeObservationProvider> provider;
    mt5bridge::ObservationCoordinator coordinator;
    mt5bridge::DispatchAdmissionBarrier admission;
    mt5bridge::runtime::OneShotDispatchBackend backend;
    AccountProbe account_probe;
    Transport transport;
    mt5bridge::ObservationCollectionRequest collection;
    mt5bridge::dispatch::PrivateTradeManager manager;
};

void test_two_managers_share_one_account_capability() {
    auto records = std::make_shared<SharedRecords>();
    SerializedLease lease(account());
    ManagerFixture first(records, lease, 7, 7001);
    ManagerFixture second(records, lease, 8, 7002);
    lease.pause_first_callback();

    std::promise<void> second_started;
    std::atomic<bool> second_done = false;
    std::optional<mt5bridge::dispatch::PrivateTradeManagerResult> first_result;
    std::optional<mt5bridge::dispatch::PrivateTradeManagerResult> second_result;

    std::thread first_thread([&] {
        auto first_intent = first.intent();
        first_result = first.manager.prepare_open(1, std::move(first_intent));
    });
    lease.wait_first_entered();
    std::thread second_thread([&] {
        second_started.set_value();
        second_result = second.manager.prepare_open(1, second.intent());
        second_done = true;
    });
    second_started.get_future().wait();
    const bool bypassed = second_done.load() || lease.second_entered();
    lease.release_first();
    first_thread.join();
    second_thread.join();

    require(!bypassed, "second manager entered while first capability was held");
    require(first_result && first_result->owner &&
                first_result->owner->status ==
                    mt5bridge::dispatch::OwnerStepStatus::prepared,
            "first manager did not prepare its OPEN");
    require(second_result &&
                second_result->status ==
                    mt5bridge::dispatch::PrivateTradeManagerStatus::blocked &&
                !second_result->owner,
            "second manager bypassed the prepared same-account blocker");
    require(first.journal.find({account(), 7, 1}).has_value() &&
                !first.journal.find({account(), 8, 1}).has_value(),
            "same-account interleaving left an unexpected durable operation");
    require(first.transport.calls == 0 && second.transport.calls == 0,
            "preparation interleaving invoked broker transport");
}

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

    const std::filesystem::path &path() const { return path_; }

private:
    std::filesystem::path path_;
};

void test_windows_store_preserves_blocker_after_restart_scan() {
    TestRoot root;
    const auto journal_path = root.path() / "journal";
    mt5bridge::WindowsFileJournalStore writer_store(journal_path);
    mt5bridge::WindowsFileJournalStore reader_store(journal_path);
    mt5bridge::WindowsSingleWriterLease lease(journal_path, account());
    require(writer_store.ready() && reader_store.ready() && lease.ready(),
            "Windows concurrency stores were not ready");

    mt5bridge::OperationJournal writer(writer_store);
    const auto key = mt5bridge::OperationKey{account(), 90, 1};
    const auto baseline = mt5bridge::ReconciliationBaseline::restore(
        account(), 1, 1, 1, 1, 1, 1);
    require(baseline && writer.create(key, {'{', '}'}, mt5bridge::OperationKind::open)
                         .accepted() &&
                writer.transition_operation(key,
                                           mt5bridge::OperationState::prechecking)
                    .accepted() &&
                writer.transition_journal(key, mt5bridge::JournalState::prechecked)
                    .accepted() &&
                writer.transition_journal(
                    key, mt5bridge::JournalState::dispatch_intent_persisted)
                    .accepted() &&
                writer.persist_reconciliation_descriptor(
                           key,
                           {account(), *baseline,
                            {mt5bridge::require_active_order(7001)},
                            mt5bridge::OperationState::filled, 90, 1, 1})
                    .accepted(),
            "Windows blocker fixture was not durable");

    mt5bridge::OperationJournal reader(reader_store);
    mt5bridge::dispatch::PrivateOwnerLoop loop(reader, account(), lease);
    require(loop.start().ready(), "Windows restart owner did not recover");
    require(loop.intent_admission({account(), 91, 1}).status ==
                mt5bridge::dispatch::PrivateOwnerLoopIntentStatus::unresolved_block,
            "Windows restart scan lost the account-wide blocker");
}

} // namespace

int main() {
    try {
        test_two_managers_share_one_account_capability();
        test_windows_store_preserves_blocker_after_restart_scan();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
