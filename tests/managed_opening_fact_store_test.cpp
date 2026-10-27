/// \file managed_opening_fact_store_test.cpp
/// \brief Verifies durable OPEN fact decoding, replay, fencing, and recovery.

#include "managed_opening_fact_store.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace mt5bridge;
using namespace mt5bridge::dispatch;

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

AccountKey account(std::uint64_t login = 42) { return {"demo", login}; }
AccountKey account(std::string server, std::uint64_t login) {
    return {std::move(server), login};
}

OperationKey operation(std::uint64_t login = 42, std::uint64_t operation_id = 9,
                       std::string server = "demo") {
    return {account(std::move(server), login), 71, operation_id};
}

ManagedOpeningDealObservation observation(std::uint64_t deal_ticket = 5001,
                                           std::uint64_t source_revision = 4,
                                           std::uint64_t login = 42,
                                           double volume = 0.05,
                                           std::string server = "demo") {
    return {operation(login, 9, std::move(server)), 3, 3001, deal_ticket, 88001,
            kManagedOpeningRequiredDealFields, BrokerPositionDirection::buy,
            BrokerDealEntry::in, 1500, volume, 0.01,
            ManagedOpeningQuantityDomain{2, 1, 1, 1000, 1000},
            source_revision, 99, source_revision, 1000, 2000};
}

ManagedOpeningDealFact fact(ManagedOpeningDealObservation candidate) {
    const ManagedOpeningExecutionRequest request{
        candidate.operation_key, OperationKind::open, candidate.operation_revision,
        candidate.order_ticket, candidate.position_identifier};
    const auto result = ManagedOpeningDealFact::from_observation(request, candidate);
    require(result.has_value(), "valid OPEN observation was rejected");
    return *result;
}

std::optional<ManagedOpeningDealFact> try_fact(
    ManagedOpeningDealObservation candidate) {
    const ManagedOpeningExecutionRequest request{
        candidate.operation_key, OperationKind::open, candidate.operation_revision,
        candidate.order_ticket, candidate.position_identifier};
    return ManagedOpeningDealFact::from_observation(request, candidate);
}

ManagedOpeningDealFact maximum_fact() {
    const auto one_byte_server = fact(observation(5002, 4, 42, 0.05, "x"));
    const auto maximum_server_length =
        kManagedOpeningMaxCanonicalBytes - (one_byte_server.canonical_bytes().size() - 1U);
    const auto maximum = try_fact(
        observation(5003, 4, 42, 0.05, std::string(maximum_server_length, 'x')));
    require(maximum.has_value() &&
                maximum->canonical_bytes().size() == kManagedOpeningMaxCanonicalBytes,
            "maximum canonical fact was rejected");
    return *maximum;
}

class TestLease final : public SingleWriterLease {
public:
    bool held = true;
    std::uint64_t token = 7;
    std::optional<std::uint64_t> expected_token;
    bool deny_capability = false;

    std::optional<std::uint64_t> held_fencing_token(
        const AccountKey &) const override {
        return held ? std::optional<std::uint64_t>(token) : std::nullopt;
    }

    bool with_fencing_capability(const AccountKey &, std::uint64_t candidate,
                                 const std::function<void()> &publication) const override {
        if (!held || deny_capability ||
            (expected_token && *expected_token != candidate))
            return false;
        publication();
        return true;
    }
};

std::vector<std::uint8_t> read_bytes(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_bytes(const std::filesystem::path &path,
                 const std::vector<std::uint8_t> &bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(output), "test fixture write failed");
}

std::filesystem::path only_record(const std::filesystem::path &directory) {
    for (const auto &entry : std::filesystem::recursive_directory_iterator(directory))
        if (entry.path().filename().wstring().rfind(L"open-", 0) == 0 &&
            entry.path().extension() == L".bin")
            return entry.path();
    throw std::runtime_error("fact record was not published");
}

std::vector<std::filesystem::path> record_paths(
    const std::filesystem::path &directory) {
    std::vector<std::filesystem::path> paths;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(directory))
        if (entry.path().filename().wstring().rfind(L"open-", 0) == 0 &&
            entry.path().extension() == L".bin")
            paths.push_back(entry.path());
    return paths;
}

void verifies_canonical_decoder() {
    const auto encoded = fact(observation()).canonical_bytes();
    const auto decoded = ManagedOpeningDealFact::from_canonical_bytes(encoded);
    require(decoded.has_value(), "canonical fact did not decode");
    require(decoded->canonical_bytes() == encoded,
            "canonical fact changed during decode/encode round trip");

    auto truncated = encoded;
    truncated.pop_back();
    require(!ManagedOpeningDealFact::from_canonical_bytes(truncated),
            "truncated canonical fact decoded");
    auto unknown_schema = encoded;
    const auto tag_size = std::string("mt5bridge-managed-opening-deal-fact").size();
    unknown_schema[sizeof(std::uint64_t) + tag_size] = 2;
    require(!ManagedOpeningDealFact::from_canonical_bytes(unknown_schema),
            "unknown canonical schema decoded");
    auto corrupted = encoded;
    corrupted[8] ^= 0x01U;
    require(!ManagedOpeningDealFact::from_canonical_bytes(corrupted),
            "corrupted canonical fact decoded");
    auto trailing = encoded;
    trailing.push_back(0);
    require(!ManagedOpeningDealFact::from_canonical_bytes(trailing),
            "canonical fact with trailing bytes decoded");

    auto oversized = encoded;
    oversized.resize(kManagedOpeningMaxCanonicalBytes + 1U, 0);
    require(!ManagedOpeningDealFact::from_canonical_bytes(oversized),
            "oversized canonical fact decoded");

    const auto maximum = maximum_fact();
    require(!try_fact(observation(5004, 4, 42, 0.05,
                                  std::string(maximum.operation_key().account.server.size() + 1U,
                                              'x'))),
            "canonical fact above the shared bound was accepted");
}

void verifies_maximum_durable_round_trip(const std::filesystem::path &directory) {
    const auto original = maximum_fact();
    TestLease lease;
    WindowsFileManagedOpeningFactStore store(directory);
    require(store.ready(), "maximum fact store did not open");
    require(store.commit(original, lease) == ManagedOpeningFactCommitStatus::committed,
            "maximum canonical fact was not committed");

    WindowsFileManagedOpeningFactStore restarted(directory);
    const auto recovered = restarted.load(original.operation_key().account,
                                          original.deal_ticket());
    require(recovered.found() && *recovered.fact == original &&
                recovered.fact->canonical_bytes() == original.canonical_bytes(),
            "maximum canonical fact did not survive durable restart");
}

void verifies_durable_replay_and_account_isolation(
    const std::filesystem::path &directory) {
    WindowsFileManagedOpeningFactStore store(directory);
    require(store.ready(), "fact store did not open");
    TestLease lease;

    const auto first = fact(observation());
    require(store.commit(first, lease) == ManagedOpeningFactCommitStatus::committed,
            "first fact was not committed");
    const auto loaded = store.load(account(), 5001);
    require(loaded.found() && *loaded.fact == first,
            "committed fact was not recovered exactly");

    const auto repoll = fact(observation(5001, 5));
    require(store.commit(repoll, lease) ==
                ManagedOpeningFactCommitStatus::already_committed,
            "repoll changed the durable receipt or created a duplicate");
    const auto after_repoll = store.load(account(), 5001);
    require(after_repoll.found() && *after_repoll.fact == first,
            "repoll rewrote the first admission receipt");

    require(store.commit(fact(observation(5001, 5, 42, 0.04)), lease) ==
                ManagedOpeningFactCommitStatus::conflict,
            "changed payload was accepted for an existing deal ticket");
    lease.deny_capability = true;
    require(store.commit(fact(observation(5002, 6)), lease) ==
                ManagedOpeningFactCommitStatus::lease_not_held,
            "publication was accepted after the fencing callback was denied");
    require(store.load(account(), 5002).status == ManagedOpeningFactLoadStatus::not_found,
            "denied fencing callback left a durable record");
    lease.deny_capability = false;
    lease.expected_token = 99;
    require(store.commit(fact(observation(5003, 7)), lease) ==
                ManagedOpeningFactCommitStatus::lease_not_held,
            "stale fencing token was accepted");
    require(store.load(account(), 5003).status == ManagedOpeningFactLoadStatus::not_found,
            "stale fencing token left a durable record");
    lease.expected_token.reset();

    const auto first_record = only_record(directory);
    const auto first_bytes = read_bytes(first_record);
    WindowsFileManagedOpeningFactStore concurrent_writer(directory);
    require(concurrent_writer.commit(first, lease) ==
                ManagedOpeningFactCommitStatus::already_committed,
            "second writer did not observe the committed immutable fact");
    require(store.load(account(), 9999).status ==
                ManagedOpeningFactLoadStatus::not_found,
            "missing fact was not reported as absent");

    const auto other_account = fact(observation(5001, 1, 2, 0.05, "demo4"));
    require(store.commit(other_account, lease) ==
                ManagedOpeningFactCommitStatus::committed,
            "same deal ticket in another account was not isolated");
    const auto other_record = [&] {
        for (const auto &path : record_paths(directory))
            if (path != first_record)
                return path;
        throw std::runtime_error("second fact record was not published");
    }();
    require(store.load(account("demo4", 2), 5001).found(),
            "isolated account fact was not recoverable");
    require(store.scan(account()).complete() && store.scan(account()).facts.size() == 1,
            "account scan included another account");
    require(store.scan(account("demo4", 2)).complete() &&
                store.scan(account("demo4", 2)).facts.size() == 1,
            "second account scan was incomplete");

    auto foreign_bytes = read_bytes(other_record);
    write_bytes(first_record, foreign_bytes);
    require(store.load(account(), 5001).status ==
                ManagedOpeningFactLoadStatus::invalid_record,
            "foreign fact under an expected filename was returned");
    require(store.scan(account()).status == ManagedOpeningFactScanStatus::invalid_record,
            "foreign fact under an expected account path was ignored");
    write_bytes(first_record, first_bytes);

    auto corrupted_other = foreign_bytes;
    corrupted_other.back() ^= 0x01U;
    write_bytes(other_record, corrupted_other);
    require(store.scan(account()).complete(),
            "corruption in another account blocked this account scan");
    require(store.scan(account("demo4", 2)).status ==
                ManagedOpeningFactScanStatus::invalid_record,
            "corruption in the requested account was ignored");
    write_bytes(other_record, foreign_bytes);

    auto corrupted_first = first_bytes;
    corrupted_first.back() ^= 0x01U;
    write_bytes(first_record, corrupted_first);
    require(store.scan(account()).status == ManagedOpeningFactScanStatus::invalid_record,
            "corruption in the requested account did not fail closed");
    write_bytes(first_record, first_bytes);

    WindowsFileManagedOpeningFactStore restarted(directory);
    require(restarted.commit(repoll, lease) ==
                ManagedOpeningFactCommitStatus::already_committed,
            "restart replay was not idempotent");

    lease.held = false;
    require(restarted.commit(fact(observation(5002, 6)), lease) ==
                ManagedOpeningFactCommitStatus::lease_not_held,
            "unfenced publication was accepted");
}

void rejects_corruption_and_ignores_incomplete_temp(
    const std::filesystem::path &directory) {
    WindowsFileManagedOpeningFactStore store(directory);
    TestLease lease;
    require(store.commit(fact(observation()), lease) ==
                ManagedOpeningFactCommitStatus::committed,
            "corruption fixture fact was not committed");
    const auto record = only_record(directory);
    const auto account_path = record.parent_path();
    // This is a deterministic incomplete-publication fixture, not a process-crash
    // simulation; the post-rename restart replay is covered separately.
    write_bytes(std::filesystem::path(record.wstring() + L".tmp"), {1, 2, 3});
    require(store.scan(account()).complete(),
            "incomplete temporary publication was treated as a record");
    write_bytes(account_path / L"unreserved.bin", {1, 2, 3});
    require(store.scan(account()).complete(),
            "unreserved filesystem entry blocked account scan");

    const auto reserved_directory = account_path / L"open-0123456789abcdef.bin";
    require(std::filesystem::create_directory(reserved_directory),
            "reserved directory fixture was not created");
    require(store.scan(account()).status == ManagedOpeningFactScanStatus::invalid_record,
            "reserved non-file entry was silently ignored");
    std::filesystem::remove(reserved_directory);

    const auto original = read_bytes(record);
    require(!original.empty(), "durable record fixture was empty");
    auto oversized_envelope = original;
    for (std::size_t index = 12; index != 20; ++index)
        oversized_envelope[index] = 0xffU;
    write_bytes(record, oversized_envelope);
    require(store.load(account(), 5001).status ==
                ManagedOpeningFactLoadStatus::invalid_record,
            "oversized durable envelope was accepted");

    auto unknown_schema = original;
    unknown_schema[8] = 2;
    write_bytes(record, unknown_schema);
    require(store.load(account(), 5001).status ==
                ManagedOpeningFactLoadStatus::invalid_record,
            "unknown durable schema was accepted");
    require(store.scan(account()).status == ManagedOpeningFactScanStatus::invalid_record,
            "scan recovered an unknown durable schema");

    auto corrupted = original;
    corrupted.back() ^= 0x01U;
    write_bytes(record, corrupted);
    require(store.load(account(), 5001).status ==
                ManagedOpeningFactLoadStatus::invalid_record,
            "corrupted durable record was accepted");

    auto truncated = original;
    truncated.resize(truncated.size() - 1);
    write_bytes(record, truncated);
    require(store.load(account(), 5001).status ==
                ManagedOpeningFactLoadStatus::invalid_record,
            "truncated durable record was accepted");
}

} // namespace

int main() {
#if !defined(_WIN32)
    std::cout << "managed opening fact store requires Windows\n";
    return 0;
#else
    try {
        verifies_canonical_decoder();
        const auto root = std::filesystem::temp_directory_path() /
                          ("mt5bridge-opening-fact-store-" +
                           std::to_string(std::chrono::steady_clock::now()
                                              .time_since_epoch()
                                              .count()));
        std::filesystem::create_directories(root);
        try {
            verifies_durable_replay_and_account_isolation(root / L"replay");
            rejects_corruption_and_ignores_incomplete_temp(root / L"corruption");
            verifies_maximum_durable_round_trip(root / L"maximum");
        } catch (...) {
            std::filesystem::remove_all(root);
            throw;
        }
        std::filesystem::remove_all(root);
        std::cout << "managed opening fact store tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
#endif
}
