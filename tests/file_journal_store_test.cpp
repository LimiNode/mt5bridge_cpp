/// \file file_journal_store_test.cpp
/// \brief Exercises the Windows-backed durable journal store.

#include <mt5bridge.hpp>

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <sstream>
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
    descriptor.requested_volume = 5;
    require(journal.persist_reconciliation_descriptor(operation_key,
                                                       std::move(descriptor))
                .accepted(),
            "reconciliation descriptor transition failed");
}

void append_u32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
    for (unsigned shift = 0; shift != 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void append_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
    for (unsigned shift = 0; shift != 64; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void append_string(std::vector<std::uint8_t> &bytes, const std::string &value) {
    require(value.size() <= (std::numeric_limits<std::uint32_t>::max)(),
            "legacy fixture string is too large");
    append_u32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

void append_payload(std::vector<std::uint8_t> &bytes,
                    const std::vector<std::uint8_t> &value) {
    require(value.size() <= (std::numeric_limits<std::uint32_t>::max)(),
            "legacy fixture payload is too large");
    append_u32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

std::uint64_t fixture_checksum(const std::vector<std::uint8_t> &bytes) {
    std::uint64_t result = 1469598103934665603ULL;
    for (const auto byte : bytes) {
        result ^= byte;
        result *= 1099511628211ULL;
    }
    return result;
}

std::string fixture_hex(std::uint64_t value) {
    std::ostringstream stream;
    stream << std::hex << std::setw(16) << std::setfill('0') << value;
    return stream.str();
}

std::uint64_t fixture_fnv1a(const std::string &value) {
    std::uint64_t result = 1469598103934665603ULL;
    for (const unsigned char byte : value) {
        result ^= byte;
        result *= 1099511628211ULL;
    }
    return result;
}

std::filesystem::path fixture_path(const std::filesystem::path &directory,
                                   const mt5bridge::OperationKey &operation_key) {
    const auto suffix = "op-" + fixture_hex(fixture_fnv1a(operation_key.account.server)) +
                        "-" + fixture_hex(operation_key.account.login) + "-" +
                        fixture_hex(operation_key.trade_id) + "-" +
                        fixture_hex(operation_key.operation_id) + ".bin";
    return directory / suffix;
}

std::vector<std::uint8_t> wrap_fixture(std::uint32_t version,
                                       const std::vector<std::uint8_t> &body) {
    const std::vector<std::uint8_t> magic{'M', 'T', '5', 'J', 'N', 'L', '0', '1'};
    std::vector<std::uint8_t> bytes(magic.begin(), magic.end());
    append_u32(bytes, version);
    append_u64(bytes, static_cast<std::uint64_t>(body.size()));
    append_u64(bytes, fixture_checksum(body));
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

std::vector<std::uint8_t> legacy_v1_body(const mt5bridge::OperationKey &operation_key) {
    std::vector<std::uint8_t> body;
    append_string(body, operation_key.account.server);
    append_payload(body, {0x11});
    append_payload(body, {});
    append_u64(body, operation_key.account.login);
    append_u64(body, operation_key.trade_id);
    append_u64(body, operation_key.operation_id);
    append_u32(body, static_cast<std::uint32_t>(mt5bridge::OperationState::queued));
    append_u32(body, static_cast<std::uint32_t>(mt5bridge::JournalState::created));
    append_u64(body, 1);
    append_u64(body, 0);
    return body;
}

std::vector<std::uint8_t> legacy_v2_body(const mt5bridge::OperationKey &operation_key) {
    mt5bridge::ObservationGraph graph(operation_key.account);
    const auto baseline = mt5bridge::capture_reconciliation_baseline(graph);
    const mt5bridge::ObservationWindow window{1000, 2000};
    std::vector<std::uint8_t> body;
    append_string(body, operation_key.account.server);
    append_payload(body, {0x22});
    append_payload(body, {0xA0});
    append_u64(body, operation_key.account.login);
    append_u64(body, operation_key.trade_id);
    append_u64(body, operation_key.operation_id);
    append_u32(body, static_cast<std::uint32_t>(mt5bridge::OperationState::reconciling));
    append_u32(body, static_cast<std::uint32_t>(mt5bridge::JournalState::reconciling));
    append_u64(body, 8);
    append_u64(body, 77);

    append_u32(body, 1);
    append_string(body, operation_key.account.server);
    append_u64(body, operation_key.account.login);
    append_u64(body, operation_key.trade_id);
    append_u64(body, operation_key.operation_id);
    append_string(body, baseline.account().server);
    append_u64(body, baseline.account().login);
    append_u64(body, baseline.graph_instance_id());
    append_u64(body, baseline.graph_revision());
    append_u64(body, baseline.active_orders_revision());
    append_u64(body, baseline.positions_revision());
    append_u64(body, baseline.history_orders_revision());
    append_u64(body, baseline.history_deals_revision());
    append_u32(body, static_cast<std::uint32_t>(mt5bridge::OperationState::filled));
    append_u32(body, 1);
    append_u32(body, static_cast<std::uint32_t>(
                           mt5bridge::ReconciliationPredicateKind::history_deal_present));
    append_u64(body, 0);
    append_u32(body, 1);
    append_u64(body, static_cast<std::uint64_t>(window.from_msc));
    append_u64(body, static_cast<std::uint64_t>(window.to_msc));
    append_u32(body, 1);
    append_u64(body, 701);
    append_u32(body, static_cast<std::uint32_t>(
                           mt5bridge::ReconciliationTransition::absent_to_present));

    append_u32(body, 1);
    append_u64(body, 701);
    append_u64(body, 730);
    return body;
}

std::vector<std::uint8_t> legacy_v3_body(
    const mt5bridge::OperationKey &operation_key,
    mt5bridge::OperationState operation_state,
    std::uint64_t settled_volume) {
    mt5bridge::ObservationGraph graph(operation_key.account);
    const auto baseline = mt5bridge::capture_reconciliation_baseline(graph);
    const mt5bridge::ObservationWindow window{1000, 2000};
    std::vector<std::uint8_t> body;
    append_string(body, operation_key.account.server);
    append_payload(body, {0x33});
    append_payload(body, {0xA1});
    append_u64(body, operation_key.account.login);
    append_u64(body, operation_key.trade_id);
    append_u64(body, operation_key.operation_id);
    append_u32(body, static_cast<std::uint32_t>(operation_state));
    append_u32(body, static_cast<std::uint32_t>(mt5bridge::JournalState::reconciling));
    append_u64(body, 9);
    append_u64(body, 78);
    append_u64(body, settled_volume);

    append_u32(body, 1);
    append_string(body, operation_key.account.server);
    append_u64(body, operation_key.account.login);
    append_u64(body, operation_key.trade_id);
    append_u64(body, operation_key.operation_id);
    append_u64(body, 5);
    append_string(body, baseline.account().server);
    append_u64(body, baseline.account().login);
    append_u64(body, baseline.graph_instance_id());
    append_u64(body, baseline.graph_revision());
    append_u64(body, baseline.active_orders_revision());
    append_u64(body, baseline.positions_revision());
    append_u64(body, baseline.history_orders_revision());
    append_u64(body, baseline.history_deals_revision());
    append_u32(body, static_cast<std::uint32_t>(mt5bridge::OperationState::filled));
    append_u32(body, 1);
    append_u32(body, static_cast<std::uint32_t>(
                           mt5bridge::ReconciliationPredicateKind::history_deal_present));
    append_u64(body, 0);
    append_u32(body, 1);
    append_u64(body, static_cast<std::uint64_t>(window.from_msc));
    append_u64(body, static_cast<std::uint64_t>(window.to_msc));
    append_u32(body, 1);
    append_u64(body, 701);
    append_u32(body, static_cast<std::uint32_t>(
                           mt5bridge::ReconciliationTransition::absent_to_present));

    append_u32(body, 1);
    append_u64(body, 701);
    append_u64(body, 730);
    return body;
}

void write_fixture(const std::filesystem::path &directory,
                   const mt5bridge::OperationKey &operation_key, std::uint32_t version,
                   const std::vector<std::uint8_t> &body) {
    std::ofstream output(fixture_path(directory, operation_key),
                         std::ios::binary | std::ios::trunc);
    const auto bytes = wrap_fixture(version, body);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(output.good(), "legacy fixture could not be written");
}

} // namespace

/// \brief Runs file-store durability and corruption checks.
/// \return Zero on success; non-zero when an invariant fails.
int main() {
    const auto directory = std::filesystem::temp_directory_path() /
                           "mt5bridge_file_journal_store_test";
    const auto legacy_directory = std::filesystem::temp_directory_path() /
                                  "mt5bridge_file_journal_store_legacy_test";
    const auto original_cwd = std::filesystem::current_path();
    std::error_code cleanup_error;
    std::filesystem::remove_all(directory, cleanup_error);
    std::filesystem::remove_all(legacy_directory, cleanup_error);

    try {
        mt5bridge::WindowsFileJournalStore store(directory);
        require(store.ready(), "file journal store did not open its directory");

        const auto legacy_v1_key = key(70, 71);
        const auto legacy_v2_key = key(72, 73);
        const auto legacy_v3_settled_key = key(74, 75);
        const auto legacy_v3_unresolved_key = key(76, 77);
        mt5bridge::WindowsFileJournalStore legacy_store(legacy_directory);
        require(legacy_store.ready(), "legacy fixture store did not open its directory");
        write_fixture(legacy_store.directory(), legacy_v1_key, 1,
                      legacy_v1_body(legacy_v1_key));
        write_fixture(legacy_store.directory(), legacy_v2_key, 2,
                      legacy_v2_body(legacy_v2_key));
        write_fixture(
            legacy_store.directory(), legacy_v3_settled_key, 3,
            legacy_v3_body(legacy_v3_settled_key,
                           mt5bridge::OperationState::filled, 5));
        write_fixture(
            legacy_store.directory(), legacy_v3_unresolved_key, 3,
            legacy_v3_body(legacy_v3_unresolved_key,
                           mt5bridge::OperationState::reconciling, 0));
        const auto legacy_v1 = legacy_store.load(legacy_v1_key);
        require(legacy_v1.found() && legacy_v1.record &&
                    !legacy_v1.record->reconciliation_descriptor &&
                    legacy_v1.record->operation_state ==
                        mt5bridge::OperationState::queued &&
                    legacy_v1.record->journal_state == mt5bridge::JournalState::created,
                "version-one journal fixture was not read conservatively");
        const auto legacy_v2 = legacy_store.load(legacy_v2_key);
        require(legacy_v2.found() && legacy_v2.record &&
                    legacy_v2.record->reconciliation_descriptor &&
                    legacy_v2.record->reconciliation_descriptor->trade_id ==
                        legacy_v2_key.trade_id &&
                    legacy_v2.record->reconciliation_descriptor->operation_id ==
                        legacy_v2_key.operation_id &&
                    legacy_v2.record->reconciliation_bindings.size() == 1 &&
                    legacy_v2.record->reconciliation_bindings.front() ==
                        mt5bridge::ReconciliationBinding{701, 730} &&
                    legacy_v2.record->settled_volume == 0 &&
                    legacy_v2.record->operation_kind ==
                        mt5bridge::OperationKind::unspecified,
                "version-two journal fixture did not preserve causal metadata");
        const auto legacy_v3_settled = legacy_store.load(legacy_v3_settled_key);
        require(legacy_v3_settled.found() && legacy_v3_settled.record &&
                    legacy_v3_settled.record->operation_kind ==
                        mt5bridge::OperationKind::open &&
                    legacy_v3_settled.record->settled_volume == 5 &&
                    legacy_v3_settled.record->reconciliation_descriptor &&
                    legacy_v3_settled.record->reconciliation_descriptor
                            ->requested_volume == 5,
                "version-three journal fixture did not migrate settled OPEN kind");
        const auto legacy_v3_unresolved =
            legacy_store.load(legacy_v3_unresolved_key);
        require(legacy_v3_unresolved.found() && legacy_v3_unresolved.record &&
                    legacy_v3_unresolved.record->operation_kind ==
                        mt5bridge::OperationKind::open &&
                    legacy_v3_unresolved.record->settled_volume == 0 &&
                    legacy_v3_unresolved.record->operation_state ==
                        mt5bridge::OperationState::reconciling,
                "version-three unresolved OPEN lost its durable kind migration");
        const auto legacy_scan = legacy_store.scan();
        require(legacy_scan.complete() && legacy_scan.records.size() == 4,
                "legacy journal fixtures did not survive a complete scan");

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
        require(journal
                    .create(operation_key, {0x01, 0x02, 0x03},
                            mt5bridge::OperationKind::open)
                    .accepted(),
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
                    recovered_record.record->operation_kind ==
                        mt5bridge::OperationKind::open &&
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
        const auto settled_reconciling = owner_a.transition_operation(
            operation_key, mt5bridge::OperationState::reconciling);
        require(settled_reconciling.accepted(),
                "managed operation did not enter reconciliation");
        require(owner_a.transition_journal(operation_key,
                                           mt5bridge::JournalState::reconciling)
                    .accepted(),
                "settlement journal barrier was not committed");
        const auto reconciling_record = owner_a.find(operation_key);
        require(reconciling_record.has_value(), "reconciling record was not retained");
        auto settled_record_value = *reconciling_record;
        settled_record_value.operation_state = mt5bridge::OperationState::filled;
        settled_record_value.settled_volume = 5;
        ++settled_record_value.revision;
        require(settled_record_value.valid() &&
                    store.commit(settled_record_value, reconciling_record->revision) ==
                        mt5bridge::StoreCommitStatus::committed,
                "settled managed volume was not committed");
        mt5bridge::WindowsFileJournalStore settled_store(directory);
        const auto settled_record = settled_store.load(operation_key);
        require(settled_record.found() && settled_record.record->operation_state ==
                                       mt5bridge::OperationState::filled &&
                    settled_record.record->settled_volume == 5 &&
                    settled_record.record->reconciliation_descriptor &&
                    settled_record.record->reconciliation_descriptor->requested_volume == 5,
                "settled managed volume did not survive store reopen");
        require(result_store.load(key(99, 100)).status ==
                    mt5bridge::StoreLoadStatus::not_found,
                "missing operation was not distinguished from storage failure");

        const auto duplicate_key = key(8, 12);
        mt5bridge::OperationJournal duplicate_owner(result_store);
        require(duplicate_owner
                    .create(duplicate_key, {0x09}, mt5bridge::OperationKind::close)
                    .accepted(),
                "duplicate setup create failed");
        mt5bridge::OperationJournal second_duplicate_owner(result_store);
        require(second_duplicate_owner.create(duplicate_key, {0x0A}).status ==
                    mt5bridge::JournalMutationStatus::conflict,
                "create CAS did not reject an existing durable record");

        mt5bridge::OperationJournal restarted(result_store);
        const auto recovered_all = restarted.recover_all();
        require(recovered_all.accepted() && recovered_all.records().size() == 2 &&
                    restarted.find(operation_key) && restarted.find(duplicate_key) &&
                    restarted.find(duplicate_key)->operation_kind ==
                        mt5bridge::OperationKind::close,
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
        require(corrupted_journal.recover_all().status() ==
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
        std::filesystem::remove_all(legacy_directory, cleanup_error);
        std::cout << "file journal store checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::filesystem::current_path(original_cwd);
        std::filesystem::remove_all(directory, cleanup_error);
        std::filesystem::remove_all(legacy_directory, cleanup_error);
        std::cerr << "file journal store checks failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
