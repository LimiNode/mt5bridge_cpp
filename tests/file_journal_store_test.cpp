/// \file file_journal_store_test.cpp
/// \brief Exercises the Windows-backed durable journal store.

#include <mt5bridge.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
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
}

} // namespace

/// \brief Runs file-store durability and corruption checks.
/// \return Zero on success; non-zero when an invariant fails.
int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           "mt5bridge_file_journal_store_test";
    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);

    try {
        mt5bridge::WindowsFileJournalStore store(directory);
        require(store.ready(), "file journal store did not open its directory");

        const auto operation_key = key();
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
        require(recovered_record && recovered_record->journal_state ==
                                       mt5bridge::JournalState::dispatching &&
                    recovered_record->fencing_token == 77,
                "dispatching record did not survive store reopen");

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
        require(result_record && result_record->operation_state ==
                                       mt5bridge::OperationState::accepted &&
                    result_record->result_payload == std::vector<std::uint8_t>({0xA0, 0x01}),
                "result payload did not survive reopen");

        const auto duplicate_key = key(8, 12);
        mt5bridge::OperationJournal duplicate_owner(result_store);
        require(duplicate_owner.create(duplicate_key, {0x09}).accepted(),
                "duplicate setup create failed");
        mt5bridge::OperationJournal second_duplicate_owner(result_store);
        require(second_duplicate_owner.create(duplicate_key, {0x0A}).status ==
                    mt5bridge::JournalMutationStatus::conflict,
                "create CAS did not reject an existing durable record");

        std::size_t record_count = 0;
        for (const auto &entry : std::filesystem::directory_iterator(directory)) {
            if (entry.path().extension() == L".bin") {
                std::ofstream corrupt(entry.path(),
                                      std::ios::binary | std::ios::trunc);
                corrupt.put('X');
                ++record_count;
            }
        }
        require(record_count == 2, "record files were not created");
        require(!result_store.load(operation_key),
                "corrupt record was accepted during recovery");
        require(!result_store.last_error().empty(),
                "corrupt record did not produce a diagnostic");
        auto replacement_candidate = *result_record;
        ++replacement_candidate.revision;
        require(result_store.commit(replacement_candidate, result_record->revision) ==
                    mt5bridge::StoreCommitStatus::io_error,
                "corrupt record was overwritten instead of failing closed");

        std::filesystem::remove_all(directory, cleanup_error);
        std::cout << "file journal store checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::filesystem::remove_all(directory, cleanup_error);
        std::cerr << "file journal store checks failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
