#pragma once

/// \file dispatch/managed_broker_volume_mapping.hpp
/// \brief Defines exact managed-logical to broker-volume conversion proofs.

#include "broker_reversal.hpp"
#include "broker_reversal_reconciliation.hpp"
#include "managed_deal_settlement.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

namespace mt5bridge {

namespace detail {

/// \brief Returns 10 raised to a bounded broker-volume exponent.
/// \param exponent Decimal exponent no greater than nine.
/// \return Exact power, or empty for an unsupported exponent.
inline std::optional<std::uint64_t> broker_volume_power10(std::uint32_t exponent) {
    if (exponent > 9)
        return std::nullopt;
    std::uint64_t result = 1;
    for (std::uint32_t index = 0; index != exponent; ++index)
        result *= 10;
    return result;
}

} // namespace detail

/// \brief Converts managed logical units into exact broker step units.
/// \param managed_logical_units Positive managed quantity numerator.
/// \param broker_normalization Decimal scale and step proof for the broker.
/// \return Exact broker volume, or empty when conversion overflows or is not
///         step-aligned.
inline std::optional<BrokerVolume> map_managed_logical_units_to_broker_volume(
    std::uint64_t managed_logical_units,
    const BrokerVolumeNormalization &broker_normalization) {
    if (managed_logical_units == 0 || !broker_normalization.valid())
        return std::nullopt;

    const auto multiplier =
        detail::broker_volume_power10(broker_normalization.scale);
    if (!multiplier || managed_logical_units >
                           (std::numeric_limits<std::uint64_t>::max)() /
                               *multiplier)
        return std::nullopt;
    const auto broker_units = managed_logical_units * *multiplier;

    BrokerVolume result{broker_units, broker_normalization.scale,
                        broker_normalization.step_units};
    return result.valid() ? std::optional<BrokerVolume>(result) : std::nullopt;
}

/// \class ManagedBrokerVolumeMapping
/// \brief Exact conversion fact bound to one managed deal and broker deal.
///
/// This value proves only a decimal-domain conversion. It does not claim that
/// the mapped quantity owns a broker leg, consume an allocation envelope, or
/// mutate managed exposure. Those decisions remain a later allocation-layer
/// responsibility.
class ManagedBrokerVolumeMapping {
public:
    /// \brief Returns the immutable managed deal identity.
    /// \return Operation/deal composite key.
    const ManagedDealSettlementKey &managed_key() const { return managed_key_; }

    /// \brief Returns the immutable broker deal identity.
    /// \return Account/deal ticket key.
    const BrokerReversalKey &broker_key() const { return broker_key_; }

    /// \brief Returns the managed settlement provenance.
    /// \return Source proof identity.
    const ManagedSettlementProvenance &managed_provenance() const {
        return managed_provenance_;
    }

    /// \brief Returns the broker observation provenance.
    /// \return Broker proof identity.
    const BrokerReversalProvenance &broker_provenance() const {
        return broker_provenance_;
    }

    /// \brief Returns the source managed quantity numerator.
    /// \return Positive logical units.
    std::uint64_t managed_logical_units() const { return managed_logical_units_; }

    /// \brief Returns the converted broker quantity.
    /// \return Exact step-normalized volume.
    const BrokerVolume &broker_volume() const { return broker_volume_; }

    /// \brief Tests identity, provenance, and conversion invariants.
    /// \return True only for a self-consistent exact mapping.
    bool valid() const {
        if (!managed_key_.valid() || !broker_key_.valid() ||
            managed_key_.account != broker_key_.account ||
            managed_key_.deal_ticket != broker_key_.deal_ticket ||
            !managed_provenance_.valid() || !broker_provenance_.valid() ||
            managed_logical_units_ == 0 ||
            !broker_volume_.valid())
            return false;

        const BrokerVolumeNormalization broker_normalization{
            broker_volume_.scale, broker_volume_.step_units};
        const auto expected = map_managed_logical_units_to_broker_volume(
            managed_logical_units_, broker_normalization);
        return expected && *expected == broker_volume_;
    }

    /// \brief Compares exact conversion-proof fields.
    /// \param other Mapping to compare.
    /// \return True when every field is identical.
    bool operator==(const ManagedBrokerVolumeMapping &other) const {
        return managed_key_ == other.managed_key_ && broker_key_ == other.broker_key_ &&
               managed_provenance_ == other.managed_provenance_ &&
               broker_provenance_ == other.broker_provenance_ &&
               managed_logical_units_ == other.managed_logical_units_ &&
               broker_volume_ == other.broker_volume_;
    }

private:
    ManagedBrokerVolumeMapping(ManagedDealSettlementKey managed_key,
                               BrokerReversalKey broker_key,
                               ManagedSettlementProvenance managed_provenance,
                               BrokerReversalProvenance broker_provenance,
                               std::uint64_t managed_logical_units,
                               BrokerVolume broker_volume)
        : managed_key_(std::move(managed_key)), broker_key_(std::move(broker_key)),
          managed_provenance_(std::move(managed_provenance)),
          broker_provenance_(std::move(broker_provenance)),
          managed_logical_units_(managed_logical_units),
          broker_volume_(broker_volume) {}

    ManagedDealSettlementKey managed_key_;
    BrokerReversalKey broker_key_;
    ManagedSettlementProvenance managed_provenance_;
    BrokerReversalProvenance broker_provenance_;
    std::uint64_t managed_logical_units_ = 0;
    BrokerVolume broker_volume_;

    friend std::optional<ManagedBrokerVolumeMapping>
    derive_managed_broker_volume_mapping(
        const ManagedDealSettlement &, const BrokerReversalRecord &);
};

/// \brief Derives a conversion fact from two existing durable proofs.
/// \param settlement Immutable managed per-deal contribution.
/// \param broker Durable broker reversal proof for the same deal. Until a
///             generic durable deal proof exists, non-reversal deals are not
///             accepted as mapping evidence.
/// \return An exact conversion fact, or empty when identity or arithmetic is
///         not provable. Managed logical units are the integer domain defined
///         by ADR-0024; the broker scale and step are taken from the durable
///         broker deal proof.
inline std::optional<ManagedBrokerVolumeMapping> derive_managed_broker_volume_mapping(
    const ManagedDealSettlement &settlement, const BrokerReversalRecord &broker) {
    if (!settlement.valid() || !broker.valid() ||
        settlement.key.account != broker.account ||
        settlement.key.deal_ticket != broker.deal_ticket)
        return std::nullopt;

    const BrokerVolumeNormalization broker_normalization{
        broker.deal_volume.scale, broker.deal_volume.step_units};
    const auto broker_volume = map_managed_logical_units_to_broker_volume(
        settlement.managed_logical_units, broker_normalization);
    if (!broker_volume)
        return std::nullopt;

    ManagedBrokerVolumeMapping result(
        settlement.key, broker.key(), settlement.provenance, broker.provenance,
        settlement.managed_logical_units, *broker_volume);
    return result.valid() ? std::optional<ManagedBrokerVolumeMapping>(result)
                          : std::nullopt;
}

} // namespace mt5bridge
