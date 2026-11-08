/// \file private_owner_loop_test.cpp
/// \brief Exercises private startup recovery and owner-loop blocking rules.

#include "private_owner_loop.hpp"

#include <mt5bridge/dispatch/file_journal_store.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

class TestRoot final {
public:
    explicit TestRoot(const char *label) {
        const auto stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        for (unsigned int attempt = 0; attempt != 100; ++attempt) {
            path_ = std::filesystem::temp_directory_path() /
                    (std::string(label) + "-" + std::to_string(stamp) + "-" +
                     std::to_string(attempt));
            std::error_code error;
            if (std::filesystem::create_directories(path_, error) && !error)
                return;
        }
        throw std::runtime_error("could not create owner-loop test directory");
    }

    ~TestRoot() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path &path() const { return path_; }

private:
    std::filesystem::path path_;
};

mt5bridge::AccountKey account() { return {"Fake-Server", 42}; }

class ToggleLease final : public mt5bridge::SingleWriterLease {
public:
    explicit ToggleLease(mt5bridge::AccountKey account) : account_(std::move(account)) {}

    std::optional<std::uint64_t> held_fencing_token(
        const mt5bridge::AccountKey &account) const override {
        if (!held_ || account != account_)
            return std::nullopt;
        return token_;
    }

    void revoke() { held_ = false; }

private:
    mt5bridge::AccountKey account_;
    std::uint64_t token_ = 17;
    bool held_ = true;
};

mt5bridge::ReconciliationDescriptor descriptor(
    const mt5bridge::AccountKey &key, std::uint64_t operation_id) {
    const auto baseline = mt5bridge::ReconciliationBaseline::restore(
        key, 1, 1, 1, 1, 1, 1);
    require(baseline.has_value(), "test baseline was not valid");
    return {key,
            *baseline,
            {mt5bridge::require_active_order(7001)},
            mt5bridge::OperationState::filled,
            7,
            operation_id,
            1};
}

void prepare_record(mt5bridge::OperationJournal &journal,
                    const mt5bridge::OperationKey &key,
                    bool dispatching) {
    require(journal.create(key, {'{', '}'}, mt5bridge::OperationKind::open)
                .accepted(),
            "could not create durable owner-loop record");
    require(journal.transition_operation(key, mt5bridge::OperationState::prechecking)
                .accepted(),
            "could not enter prechecking");
    require(journal.transition_journal(key, mt5bridge::JournalState::prechecked)
                .accepted(),
            "could not persist prechecked state");
    require(journal.transition_journal(
                key, mt5bridge::JournalState::dispatch_intent_persisted)
                .accepted(),
            "could not persist dispatch intent");
    require(journal.persist_reconciliation_descriptor(
                key, descriptor(key.account, key.operation_id))
                .accepted(),
            "could not persist recovery descriptor");
    if (dispatching)
        require(journal.transition_journal(key, mt5bridge::JournalState::dispatching,
                                           17)
                    .accepted(),
                "could not persist dispatch barrier");
}

void settle_open(mt5bridge::WindowsFileJournalStore &store,
                 mt5bridge::OperationJournal &journal,
                 const mt5bridge::OperationKey &key) {
    require(journal.transition_journal(key, mt5bridge::JournalState::reconciling)
                .accepted(),
            "could not enter reconciliation");
    require(journal.transition_operation(key, mt5bridge::OperationState::reconciling)
                .accepted(),
            "could not persist reconciliation state");
    const auto current = journal.find(key);
    require(current.has_value(), "settlement lost its durable record");
    auto settled = *current;
    settled.operation_state = mt5bridge::OperationState::filled;
    settled.settled_volume = 1;
    ++settled.revision;
    require(settled.valid() &&
                store.commit(settled, current->revision) ==
                    mt5bridge::StoreCommitStatus::committed,
            "could not persist settled evidence");
    require(journal.recover_all().accepted(),
            "owner journal did not reload settled evidence");
}

void test_empty_startup_and_intent() {
    TestRoot root("private-owner-empty");
    mt5bridge::WindowsFileJournalStore store(root.path() / "journal");
    ToggleLease lease(account());
    mt5bridge::OperationJournal journal(store);
    mt5bridge::dispatch::PrivateOwnerLoop loop(journal, account(), lease);

    const auto startup = loop.start();
    require(startup.ready() && startup.operation_count == 0,
            "empty journal did not start ready");
    require(loop.step().status ==
                mt5bridge::dispatch::PrivateOwnerLoopStepStatus::idle,
            "empty journal was not idle");
    const mt5bridge::OperationKey key{account(), 7, 1};
    require(loop.intent_admission(key).accepted(),
            "empty ready owner rejected a new intent");
}

void test_terminal_open_is_replayed_without_blocking() {
    TestRoot root("private-owner-terminal");
    mt5bridge::WindowsFileJournalStore store(root.path() / "journal");
    ToggleLease lease(account());
    mt5bridge::OperationJournal journal(store);
    const mt5bridge::OperationKey key{account(), 7, 1};
    prepare_record(journal, key, true);
    require(journal.transition_operation(key, mt5bridge::OperationState::submitting)
                .accepted(),
            "could not enter submitting");
    require(journal.persist_result(key, {'o', 'k'}).accepted(),
            "could not persist broker result");
    require(journal.transition_operation(key, mt5bridge::OperationState::accepted)
                .accepted(),
            "could not persist accepted result");
    settle_open(store, journal, key);

    mt5bridge::dispatch::PrivateOwnerLoop loop(journal, account(), lease);
    require(loop.start().ready(), "terminal journal did not recover");
    require(loop.step().status ==
                mt5bridge::dispatch::PrivateOwnerLoopStepStatus::idle,
            "terminal OPEN incorrectly blocked startup");
    require(loop.intent_admission({account(), 7, 2}).accepted(),
            "terminal OPEN blocked a new intent");
}

void test_unresolved_open_is_reconcile_only_and_blocks_intent() {
    TestRoot root("private-owner-unresolved");
    mt5bridge::WindowsFileJournalStore store(root.path() / "journal");
    ToggleLease lease(account());
    mt5bridge::OperationJournal journal(store);
    const mt5bridge::OperationKey key{account(), 7, 1};
    prepare_record(journal, key, true);

    mt5bridge::dispatch::PrivateOwnerLoop loop(journal, account(), lease);
    require(loop.start().ready(), "unresolved journal did not recover");
    const auto pending = loop.step();
    require(pending.status ==
                mt5bridge::dispatch::PrivateOwnerLoopStepStatus::reconcile_only &&
                pending.operation && pending.non_resendable(),
            "unresolved OPEN was not classified reconcile-only");
    require(loop.intent_admission({account(), 7, 2}).status ==
                mt5bridge::dispatch::PrivateOwnerLoopIntentStatus::unresolved_block,
            "unresolved OPEN did not block a new intent");
    const auto before = journal.find(key);
    require(before && before->operation_state == mt5bridge::OperationState::prechecking,
            "recovery fabricated settlement evidence");

    settle_open(store, journal, key);
    require(loop.step().status ==
                mt5bridge::dispatch::PrivateOwnerLoopStepStatus::idle,
            "late durable evidence did not release reconciliation");
    require(loop.intent_admission({account(), 7, 2}).accepted(),
            "settled OPEN kept blocking a new intent");
    const auto after = journal.find(key);
    require(after && after->settled_volume == 1 &&
                after->operation_state == mt5bridge::OperationState::filled,
            "settled OPEN volume was not stable after repeated observation");
}

void test_pre_dispatch_resume_remains_admission_gated() {
    TestRoot root("private-owner-predispatch");
    mt5bridge::WindowsFileJournalStore store(root.path() / "journal");
    ToggleLease lease(account());
    mt5bridge::OperationJournal journal(store);
    const mt5bridge::OperationKey key{account(), 7, 1};
    prepare_record(journal, key, false);

    mt5bridge::dispatch::PrivateOwnerLoop loop(journal, account(), lease);
    require(loop.start().ready(), "pre-dispatch journal did not recover");
    const auto pending = loop.step();
    require(pending.status ==
                mt5bridge::dispatch::PrivateOwnerLoopStepStatus::resume_pre_dispatch &&
                pending.operation && !pending.non_resendable(),
            "pre-dispatch operation was not classified for normal admission");
    require(loop.intent_admission({account(), 7, 2}).status ==
                mt5bridge::dispatch::PrivateOwnerLoopIntentStatus::unresolved_block,
            "pre-dispatch operation did not block concurrent intent");
    require(journal.transition_journal(key, mt5bridge::JournalState::dispatching, 17)
                .accepted(),
            "pre-dispatch record could not cross its fenced barrier");
    require(loop.step().status ==
                mt5bridge::dispatch::PrivateOwnerLoopStepStatus::reconcile_only,
            "barrier crossing did not become non-resendable");
}

void test_lease_loss_blocks_steps_and_intent() {
    TestRoot root("private-owner-lease");
    mt5bridge::WindowsFileJournalStore store(root.path() / "journal");
    ToggleLease lease(account());
    mt5bridge::OperationJournal journal(store);
    mt5bridge::dispatch::PrivateOwnerLoop loop(journal, account(), lease);
    require(loop.start().ready(), "lease test did not start");
    lease.revoke();
    require(loop.step().status ==
                mt5bridge::dispatch::PrivateOwnerLoopStepStatus::lease_lost,
            "lost lease did not stop owner steps");
    require(loop.intent_admission({account(), 7, 1}).status ==
                mt5bridge::dispatch::PrivateOwnerLoopIntentStatus::not_ready,
            "lost lease did not block a new intent");
}

void test_corruption_blocks_startup() {
    TestRoot root("private-owner-corrupt");
    const auto path = root.path() / "journal";
    {
        std::ofstream file(path, std::ios::binary);
        file << "not-a-directory";
    }
    mt5bridge::WindowsFileJournalStore store(path);
    ToggleLease lease(account());
    mt5bridge::OperationJournal journal(store);
    mt5bridge::dispatch::PrivateOwnerLoop loop(journal, account(), lease);
    const auto startup = loop.start();
    require(!startup.ready() &&
                startup.status ==
                    mt5bridge::dispatch::PrivateOwnerLoopStartupStatus::recovery_failed,
            "corrupt journal did not block startup");
    require(loop.step().status ==
                mt5bridge::dispatch::PrivateOwnerLoopStepStatus::startup_required,
            "failed startup exposed owner steps");
}

void test_account_switch_blocks_intent() {
    TestRoot root("private-owner-account");
    mt5bridge::WindowsFileJournalStore store(root.path() / "journal");
    ToggleLease lease(account());
    mt5bridge::OperationJournal journal(store);
    mt5bridge::dispatch::PrivateOwnerLoop loop(journal, account(), lease);
    require(loop.start().ready(), "account test did not start");
    require(loop.intent_admission({{"Other-Server", 99}, 7, 1}).status ==
                mt5bridge::dispatch::PrivateOwnerLoopIntentStatus::account_mismatch,
            "foreign account intent was admitted");
}

} // namespace

int main() {
    try {
        test_empty_startup_and_intent();
        test_terminal_open_is_replayed_without_blocking();
        test_unresolved_open_is_reconcile_only_and_blocks_intent();
        test_pre_dispatch_resume_remains_admission_gated();
        test_lease_loss_blocks_steps_and_intent();
        test_corruption_blocks_startup();
        test_account_switch_blocks_intent();
        std::cout << "private owner-loop checks passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "private owner-loop checks failed: " << error.what() << '\n';
        return 1;
    }
}
