#pragma once

/// \file dispatch/managed_deal_settlement.hpp
/// \brief Defines immutable per-deal managed settlement facts and frontiers.

#include "journal_store.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace mt5bridge {

namespace dispatch {
class ManagedDealSettlementProducer;
}

/// \struct ManagedSettlementProvenance
/// \brief Identifies the observation proof that established one settlement fact.
struct ManagedSettlementProvenance {
    std::uint64_t graph_instance_id = 0; ///< Process-local observation graph identity.
    std::uint64_t graph_revision = 0; ///< Graph revision evaluated by reconciliation.
    std::uint64_t history_deals_revision = 0; ///< Revision containing deal evidence.
    std::uint64_t evidence_digest = 0; ///< Digest of the bound deal observation.

    /// \brief Tests whether all observation identity fields are usable.
    /// \return True for a complete, ordered provenance identity.
    bool valid() const {
        return graph_instance_id != 0 && graph_revision != 0 &&
               history_deals_revision != 0 && history_deals_revision <= graph_revision &&
               evidence_digest != 0;
    }

    /// \brief Compares two provenance identities.
    /// \param other Provenance to compare.
    /// \return True when every field matches.
    bool operator==(const ManagedSettlementProvenance &other) const {
        return graph_instance_id == other.graph_instance_id &&
               graph_revision == other.graph_revision &&
               history_deals_revision == other.history_deals_revision &&
               evidence_digest == other.evidence_digest;
    }
};

/// \struct ManagedDealSettlementKey
/// \brief Composite identity for one operation's one broker deal.
struct ManagedDealSettlementKey {
    AccountKey account; ///< Account scope shared by operation and deal evidence.
    OperationKey operation_key; ///< Managed operation receiving the contribution.
    std::uint64_t deal_ticket = 0; ///< MT5 DEAL_TICKET.

    /// \brief Tests whether the composite identity is internally consistent.
    /// \return True only for valid account, operation, and deal identities.
    bool valid() const {
        return account.valid() && operation_key.valid() && account == operation_key.account &&
               deal_ticket != 0;
    }

    /// \brief Compares two composite identities.
    /// \param other Identity to compare.
    /// \return True when all components match.
    bool operator==(const ManagedDealSettlementKey &other) const {
        return account == other.account && operation_key == other.operation_key &&
               deal_ticket == other.deal_ticket;
    }

    /// \brief Orders identities for deterministic durable scans.
    /// \param other Identity to compare.
    /// \return True when this identity sorts before `other`.
    bool operator<(const ManagedDealSettlementKey &other) const {
        if (!(account == other.account)) {
            if (account.server != other.account.server)
                return account.server < other.account.server;
            if (account.login != other.account.login)
                return account.login < other.account.login;
        }
        if (!(operation_key == other.operation_key))
            return operation_key < other.operation_key;
        return deal_ticket < other.deal_ticket;
    }
};

/// \struct ManagedDealSettlement
/// \brief Immutable logical contribution of one broker deal to one operation.
struct ManagedDealSettlement {
    ManagedDealSettlementKey key; ///< Operation/deal composite identity.
    std::uint64_t source_operation_revision = 0; ///< Revision proving this contribution.
    std::uint64_t managed_logical_units = 0; ///< Exact managed logical contribution.
    ManagedSettlementProvenance provenance; ///< Observation proof for this deal.

    /// \brief Tests local identity and immutable contribution invariants.
    /// \return True only for a positive, provenance-bearing fact.
    bool valid() const {
        return key.valid() && source_operation_revision != 0 &&
               managed_logical_units != 0 && provenance.valid();
    }

    /// \brief Compares all durable fact fields.
    /// \param other Fact to compare.
    /// \return True when exact replay is possible.
    bool operator==(const ManagedDealSettlement &other) const {
        return key == other.key &&
               source_operation_revision == other.source_operation_revision &&
               managed_logical_units == other.managed_logical_units &&
               provenance == other.provenance;
    }
};

/// \struct ManagedSettlementFrontierEntry
/// \brief Pins one immutable per-deal fact into a cumulative frontier.
struct ManagedSettlementFrontierEntry {
    std::uint64_t deal_ticket = 0; ///< Referenced deal identity.
    std::uint64_t source_operation_revision = 0; ///< Fact revision.
    std::uint64_t managed_logical_units = 0; ///< Fact units copied for exact replay.

    /// \brief Tests local entry fields.
    /// \return True for a usable fact reference.
    bool valid() const {
        return deal_ticket != 0 && source_operation_revision != 0 &&
               managed_logical_units != 0;
    }

    /// \brief Compares exact entry references.
    /// \param other Entry to compare.
    /// \return True when all fields match.
    bool operator==(const ManagedSettlementFrontierEntry &other) const {
        return deal_ticket == other.deal_ticket &&
               source_operation_revision == other.source_operation_revision &&
               managed_logical_units == other.managed_logical_units;
    }
};

/// \struct ManagedSettlementFrontier
/// \brief Complete cumulative decomposition at one operation revision.
struct ManagedSettlementFrontier {
    AccountKey account; ///< Account scope of the operation.
    OperationKey operation_key; ///< Managed operation being decomposed.
    std::uint64_t operation_revision = 0; ///< Exact cumulative operation revision.
    std::uint64_t settled_volume = 0; ///< Cumulative logical frontier value.
    ManagedSettlementProvenance provenance; ///< Proof of frontier completeness.
    std::vector<ManagedSettlementFrontierEntry> entries; ///< Complete immutable fact set.

    /// \brief Tests local frontier shape and overflow-safe entry sum.
    /// \return True only for a non-empty, provenance-bearing frontier.
    bool valid() const {
        if (!account.valid() || !operation_key.valid() || account != operation_key.account ||
            operation_revision == 0 || settled_volume == 0 || !provenance.valid() ||
            entries.empty())
            return false;
        std::uint64_t total = 0;
        for (std::size_t index = 0; index != entries.size(); ++index) {
            if (!entries[index].valid() || entries[index].source_operation_revision >
                                                    operation_revision ||
                total > (std::numeric_limits<std::uint64_t>::max)() -
                            entries[index].managed_logical_units)
                return false;
            for (std::size_t prior = 0; prior < index; ++prior) {
                if (entries[prior].deal_ticket == entries[index].deal_ticket)
                    return false;
            }
            total += entries[index].managed_logical_units;
        }
        return total == settled_volume;
    }

    /// \brief Compares exact durable frontier fields.
    /// \param other Frontier to compare.
    /// \return True when all fields and entry order match.
    bool operator==(const ManagedSettlementFrontier &other) const {
        return account == other.account && operation_key == other.operation_key &&
               operation_revision == other.operation_revision &&
               settled_volume == other.settled_volume && provenance == other.provenance &&
               entries == other.entries;
    }
};

/// \class ManagedDealSettlementCommit
/// \brief Sealed producer output accepted by the durable settlement store.
class ManagedDealSettlementCommit {
public:
    /// \brief Returns the immutable per-deal fact carried by this proof.
    /// \return Producer-derived fact.
    const ManagedDealSettlement &fact() const { return fact_; }

    /// \brief Returns the complete frontier carried by this proof.
    /// \return Producer-derived frontier.
    const ManagedSettlementFrontier &frontier() const { return frontier_; }

private:
    ManagedDealSettlementCommit(ManagedDealSettlement fact,
                                ManagedSettlementFrontier frontier)
        : fact_(std::move(fact)), frontier_(std::move(frontier)) {}

    ManagedDealSettlement fact_;
    ManagedSettlementFrontier frontier_;
    friend class dispatch::ManagedDealSettlementProducer;
};

/// \enum ManagedDealSettlementCommitStatus
/// \brief Reports proof-gated fact/frontier persistence.
enum class ManagedDealSettlementCommitStatus {
    committed, ///< New fact and frontier were durably written.
    already_committed, ///< Exact fact/frontier replay was accepted.
    conflict, ///< Immutable fact or frontier identity has different content.
    invalid_record, ///< Candidate fact or frontier failed local validation.
    missing_operation_record, ///< The source operation is not durable.
    invalid_operation_record, ///< The source operation cannot authorize settlement.
    missing_fact, ///< A frontier references a fact not present in the store.
    io_error, ///< Storage or source-store I/O failed.
};

/// \enum ManagedDealSettlementLoadStatus
/// \brief Reports lookup state for one immutable settlement fact.
enum class ManagedDealSettlementLoadStatus { found, not_found, invalid_record, io_error };

/// \struct ManagedDealSettlementLoadResult
/// \brief Carries a status-bearing fact lookup result.
struct ManagedDealSettlementLoadResult {
    ManagedDealSettlementLoadStatus status = ManagedDealSettlementLoadStatus::io_error;
    std::optional<ManagedDealSettlement> record;

    /// \brief Tests whether a valid fact was loaded.
    /// \return True only for `found` with a record.
    bool found() const {
        return status == ManagedDealSettlementLoadStatus::found && record.has_value();
    }
};

/// \class DurableManagedDealSettlementStore
/// \brief Durable seam for immutable per-deal facts and cumulative frontiers.
class DurableManagedDealSettlementStore {
public:
    virtual ~DurableManagedDealSettlementStore() = default;

    /// \brief Commits one fact and its complete frontier under the store lock.
    /// \param proof Sealed output of the authoritative private producer.
    /// \param journal_store Durable source of operation lifecycle state.
    /// \return Proof, replay, conflict, or I/O status.
    virtual ManagedDealSettlementCommitStatus commit(
        const ManagedDealSettlementCommit &proof,
        const DurableJournalStore &journal_store) = 0;

    /// \brief Loads one immutable fact.
    /// \param key Composite operation/deal identity.
    /// \return Status-bearing fact result.
    virtual ManagedDealSettlementLoadResult load(
        const ManagedDealSettlementKey &key) const = 0;
};

} // namespace mt5bridge
