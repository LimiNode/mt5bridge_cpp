/// \file typed_managed_open_validation.cpp
/// \brief Implements Python-free quantity/policy checks on unauthenticated inputs.

#include "typed_managed_open_validation.hpp"
#include "managed_opening_quantity_contract.hpp"

#include <mt5bridge/dispatch/managed_broker_volume_mapping.hpp>

#include <cmath>
#include <string_view>

namespace mt5bridge::dispatch {
namespace {

/// \brief Checks bounded, nonempty UTF-8 identities without NUL/control bytes.
bool valid_identity(std::string_view value, std::size_t capacity) {
    if (value.empty() || value.size() >= capacity)
        return false;
    for (std::size_t index = 0; index < value.size();) {
        const auto lead = static_cast<unsigned char>(value[index++]);
        if (lead < 0x80) {
            if (lead < 0x20 || lead == 0x7f)
                return false;
            continue;
        }
        const unsigned length = lead >= 0xc2 && lead <= 0xdf ? 2 :
                                lead >= 0xe0 && lead <= 0xef ? 3 :
                                lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
        if (length == 0 || value.size() - index < length - 1)
            return false;
        const auto second = static_cast<unsigned char>(value[index]);
        if ((lead == 0xe0 && second < 0xa0) ||
            (lead == 0xed && second >= 0xa0) ||
            (lead == 0xf0 && second < 0x90) ||
            (lead == 0xf4 && second >= 0x90))
            return false;
        for (unsigned tail = 1; tail < length; ++tail)
            if ((static_cast<unsigned char>(value[index++]) & 0xc0) != 0x80)
                return false;
    }
    return true;
}

/// \brief Obtains a bounded view; an unterminated buffer remains over capacity.
template <std::size_t Capacity>
std::string_view bounded_identity(const char (&text)[Capacity]) {
    std::size_t length = 0;
    while (length < Capacity && text[length] != '\0')
        ++length;
    return {text, length};
}

/// \brief Keeps SYMBOL_FILLING flags distinct from ORDER_FILLING enum values.
bool supported_policy(const TypedManagedOpenIntent &intent,
                      const Mt5SymbolCapabilities &symbol) {
    constexpr auto required = MT5BRIDGE_SYMBOL_KNOWN_TRADE_MODE |
                              MT5BRIDGE_SYMBOL_KNOWN_ORDER_MODE |
                              MT5BRIDGE_SYMBOL_KNOWN_TRADE_EXEMODE |
                              MT5BRIDGE_SYMBOL_KNOWN_FILLING_MODE;
    // Only MARKET execution is supported: no price/deviation policy is inferred.
    if ((symbol.known_fields & required) != required || symbol.trade_exemode != 2 ||
        (symbol.order_mode & 1U) == 0 || (symbol.order_mode & ~127U) != 0 ||
        (symbol.filling_mode & ~7U) != 0)
        return false;
    const bool side_allowed = symbol.trade_mode == 4 ||
        (symbol.trade_mode == 1 && intent.side == ManagedOpenSide::buy) ||
        (symbol.trade_mode == 2 && intent.side == ManagedOpenSide::sell);
    const std::uint32_t filling_flag =
        intent.filling == ManagedOpenFilling::fok ? 1U : 2U;
    return side_allowed && (symbol.filling_mode & filling_flag) != 0;
}

} // namespace

ManagedOpenValidationResult validate_typed_managed_open(
    const TypedManagedOpenIntent &intent,
    const SuppliedManagedOpenMetadata &supplied) {
    using Status = ManagedOpenValidationStatus;
    const auto reject = [](Status status) {
        return ManagedOpenValidationResult{status, std::nullopt};
    };
    if (!intent.account.valid() ||
        !valid_identity(intent.account.server, sizeof(supplied.account.server)) ||
        intent.logical_volume == 0 ||
        (intent.side != ManagedOpenSide::buy && intent.side != ManagedOpenSide::sell) ||
        (intent.filling != ManagedOpenFilling::fok &&
         intent.filling != ManagedOpenFilling::ioc))
        return reject(Status::invalid_intent);
    const auto server = bounded_identity(supplied.account.server);
    if (!valid_identity(server, sizeof(supplied.account.server)) ||
        make_account_key(supplied.account) != intent.account ||
        supplied.symbol_account != intent.account)
        return reject(Status::account_mismatch);
    constexpr auto permissions = MT5BRIDGE_ACCOUNT_KNOWN_TRADE_ALLOWED |
                                 MT5BRIDGE_ACCOUNT_KNOWN_TRADE_EXPERT;
    if ((supplied.account.known_fields & permissions) != permissions ||
        supplied.account.trade_allowed == 0 || supplied.account.trade_expert == 0)
        return reject(Status::permission_denied);
    const auto &symbol = supplied.symbol;
    const auto name = bounded_identity(symbol.symbol);
    if (!valid_identity(intent.symbol, sizeof(symbol.symbol)) ||
        !valid_identity(name, sizeof(symbol.symbol)) || name != intent.symbol)
        return reject(Status::invalid_symbol);
    if (!supported_policy(intent, symbol))
        return reject(Status::unsupported_policy);
    if ((symbol.known_fields & MT5BRIDGE_SYMBOL_KNOWN_VOLUME_LIMITS) == 0 ||
        !supplied.normalization.valid())
        return reject(Status::invalid_quantity);

    // Reduce equivalent decimal domains before any conversion/comparison.
    auto normalization = supplied.normalization;
    while (normalization.scale > 0 && normalization.step_units % 10 == 0) {
        --normalization.scale;
        normalization.step_units /= 10;
    }
    const ManagedOpeningQuantityDomain domain{
        normalization.scale, normalization.step_units, 1};
    const auto convert = [&](double raw) {
        return derive_managed_opening_quantity(raw, symbol.volume_step, domain);
    };
    const auto minimum = convert(symbol.volume_min);
    const auto maximum = convert(symbol.volume_max);
    const auto broker = map_managed_logical_units_to_broker_volume(
        intent.logical_volume, normalization);
    if (!minimum || !maximum || !broker ||
        minimum->broker_units() > maximum->broker_units() ||
        broker->units < minimum->broker_units() || broker->units > maximum->broker_units())
        return reject(Status::invalid_quantity);
    // B2's step-count logical map is temporary and never enters the owner state.
    const auto round_trip = convert(static_cast<double>(intent.logical_volume));
    if (!round_trip || round_trip->broker_units() != broker->units ||
        !matches_managed_opening_quantity(
            static_cast<double>(intent.logical_volume), *round_trip))
        return reject(Status::invalid_quantity);

    if (!std::isfinite(symbol.volume_limit) || std::signbit(symbol.volume_limit))
        return reject(Status::invalid_quantity);
    if (symbol.volume_limit > 0.0) {
        const auto limit = convert(symbol.volume_limit);
        if (!limit)
            return reject(Status::invalid_quantity);
        if (!supplied.directional_volume)
            return reject(Status::incomplete_directional_volume);
        const double raw_used = *supplied.directional_volume;
        if (!std::isfinite(raw_used) || std::signbit(raw_used))
            return reject(Status::invalid_quantity);
        std::uint64_t used = 0;
        if (raw_used != 0.0) {
            const auto aggregate = convert(raw_used);
            if (!aggregate)
                return reject(Status::invalid_quantity);
            used = aggregate->broker_units();
        }
        if (broker->units > limit->broker_units() ||
            used > limit->broker_units() - broker->units)
            return reject(Status::volume_limit_exceeded);
    }
    return {Status::candidate, ManagedOpenCandidate{intent, *broker}};
}

} // namespace mt5bridge::dispatch
