/// \file file_journal_store_test.cpp
/// \brief Exercises the Windows-backed durable journal store.

#include <mt5bridge.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

mt5bridge::OperationKey key(std::uint64_t trade_id = 7,
                            std::uint64_t operation_id = 11) {
    return {mt5bridge::AccountKey{"Demo-Trade", 42}, trade_id, operation_id};
}

void prepare(mt5bridge::OperationJournal &journal,
             const mt5bridge::OperationKey &operation_key) {
    require(journal.transition_operation(operation_key,
                                         mt5bridge::OperationState::prechecking)
                .accepted(),
            "prechecking transition failed");
    require(journal.transition_journal(operation_key, mt5bridge::JournalState::prechecked)
                .accepted(),
            "prechecked transition failed");
    require(journal.transition_journal(
                operation_key, mt5bridge::JournalState::dispatch_intent_persisted)
                .accepted(),
            "dispatch intent transition failed");
    mt5bridge::ObservationGraph graph(operation_key.account);
    const mt5bridge::ObservationWindow history_window{1000, 2000};
    mt5bridge::ReconciliationDescriptor descriptor{
        operation_key.account, mt5bridge::capture_reconciliation_baseline(graph),
        {mt5bridge::require_active_order(20),
         mt5bridge::require_history_order(21, history_window)},
        mt5bridge::OperationState::filled};
    require(journal.persist_reconciliation_descriptor(operation_key,
                                                       std::move(descriptor))
                .accepted(),
            "reconciliation descriptor transition failed");
}

} // namespace

/// \brief Runs file-store durability and corruption checks.
/// \return Zero on success; non-zero when an invariant fails.
int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           "mt5bridge_file_journal_store_test";
    const auto original_cwd = std::filesystem::current_path();
    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);

    try {
        mt5bridge::WindowsFileJournalStore store(directory);
        require(store.ready(), "file journal store did not open its directory");

        const auto operation_key = key();
        const auto lease_account = operation_key.account;
        std::uint64_t first_fencing_token = 0;
        {
            mt5bridge::WindowsSingleWriterLease lease(directory, lease_account);
            require(lease.ready(), "production writer lease did not open");
            const auto held = lease.held_fencing_token(lease_account);
            require(held && *held != 0, "writer lease did not expose a token");
            first_fencing_token = *held;
            require(!lease.held_fencing_token(mt5bridge::AccountKey{"Other-Trade", 43}),
                    "writer lease exposed its token to another account");

            mt5bridge::WindowsSingleWriterLease duplicate(directory, lease_account);
            require(!duplicate.ready() &&
                        !duplicate.held_fencing_token(lease_account),
                    "duplicate account owner acquired the writer lease");
        }
        {
            mt5bridge::WindowsSingleWriterLease successor(directory, lease_account);
            require(successor.ready(), "writer lease was not reacquired after release");
            const auto held = successor.held_fencing_token(lease_account);
            require(held && *held > first_fencing_token,
                    "fencing epoch did not advance durably");
        }

        mt5bridge::OperationJournal journal(store);
        require(journal.create(operation_key, {0x01, 0x02, 0x03}).accepted(),
                "file-backed create was not committed");
        prepare(journal, operation_key);
        require(journal.transition_journal(operation_key,
                                           mt5bridge::JournalState::dispatching, 77)
                    .accepted(),
                "dispatching barrier was not committed");

        mt5bridge::WindowsFileJournalStore recovered_store(directory);
        const auto recovered_record = recovered_store.load(operation_key);
        require(recovered_record.found() && recovered_record.record->journal_state ==
                                       mt5bridge::JournalState::dispatching &&
                    recovered_record.record->fencing_token == 77 &&
                    recovered_record.record->reconciliation_descriptor &&
                    recovered_record.record->reconciliation_descriptor->valid() &&
                    recovered_record.record->reconciliation_descriptor->predicates.size() ==
                        2 &&
                    recovered_record.record->reconciliation_descriptor->predicates.front()
                            .ticket ==
                        20 &&
                    recovered_record.record->reconciliation_descriptor->predicates.back()
                            .history_window &&
                    recovered_record.record->reconciliation_descriptor->predicates.back()
                            .history_window->from_msc ==
                        1000 &&
                    recovered_record.record->reconciliation_descriptor->predicates.back()
                            .history_window->to_msc ==
                        2000,
                "dispatching record or reconciliation descriptor did not survive store reopen");

        mt5bridge::OperationJournal owner_a(store);
        mt5bridge::OperationJournal owner_b(recovered_store);
        require(owner_a.recover(operation_key).accepted() &&
                    owner_b.recover(operation_key).accepted(),
                "two owner loops could not recover the same record");
        require(owner_a.transition_operation(operation_key,
                                             mt5bridge::OperationState::submitting)
                    .accepted(),
                "first owner could not claim submitting");
        require(owner_b.transition_operation(operation_key,
                                             mt5bridge::OperationState::submitting)
                        .status == mt5bridge::JournalMutationStatus::conflict,
                "stale owner overwrote the durable revision");
        require(owner_a.persist_result(operation_key, {0xA0, 0x01}).accepted(),
                "result payload was not atomically persisted");
        require(owner_a.transition_operation(operation_key,
                                             mt5bridge::OperationState::accepted)
                    .accepted(),
                "accepted state was not committed after result persistence");

        mt5bridge::WindowsFileJournalStore result_store(directory);
        const auto result_record = result_store.load(operation_key);
        require(result_record.found() && result_record.record->operation_state ==
                                       mt5bridge::OperationState::accepted &&
                    result_record.record->result_payload ==
                        std::vector<std::uint8_t>({0xA0, 0x01}),
                "result payload did not survive reopen");
        require(result_store.load(key(99, 100)).status ==
                    mt5bridge::StoreLoadStatus::not_found,
                "missing operation was not distinguished from storage failure");

        const auto duplicate_key = key(8, 12);
        mt5bridge::OperationJournal duplicate_owner(result_store);
        require(duplicate_owner.create(duplicate_key, {0x09}).accepted(),
                "duplicate setup create failed");
        mt5bridge::OperationJournal second_duplicate_owner(result_store);
        require(second_duplicate_owner.create(duplicate_key, {0x0A}).status ==
                    mt5bridge::JournalMutationStatus::conflict,
                "create CAS did not reject an existing durable record");

        mt5bridge::OperationJournal restarted(result_store);
        const auto recovered_all = restarted.recover_all();
        require(recovered_all.accepted() && recovered_all.records.size() == 2 &&
                    restarted.find(operation_key) && restarted.find(duplicate_key),
                "restart enumeration did not recover every durable operation");

        const auto crash_key = key(20, 21);
        {
            mt5bridge::OperationJournal before_crash(store);
            require(before_crash.create(crash_key, {0x0D}).accepted(),
                    "crash-case operation create failed");
            prepare(before_crash, crash_key);
            require(before_crash
                        .transition_journal(crash_key, mt5bridge::JournalState::dispatching,
                                             88)
                        .accepted() &&
                        before_crash
                            .transition_operation(crash_key,
                                                   mt5bridge::OperationState::submitting)
                            .accepted(),
                    "crash-case submitting state was not committed");
        }
        {
            mt5bridge::WindowsFileJournalStore post_crash_store(directory);
            mt5bridge::OperationJournal after_crash(post_crash_store);
            const auto crash_recovered = after_crash.recover_all();
            const auto crash_record = after_crash.find(crash_key);
            require(crash_recovered.accepted() && crash_record &&
                        crash_record->journal_state == mt5bridge::JournalState::dispatching &&
                        crash_record->operation_state == mt5bridge::OperationState::submitting &&
                        !mt5bridge::OperationJournal::can_transition_operation(
                            mt5bridge::OperationState::submitting,
                            mt5bridge::OperationState::submitting),
                    "restart did not rediscover unresolved submitting operation");
        }

        const auto cwd_root = directory / "cwd-regression";
        const auto cwd_a = cwd_root / "a";
        const auto cwd_b = cwd_root / "b";
        std::filesystem::create_directories(cwd_a);
        std::filesystem::create_directories(cwd_b);
        std::filesystem::current_path(cwd_a);
        mt5bridge::WindowsFileJournalStore anchored_store("relative-journal");
        require(anchored_store.ready() && anchored_store.directory().is_absolute(),
                "relative journal path was not anchored at construction");
        const auto anchored_key = key(15, 16);
        mt5bridge::OperationJournal anchored_journal(anchored_store);
        require(anchored_journal.create(anchored_key, {0x0B}).accepted(),
                "relative journal create failed");
        const auto anchored_directory = anchored_store.directory();
        std::filesystem::current_path(cwd_b);
        const auto anchored_load = anchored_store.load(anchored_key);
        require(anchored_load.found() && anchored_store.directory() == anchored_directory,
                "journal operations followed a changed process CWD");
        std::filesystem::current_path(original_cwd);

        std::size_t record_count = 0;
        for (const auto &entry : std::filesystem::directory_iterator(directory)) {
            if (entry.path().extension() == L".bin") {
                std::ofstream corrupt(entry.path(),
                                      std::ios::binary | std::ios::trunc);
                corrupt.put('X');
                ++record_count;
            }
        }
        require(record_count == 3, "record files were not created");
        const auto corrupt_load = result_store.load(operation_key);
        require(corrupt_load.status == mt5bridge::StoreLoadStatus::invalid_record,
                "corrupt record was accepted during recovery");
        require(!result_store.last_error().empty(),
                "corrupt record did not produce a diagnostic");
        require(result_store.scan().status == mt5bridge::StoreScanStatus::invalid_record,
                "corrupt record did not invalidate complete restart scan");
        mt5bridge::OperationJournal corrupted_journal(result_store);
        require(corrupted_journal.recover(operation_key).status ==
                    mt5bridge::JournalMutationStatus::invalid_record,
                "corrupt single-record recovery was reported as missing");
        require(corrupted_journal.recover_all().status ==
                    mt5bridge::JournalMutationStatus::invalid_record &&
                    !corrupted_journal.find(operation_key),
                "corrupt restart scan partially replaced the owner cache");
        auto replacement_candidate = *result_record.record;
        ++replacement_candidate.revision;
        require(result_store.commit(replacement_candidate, result_record.record->revision) ==
                    mt5bridge::StoreCommitStatus::io_error,
                "corrupt record was overwritten instead of failing closed");

        std::filesystem::path epoch_path;
        for (const auto &entry : std::filesystem::directory_iterator(directory)) {
            if (entry.path().extension() == L".epoch") {
                epoch_path = entry.path();
                break;
            }
        }
        require(!epoch_path.empty(), "fencing epoch file was not created");
        std::vector<char> epoch_bytes;
        {
            std::ifstream input(epoch_path, std::ios::binary);
            epoch_bytes.assign(std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>());
        }
        require(epoch_bytes.size() > 8, "fencing epoch envelope was unexpectedly short");
        {
            auto corrupt_bytes = epoch_bytes;
            std::ofstream corrupt_epoch(epoch_path,
                                        std::ios::binary | std::ios::trunc);
            corrupt_bytes[corrupt_bytes.size() / 2] ^= static_cast<char>(0x01);
            corrupt_epoch.write(corrupt_bytes.data(),
                                static_cast<std::streamsize>(corrupt_bytes.size()));
        }
        mt5bridge::WindowsSingleWriterLease corrupt_lease(directory, lease_account);
        require(!corrupt_lease.ready() && !corrupt_lease.last_error().empty(),
                "corrupt fencing epoch was accepted");
        {
            std::ofstream restore_epoch(epoch_path,
                                        std::ios::binary | std::ios::trunc);
            restore_epoch.write(epoch_bytes.data(),
                                static_cast<std::streamsize>(epoch_bytes.size()));
        }
        mt5bridge::WindowsSingleWriterLease successor_while_failed(directory,
                                                                     lease_account);
        require(successor_while_failed.ready(),
                "failed lease retained the account lock after initialization error");

        std::filesystem::remove_all(directory, cleanup_error);
        std::cout << "file journal store checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::filesystem::current_path(original_cwd);
        std::filesystem::remove_all(directory, cleanup_error);
        std::cerr << "file journal store checks failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
