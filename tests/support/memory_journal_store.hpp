#pragma once

/// \file memory_journal_store.hpp
/// \brief Provides the deterministic in-memory journal fake shared by store tests.

#include <mt5bridge/dispatch/journal_store.hpp>

#include <cstdint>
#include <map>
#include <optional>

namespace mt5bridge_test_support {

/// \class MemoryJournalStore
/// \brief Stores operation records in memory using the durable journal seam.
class MemoryJournalStore final : public mt5bridge::DurableJournalStore {
public:
    /// \brief Commits a record into the fake journal.
    /// \param record Record to retain.
    /// \param expected_revision Optional compare-and-swap revision.
    /// \return Committed; this fixture does not simulate write failures.
    mt5bridge::StoreCommitStatus commit(
        const mt5bridge::OperationRecord &record,
        std::optional<std::uint64_t> expected_revision) override {
        if (expected_revision.has_value()) {
            const auto it = records.find(record.key);
            if (it == records.end() || it->second.revision != *expected_revision)
                return mt5bridge::StoreCommitStatus::conflict;
        } else if (records.find(record.key) != records.end()) {
            return mt5bridge::StoreCommitStatus::conflict;
        }
        records[record.key] = record;
        return mt5bridge::StoreCommitStatus::committed;
    }

    /// \brief Loads a record by its operation key.
    /// \param key Operation identity.
    /// \return The stored record or not-found.
    mt5bridge::StoreLoadResult load(
        const mt5bridge::OperationKey &key) const override {
        if (load_status != mt5bridge::StoreLoadStatus::found)
            return {load_status, std::nullopt};
        const auto it = records.find(key);
        return it == records.end()
                   ? mt5bridge::StoreLoadResult{mt5bridge::StoreLoadStatus::not_found,
                                                std::nullopt}
                   : mt5bridge::StoreLoadResult{mt5bridge::StoreLoadStatus::found,
                                                it->second};
    }

    /// \brief Enumerates all retained records.
    /// \return Complete in-memory snapshot.
    mt5bridge::StoreScanResult scan() const override {
        mt5bridge::StoreScanResult result;
        result.status = mt5bridge::StoreScanStatus::complete;
        for (const auto &entry : records)
            result.records.push_back(entry.second);
        return result;
    }

    mt5bridge::StoreLoadStatus load_status = mt5bridge::StoreLoadStatus::found;
    std::map<mt5bridge::OperationKey, mt5bridge::OperationRecord> records;
};

} // namespace mt5bridge_test_support
