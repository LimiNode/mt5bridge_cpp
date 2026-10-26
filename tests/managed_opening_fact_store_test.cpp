/// \file managed_opening_fact_store_test.cpp
/// \brief Verifies durable OPEN fact decoding, replay, fencing, and recovery.

#include "managed_opening_fact_store.hpp"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using namespace mt5bridge;
using namespace mt5bridge::dispatch;

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

AccountKey account(std::uint64_t login = 42) { return {"demo", login}; }

OperationKey operation(std::uint64_t login = 42, std::uint64_t operation_id = 9) {
    return {account(login), 71, operation_id};
}

ManagedOpeningDealObservation observation(std::uint64_t deal_ticket = 5001,
                                           std::uint64_t source_revision = 4,
                                           std::uint64_t login = 42,
                                           double volume = 0.05) {
    return {operation(login), 3, 3001, deal_ticket, 88001,
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

class TestLease final : public SingleWriterLease {
public:
    bool held = true;
    std::uint64_t token = 7;

    std::optional<std::uint64_t> held_fencing_token(
        const AccountKey &) const override {
        return held ? std::optional<std::uint64_t>(token) : std::nullopt;
    }

    bool with_fencing_capability(const AccountKey &, std::uint64_t,
                                 const std::function<void()> &publication) const override {
        if (!held)
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
    for (const auto &entry : std::filesystem::directory_iterator(directory))
        if (entry.path().filename().wstring().rfind(L"open-", 0) == 0 &&
            entry.path().extension() == L".bin")
            return entry.path();
    throw std::runtime_error("fact record was not published");
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
    WindowsFileManagedOpeningFactStore concurrent_writer(directory);
    require(concurrent_writer.commit(first, lease) ==
                ManagedOpeningFactCommitStatus::already_committed,
            "second writer did not observe the committed immutable fact");
    require(store.load(account(), 9999).status ==
                ManagedOpeningFactLoadStatus::not_found,
            "missing fact was not reported as absent");

    const auto other_account = fact(observation(5001, 1, 43));
    require(store.commit(other_account, lease) ==
                ManagedOpeningFactCommitStatus::committed,
            "same deal ticket in another account was not isolated");
    require(store.load(account(43), 5001).found(),
            "isolated account fact was not recoverable");
    require(store.scan(account()).complete() && store.scan(account()).facts.size() == 1,
            "account scan included another account");
    require(store.scan(account(43)).complete() && store.scan(account(43)).facts.size() == 1,
            "second account scan was incomplete");

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
    write_bytes(std::filesystem::path(record.wstring() + L".tmp"), {1, 2, 3});
    require(store.scan(account()).complete(),
            "incomplete temporary publication was treated as a record");

    const auto original = read_bytes(record);
    require(!original.empty(), "durable record fixture was empty");
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
