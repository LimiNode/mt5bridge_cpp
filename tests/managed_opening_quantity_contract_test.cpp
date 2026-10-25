/// \file managed_opening_quantity_contract_test.cpp
/// \brief Verifies the private C3A B2 exact quantity-domain contract.

#include "managed_opening_quantity_contract.hpp"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {

using mt5bridge::dispatch::ManagedOpeningQuantityDomain;

void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

ManagedOpeningQuantityDomain cents() {
    return {2, 1, 1, 1000, 1000};
}

void accepts_exact_decimal_steps() {
    const auto domain = cents();
    const auto one = mt5bridge::dispatch::derive_managed_opening_quantity(
        0.01, 0.01, domain);
    require(one && one->valid() && one->canonical_scale() == 2 &&
                one->broker_step_units() == 1 && one->integer_step_count() == 1 &&
                one->broker_units() == 1 && one->managed_logical_units() == 1,
            "0.01 was not accepted as one exact broker step");

    const auto five = mt5bridge::dispatch::derive_managed_opening_quantity(
        0.05, 0.01, domain);
    require(five && five->integer_step_count() == 5 && five->broker_units() == 5 &&
                five->managed_logical_units() == 5,
            "0.05 was not accepted as five exact broker steps");

    const auto ten = mt5bridge::dispatch::derive_managed_opening_quantity(
        0.10, 0.01, domain);
    require(ten && ten->integer_step_count() == 10 && ten->broker_units() == 10,
            "0.10 was not accepted as ten exact broker steps");

    const ManagedOpeningQuantityDomain mapped{2, 10, 3, 1000, 1000};
    const auto mapped_quantity =
        mt5bridge::dispatch::derive_managed_opening_quantity(0.20, 0.10, mapped);
    require(mapped_quantity && mapped_quantity->integer_step_count() == 2 &&
                mapped_quantity->broker_units() == 20 &&
                mapped_quantity->managed_logical_units() == 6,
            "explicit broker-step to managed-unit mapping was not preserved");
}

void rejects_fractional_and_non_positive_values() {
    const auto domain = cents();
    require(!mt5bridge::dispatch::derive_managed_opening_quantity(
                0.015, 0.01, domain),
            "fractional broker step was accepted");
    require(!mt5bridge::dispatch::derive_managed_opening_quantity(
                0.0, 0.01, domain),
            "zero quantity was accepted");
    require(!mt5bridge::dispatch::derive_managed_opening_quantity(
                -0.01, 0.01, domain),
            "negative quantity was accepted");
    require(!mt5bridge::dispatch::derive_managed_opening_quantity(
                std::numeric_limits<double>::quiet_NaN(), 0.01, domain),
            "NaN quantity was accepted");
    require(!mt5bridge::dispatch::derive_managed_opening_quantity(
                std::numeric_limits<double>::infinity(), 0.01, domain),
            "infinite quantity was accepted");
}

void rejects_near_step_values() {
    const auto domain = cents();
    const double below = std::nextafter(0.01, 0.0);
    const double above =
        std::nextafter(0.01, std::numeric_limits<double>::infinity());
    require(!mt5bridge::dispatch::derive_managed_opening_quantity(
                0.01, below, domain),
            "double immediately below the reviewed step was accepted");
    require(!mt5bridge::dispatch::derive_managed_opening_quantity(
                0.01, above, domain),
            "double immediately above the reviewed step was accepted");
    require(!mt5bridge::dispatch::derive_managed_opening_quantity(
                below, 0.01, domain),
            "double immediately below the reviewed quantity was accepted");
    require(!mt5bridge::dispatch::derive_managed_opening_quantity(
                above, 0.01, domain),
            "double immediately above the reviewed quantity was accepted");
}

void rejects_ambiguous_or_incompatible_domains() {
    const auto domain = cents();
    require(!mt5bridge::dispatch::derive_managed_opening_quantity(
                100000000000000.0, 0.01, domain),
            "a quantity with multiple possible step counts was accepted");
    require(!mt5bridge::dispatch::derive_managed_opening_quantity(
                0.03, 0.02, domain),
            "a fractional integer step count was accepted");

    auto incompatible = domain;
    incompatible.canonical_scale = 3;
    incompatible.broker_step_units = 1;
    require(!mt5bridge::dispatch::derive_managed_opening_quantity(
                0.01, 0.01, incompatible),
            "a step with an incompatible canonical scale was accepted");

    auto invalid_domain = domain;
    invalid_domain.canonical_scale = 10;
    require(!mt5bridge::dispatch::derive_managed_opening_quantity(
                0.01, 0.01, invalid_domain),
            "an out-of-range canonical scale was accepted");
}

void rejects_checked_arithmetic_overflow() {
    auto logical_overflow = cents();
    logical_overflow.max_step_count = 2;
    logical_overflow.managed_logical_units_per_step =
        (std::numeric_limits<std::uint64_t>::max)();
    require(!mt5bridge::dispatch::derive_managed_opening_quantity(
                0.02, 0.01, logical_overflow),
            "managed logical-unit multiplication overflowed silently");

    auto capacity = cents();
    capacity.max_step_count = 1;
    require(!mt5bridge::dispatch::derive_managed_opening_quantity(
                0.02, 0.01, capacity),
            "bounded step capacity was ignored");
}

} // namespace

int main() {
    try {
        accepts_exact_decimal_steps();
        rejects_fractional_and_non_positive_values();
        rejects_near_step_values();
        rejects_ambiguous_or_incompatible_domains();
        rejects_checked_arithmetic_overflow();
        std::cout << "managed opening quantity contract tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
