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

/// \struct ManagedLogicalVolumeNormalization
/// \brief Decimal scale of managed logical units at a conversion boundary.
///
/// A managed quantity is interpreted as `units / 10^scale`. The scale is
/// conversion evidence, not an allocation policy and not a broker-volume
/// claim. A future owner-loop integration must source it from the durable
/// operation/symbol contract rather than inventing it for a candidate mapping.
struct ManagedLogicalVolumeNormalization {
    std::uint32_t scale = 0; ///< Decimal denominator exponent, at most nine.

    /// \brief Tests whether the logical quantity has a bounded decimal scale.
    /// \return True when the scale can be represented exactly.
    bool valid() const { return scale <= 9; }

    /// \brief Compares two logical-unit scale proofs.
    /// \param other Scale proof to compare.
    /// \return True when both use the same decimal denominator.
    bool operator==(const ManagedLogicalVolumeNormalization &other) const {
        return scale == other.scale;
    }
};

namespace detail {

/// \brief Returns 10 raised to a bounded decimal exponent.
/// \param exponent Decimal exponent no greater than nine.
/// \return Exact power, or empty for an unsupported exponent.
inline std::optional<std::uint64_t> managed_volume_power10(std::uint32_t exponent) {
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
/// \param logical_scale Decimal scale of the managed quantity.
/// \param broker_normalization Decimal scale and step proof for the broker.
/// \return Exact broker volume, or empty when conversion is not integral or
///         step-aligned.
inline std::optional<BrokerVolume> map_managed_logical_units_to_broker_volume(
    std::uint64_t managed_logical_units,
    const ManagedLogicalVolumeNormalization &logical_scale,
    const BrokerVolumeNormalization &broker_normalization) {
    if (managed_logical_units == 0 || !logical_scale.valid() ||
        !broker_normalization.valid())
        return std::nullopt;

    std::uint64_t broker_units = managed_logical_units;
    if (broker_normalization.scale >= logical_scale.scale) {
        const auto multiplier = detail::managed_volume_power10(
            broker_normalization.scale - logical_scale.scale);
        if (!multiplier || managed_logical_units >
                               (std::numeric_limits<std::uint64_t>::max)() /
                                   *multiplier)
            return std::nullopt;
        broker_units = managed_logical_units * *multiplier;
    } else {
        const auto divisor = detail::managed_volume_power10(
            logical_scale.scale - broker_normalization.scale);
        if (!divisor || managed_logical_units % *divisor != 0)
            return std::nullopt;
        broker_units = managed_logical_units / *divisor;
    }

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

    /// \brief Returns the managed logical scale proof.
    /// \return Decimal scale of the source quantity.
    const ManagedLogicalVolumeNormalization &logical_scale() const {
        return logical_scale_;
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
            !logical_scale_.valid() || managed_logical_units_ == 0 ||
            !broker_volume_.valid())
            return false;

        const BrokerVolumeNormalization broker_normalization{
            broker_volume_.scale, broker_volume_.step_units};
        const auto expected = map_managed_logical_units_to_broker_volume(
            managed_logical_units_, logical_scale_, broker_normalization);
        return expected && *expected == broker_volume_;
    }

    /// \brief Compares exact conversion-proof fields.
    /// \param other Mapping to compare.
    /// \return True when every field is identical.
    bool operator==(const ManagedBrokerVolumeMapping &other) const {
        return managed_key_ == other.managed_key_ && broker_key_ == other.broker_key_ &&
               managed_provenance_ == other.managed_provenance_ &&
               broker_provenance_ == other.broker_provenance_ &&
               logical_scale_ == other.logical_scale_ &&
               managed_logical_units_ == other.managed_logical_units_ &&
               broker_volume_ == other.broker_volume_;
    }

private:
    ManagedBrokerVolumeMapping(ManagedDealSettlementKey managed_key,
                               BrokerReversalKey broker_key,
                               ManagedSettlementProvenance managed_provenance,
                               BrokerReversalProvenance broker_provenance,
                               ManagedLogicalVolumeNormalization logical_scale,
                               std::uint64_t managed_logical_units,
                               BrokerVolume broker_volume)
        : managed_key_(std::move(managed_key)), broker_key_(std::move(broker_key)),
          managed_provenance_(std::move(managed_provenance)),
          broker_provenance_(std::move(broker_provenance)),
          logical_scale_(logical_scale), managed_logical_units_(managed_logical_units),
          broker_volume_(broker_volume) {}

    ManagedDealSettlementKey managed_key_;
    BrokerReversalKey broker_key_;
    ManagedSettlementProvenance managed_provenance_;
    BrokerReversalProvenance broker_provenance_;
    ManagedLogicalVolumeNormalization logical_scale_;
    std::uint64_t managed_logical_units_ = 0;
    BrokerVolume broker_volume_;

    friend std::optional<ManagedBrokerVolumeMapping>
    derive_managed_broker_volume_mapping(
        const ManagedDealSettlement &, const BrokerReversalRecord &,
        const ManagedLogicalVolumeNormalization &);
};

/// \brief Derives a conversion fact from two existing durable proofs.
/// \param settlement Immutable managed per-deal contribution.
/// \param broker Durable broker reversal proof for the same deal. Until a
///             generic durable deal proof exists, non-reversal deals are not
///             accepted as mapping evidence.
/// \param logical_scale Authoritative managed decimal-scale evidence.
/// \return An exact conversion fact, or empty when identity or arithmetic is
///         not provable. The broker scale and step are taken from the durable
///         broker deal proof, never from an independent caller argument.
inline std::optional<ManagedBrokerVolumeMapping> derive_managed_broker_volume_mapping(
    const ManagedDealSettlement &settlement, const BrokerReversalRecord &broker,
    const ManagedLogicalVolumeNormalization &logical_scale) {
    if (!settlement.valid() || !broker.valid() || !logical_scale.valid() ||
        settlement.key.account != broker.account ||
        settlement.key.deal_ticket != broker.deal_ticket)
        return std::nullopt;

    const BrokerVolumeNormalization broker_normalization{
        broker.deal_volume.scale, broker.deal_volume.step_units};
    const auto broker_volume = map_managed_logical_units_to_broker_volume(
        settlement.managed_logical_units, logical_scale, broker_normalization);
    if (!broker_volume)
        return std::nullopt;

    ManagedBrokerVolumeMapping result(
        settlement.key, broker.key(), settlement.provenance, broker.provenance,
        logical_scale, settlement.managed_logical_units, *broker_volume);
    return result.valid() ? std::optional<ManagedBrokerVolumeMapping>(result)
                          : std::nullopt;
}

} // namespace mt5bridge
