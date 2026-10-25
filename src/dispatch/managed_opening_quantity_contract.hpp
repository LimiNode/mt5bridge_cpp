#pragma once

/// \file dispatch/managed_opening_quantity_contract.hpp
/// \brief Defines the private exact quantity-domain contract for C3A B2.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>

namespace mt5bridge::dispatch {

namespace detail {

/// \brief Fixed-width unsigned integer used for exact binary/decimal comparisons.
class OpeningQuantityBigUnsigned {
public:
    /// \brief Number of 32-bit limbs in the checked comparison domain.
    static constexpr std::size_t k_limb_count = 40;

    /// \brief Tests whether the value is zero.
    /// \return True when every limb is zero.
    bool zero() const {
        for (const auto limb : limbs_)
            if (limb != 0)
                return false;
        return true;
    }

    /// \brief Builds a value from one machine-word integer.
    /// \param value Source value.
    /// \return Fixed-width representation.
    static OpeningQuantityBigUnsigned from_u64(std::uint64_t value) {
        OpeningQuantityBigUnsigned result;
        result.limbs_[0] = static_cast<std::uint32_t>(value);
        result.limbs_[1] = static_cast<std::uint32_t>(value >> 32);
        return result;
    }

    /// \brief Adds another fixed-width value with checked capacity.
    /// \param other Value to add.
    /// \return False when the fixed capacity would overflow.
    bool add(const OpeningQuantityBigUnsigned &other) {
        std::uint64_t carry = 0;
        for (std::size_t index = 0; index != limbs_.size(); ++index) {
            const std::uint64_t sum = static_cast<std::uint64_t>(limbs_[index]) +
                                      other.limbs_[index] + carry;
            limbs_[index] = static_cast<std::uint32_t>(sum);
            carry = sum >> 32;
        }
        return carry == 0;
    }

    /// \brief Multiplies by a small positive integer with checked capacity.
    /// \param multiplier Multiplier no greater than 32 bits.
    /// \return False when the fixed capacity would overflow.
    bool multiply_small(std::uint32_t multiplier) {
        std::uint64_t carry = 0;
        for (auto &limb : limbs_) {
            const std::uint64_t product =
                static_cast<std::uint64_t>(limb) * multiplier + carry;
            limb = static_cast<std::uint32_t>(product);
            carry = product >> 32;
        }
        return carry == 0;
    }

    /// \brief Shifts left by an exact number of bits with checked capacity.
    /// \param bits Shift distance.
    /// \return False when non-zero bits leave the fixed capacity.
    bool shift_left(std::size_t bits) {
        constexpr std::size_t bits_per_limb = 32;
        if (bits == 0)
            return true;
        if (bits >= limbs_.size() * bits_per_limb) {
            const bool fits = zero();
            limbs_.fill(0);
            return fits;
        }

        const std::size_t whole = bits / bits_per_limb;
        const std::size_t partial = bits % bits_per_limb;
        std::array<std::uint32_t, k_limb_count> shifted{};
        std::uint64_t carry = 0;
        bool fits = true;
        for (std::size_t index = limbs_.size() - whole;
             index != limbs_.size(); ++index)
            if (limbs_[index] != 0)
                fits = false;
        for (std::size_t index = 0; index != limbs_.size() - whole; ++index) {
            const std::uint64_t value =
                (static_cast<std::uint64_t>(limbs_[index]) << partial) | carry;
            const std::size_t destination = index + whole;
            shifted[destination] = static_cast<std::uint32_t>(value);
            carry = value >> bits_per_limb;
        }
        if (carry != 0)
            fits = false;
        limbs_ = shifted;
        return fits;
    }

    /// \brief Compares two fixed-width unsigned values.
    /// \param other Value to compare.
    /// \return Negative, zero, or positive according to ordering.
    int compare(const OpeningQuantityBigUnsigned &other) const {
        for (std::size_t index = limbs_.size(); index != 0; --index) {
            if (limbs_[index - 1] != other.limbs_[index - 1])
                return limbs_[index - 1] < other.limbs_[index - 1] ? -1 : 1;
        }
        return 0;
    }

private:
    std::array<std::uint32_t, k_limb_count> limbs_{};
};

/// \struct OpeningQuantityBinaryRational
/// \brief Represents a finite non-negative binary floating-point value exactly.
struct OpeningQuantityBinaryRational {
    OpeningQuantityBigUnsigned mantissa;
    int exponent = 0;
};

/// \brief Decodes a positive finite IEEE-754 binary64 value without rounding.
/// \param value Value to decode.
/// \return Exact mantissa and power-of-two exponent.
inline OpeningQuantityBinaryRational decode_opening_quantity_double(double value) {
    std::uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const std::uint64_t fraction = bits & ((std::uint64_t{1} << 52) - 1);
    const std::uint32_t exponent =
        static_cast<std::uint32_t>((bits >> 52) & 0x7ffU);
    if (exponent == 0)
        return {OpeningQuantityBigUnsigned::from_u64(fraction), -1074};
    return {OpeningQuantityBigUnsigned::from_u64(
                (std::uint64_t{1} << 52) | fraction),
            static_cast<int>(exponent) - 1023 - 52};
}

/// \brief Computes the exact midpoint between two binary floating values.
/// \param left First endpoint.
/// \param right Second endpoint.
/// \return Exact midpoint rational.
inline OpeningQuantityBinaryRational midpoint_opening_quantity(
    const OpeningQuantityBinaryRational &left,
    const OpeningQuantityBinaryRational &right) {
    const int exponent = left.exponent < right.exponent ? left.exponent
                                                         : right.exponent;
    auto left_mantissa = left.mantissa;
    auto right_mantissa = right.mantissa;
    const bool left_fits = left_mantissa.shift_left(
        static_cast<std::size_t>(left.exponent - exponent));
    const bool right_fits = right_mantissa.shift_left(
        static_cast<std::size_t>(right.exponent - exponent));
    if (!left_fits || !right_fits)
        return {OpeningQuantityBigUnsigned{}, std::numeric_limits<int>::max()};
    if (!left_mantissa.add(right_mantissa))
        return {OpeningQuantityBigUnsigned{}, std::numeric_limits<int>::max()};
    return {left_mantissa, exponent - 1};
}

/// \struct OpeningQuantityRoundingInterval
/// \brief Exact open interval that rounds to one finite binary64 value.
struct OpeningQuantityRoundingInterval {
    OpeningQuantityBinaryRational lower;
    OpeningQuantityBinaryRational upper;
};

/// \brief Builds the exact rounding interval for a positive finite double.
/// \param raw MT5 binary volume.
/// \return Open midpoint interval, or empty for an unbounded/non-finite value.
inline std::optional<OpeningQuantityRoundingInterval>
opening_quantity_rounding_interval(double raw) {
    if (!std::isfinite(raw) || raw <= 0.0)
        return std::nullopt;
    const double upper_value = std::nextafter(raw, std::numeric_limits<double>::infinity());
    if (!std::isfinite(upper_value))
        return std::nullopt;
    const auto value = decode_opening_quantity_double(raw);
    const auto lower = decode_opening_quantity_double(std::nextafter(raw, 0.0));
    const auto upper = decode_opening_quantity_double(upper_value);
    const auto lower_midpoint = midpoint_opening_quantity(lower, value);
    const auto upper_midpoint = midpoint_opening_quantity(value, upper);
    if (lower_midpoint.exponent == std::numeric_limits<int>::max() ||
        upper_midpoint.exponent == std::numeric_limits<int>::max())
        return std::nullopt;
    return OpeningQuantityRoundingInterval{lower_midpoint, upper_midpoint};
}

/// \brief Compares a canonical decimal integer with a binary rational.
/// \param units Decimal numerator.
/// \param scale Decimal denominator exponent.
/// \param value Binary rational.
/// \return Negative, zero, or positive according to ordering.
inline int compare_opening_quantity_decimal_to_binary(
    std::uint64_t units, std::uint32_t scale,
    const OpeningQuantityBinaryRational &value) {
    auto decimal = OpeningQuantityBigUnsigned::from_u64(units);
    auto binary = value.mantissa;
    if (value.exponent >= 0) {
        for (std::uint32_t index = 0; index != scale; ++index)
            if (!decimal.multiply_small(10))
                return 1;
        if (!binary.shift_left(static_cast<std::size_t>(value.exponent)))
            return -1;
    } else {
        if (!decimal.shift_left(static_cast<std::size_t>(-value.exponent)))
            return 1;
        for (std::uint32_t index = 0; index != scale; ++index)
            if (!binary.multiply_small(10))
                return -1;
    }
    return decimal.compare(binary);
}

/// \brief Tests whether one canonical decimal value is uniquely represented.
/// \param raw MT5 binary volume.
/// \param units Expected decimal numerator.
/// \param scale Decimal denominator exponent.
/// \return True only when the expected value is strictly inside the rounding
///         interval and neither adjacent decimal value is also inside it.
inline bool opening_quantity_decimal_is_unique(double raw, std::uint64_t units,
                                               std::uint32_t scale) {
    if (units == 0 || scale > 9)
        return false;
    const auto interval = opening_quantity_rounding_interval(raw);
    if (!interval)
        return false;

    const auto strictly_inside = [&](std::uint64_t candidate) {
        return compare_opening_quantity_decimal_to_binary(
                   candidate, scale, interval->lower) > 0 &&
               compare_opening_quantity_decimal_to_binary(
                   candidate, scale, interval->upper) < 0;
    };
    if (!strictly_inside(units))
        return false;
    if (units > 1 && strictly_inside(units - 1))
        return false;
    return units == (std::numeric_limits<std::uint64_t>::max)() ||
           !strictly_inside(units + 1);
}

/// \brief Compares one broker-step multiple with an exact binary boundary.
/// \param step_count Integer multiple to compare.
/// \param step_units Canonical broker step numerator.
/// \param scale Canonical decimal scale.
/// \param boundary Rounding-interval boundary.
/// \return Negative, zero, or positive according to ordering.
inline int compare_opening_quantity_step_to_boundary(
    std::uint64_t step_count, std::uint64_t step_units, std::uint32_t scale,
    const OpeningQuantityBinaryRational &boundary) {
    if (step_count != 0 &&
        step_units > (std::numeric_limits<std::uint64_t>::max)() / step_count)
        return 1;
    return compare_opening_quantity_decimal_to_binary(
        step_count * step_units, scale, boundary);
}

/// \brief Finds the first step strictly above a rounding boundary.
/// \param boundary Exact lower rounding boundary.
/// \param step_units Canonical broker step numerator.
/// \param scale Canonical decimal scale.
/// \param max_step_count Bounded search capacity.
/// \return First matching step count, or empty when none is representable.
inline std::optional<std::uint64_t> first_opening_quantity_step_above(
    const OpeningQuantityBinaryRational &boundary, std::uint64_t step_units,
    std::uint32_t scale, std::uint64_t max_step_count) {
    std::uint64_t low = 1;
    std::uint64_t high = max_step_count;
    std::optional<std::uint64_t> result;
    while (low <= high) {
        const std::uint64_t middle = low + (high - low) / 2;
        if (compare_opening_quantity_step_to_boundary(
                middle, step_units, scale, boundary) > 0) {
            result = middle;
            if (middle == 0)
                break;
            high = middle - 1;
        } else {
            if (middle == (std::numeric_limits<std::uint64_t>::max)())
                break;
            low = middle + 1;
        }
    }
    return result;
}

} // namespace detail

/// \struct ManagedOpeningQuantityDomain
/// \brief Reviewed symbol/account mapping from broker steps to logical units.
struct ManagedOpeningQuantityDomain {
    std::uint32_t canonical_scale = 0; ///< Decimal denominator exponent.
    std::uint64_t broker_step_units = 0; ///< Positive broker step numerator.
    std::uint64_t managed_logical_units_per_step = 0; ///< Explicit logical map.
    std::uint64_t max_step_count =
        (std::numeric_limits<std::uint64_t>::max)(); ///< Bounded capacity.
    std::uint64_t max_managed_logical_units =
        (std::numeric_limits<std::uint64_t>::max)(); ///< Logical capacity.

    /// \brief Tests whether the reviewed mapping is representable.
    /// \return True only for positive, bounded mapping fields.
    bool valid() const {
        return canonical_scale <= 9 && broker_step_units != 0 &&
               managed_logical_units_per_step != 0 && max_step_count != 0 &&
               max_managed_logical_units != 0;
    }
};

/// \class ManagedOpeningQuantity
/// \brief Sealed broker and managed quantity from one MT5 observation.
class ManagedOpeningQuantity {
public:
    /// \brief Returns the canonical decimal scale.
    /// \return Decimal denominator exponent.
    std::uint32_t canonical_scale() const { return canonical_scale_; }

    /// \brief Returns the canonical broker step numerator.
    /// \return Positive broker step units.
    std::uint64_t broker_step_units() const { return broker_step_units_; }

    /// \brief Returns the canonical broker quantity numerator.
    /// \return Positive broker units.
    std::uint64_t broker_units() const { return broker_units_; }

    /// \brief Returns the exact integer number of broker steps.
    /// \return Positive step count.
    std::uint64_t integer_step_count() const { return integer_step_count_; }

    /// \brief Returns the explicit managed logical quantity.
    /// \return Positive managed logical units.
    std::uint64_t managed_logical_units() const {
        return managed_logical_units_;
    }

    /// \brief Tests exact quantity and mapping invariants.
    /// \return True only for a self-consistent positive quantity.
    bool valid() const {
        return canonical_scale_ <= 9 && broker_step_units_ != 0 &&
               broker_units_ != 0 && integer_step_count_ != 0 &&
               managed_logical_units_ != 0 &&
               broker_units_ % broker_step_units_ == 0 &&
               broker_units_ / broker_step_units_ == integer_step_count_;
    }

private:
    ManagedOpeningQuantity(std::uint32_t canonical_scale,
                           std::uint64_t broker_step_units,
                           std::uint64_t broker_units,
                           std::uint64_t integer_step_count,
                           std::uint64_t managed_logical_units)
        : canonical_scale_(canonical_scale),
          broker_step_units_(broker_step_units),
          broker_units_(broker_units),
          integer_step_count_(integer_step_count),
          managed_logical_units_(managed_logical_units) {}

    std::uint32_t canonical_scale_ = 0;
    std::uint64_t broker_step_units_ = 0;
    std::uint64_t broker_units_ = 0;
    std::uint64_t integer_step_count_ = 0;
    std::uint64_t managed_logical_units_ = 0;

    friend std::optional<ManagedOpeningQuantity> derive_managed_opening_quantity(
        double, double, const ManagedOpeningQuantityDomain &);
};

/// \brief Derives an exact opening quantity from an MT5 binary observation.
/// \param raw_quantity Observed MT5 deal/position volume.
/// \param raw_step Observed MT5 symbol volume step.
/// \param domain Reviewed canonical scale and logical-unit mapping.
/// \return Exact quantity, or empty when source, step, uniqueness, or checked
///         arithmetic cannot be proven.
inline std::optional<ManagedOpeningQuantity> derive_managed_opening_quantity(
    double raw_quantity, double raw_step,
    const ManagedOpeningQuantityDomain &domain) {
    if (!domain.valid() || !detail::opening_quantity_decimal_is_unique(
            raw_step, domain.broker_step_units, domain.canonical_scale))
        return std::nullopt;

    const auto interval = detail::opening_quantity_rounding_interval(raw_quantity);
    if (!interval)
        return std::nullopt;

    const std::uint64_t capacity =
        (std::numeric_limits<std::uint64_t>::max)() /
        domain.broker_step_units;
    const std::uint64_t max_step_count =
        domain.max_step_count < capacity ? domain.max_step_count : capacity;
    const auto first = detail::first_opening_quantity_step_above(
        interval->lower, domain.broker_step_units, domain.canonical_scale,
        max_step_count);
    if (!first ||
        detail::compare_opening_quantity_step_to_boundary(
            *first, domain.broker_step_units, domain.canonical_scale,
            interval->upper) >= 0)
        return std::nullopt;
    if (*first < max_step_count &&
        detail::compare_opening_quantity_step_to_boundary(
            *first + 1, domain.broker_step_units, domain.canonical_scale,
            interval->upper) < 0)
        return std::nullopt;

    if (*first >
        domain.max_managed_logical_units /
            domain.managed_logical_units_per_step)
        return std::nullopt;
    const std::uint64_t managed_logical_units =
        *first * domain.managed_logical_units_per_step;
    const std::uint64_t broker_units = *first * domain.broker_step_units;
    ManagedOpeningQuantity result(domain.canonical_scale,
                                  domain.broker_step_units, broker_units, *first,
                                  managed_logical_units);
    return result.valid() ? std::optional<ManagedOpeningQuantity>(result)
                          : std::nullopt;
}

} // namespace mt5bridge::dispatch
