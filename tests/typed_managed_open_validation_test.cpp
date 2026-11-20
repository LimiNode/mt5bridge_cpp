/// \file typed_managed_open_validation_test.cpp
/// \brief Checks unauthenticated typed OPEN quantities and market policies.

#include "typed_managed_open_validation.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {

using mt5bridge::dispatch::ManagedOpenCandidate;
using mt5bridge::dispatch::ManagedOpenFilling;
using mt5bridge::dispatch::ManagedOpenSide;
using mt5bridge::dispatch::ManagedOpenValidationStatus;
using mt5bridge::dispatch::SuppliedManagedOpenMetadata;
using mt5bridge::dispatch::TypedManagedOpenIntent;
using mt5bridge::dispatch::validate_typed_managed_open;
using Status = ManagedOpenValidationStatus;

static_assert(std::is_aggregate_v<TypedManagedOpenIntent>,
              "typed intent must remain a plain desired-value input");
static_assert(std::is_aggregate_v<ManagedOpenCandidate> &&
                  std::is_copy_constructible_v<ManagedOpenCandidate>,
              "a numeric candidate is deliberately not a sealed capability");

/// \brief Fails every invalid expectation in Release as well as Debug.
void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

/// \brief Copies a bounded supplied identity without platform string helpers.
template <std::size_t Capacity>
void text(char (&destination)[Capacity], const std::string &value) {
    require(value.size() < Capacity, "test identity exceeds its buffer");
    std::fill_n(destination, Capacity, '\0');
    std::copy(value.begin(), value.end(), destination);
}

/// \brief Groups desired values and explicitly unauthenticated metadata.
struct Fixture {
    TypedManagedOpenIntent intent; ///< Desired values only.
    SuppliedManagedOpenMetadata supplied; ///< Caller-authored test observations.
};

/// \brief Creates one whole-lot market BUY with exact one-cent broker steps.
Fixture fixture() {
    Fixture value;
    value.intent = {{"Validation-Server", 77}, "EURUSD", ManagedOpenSide::buy,
                    1, ManagedOpenFilling::fok};
    text(value.supplied.account.server, value.intent.account.server);
    value.supplied.account.login = value.intent.account.login;
    value.supplied.account.known_fields = MT5BRIDGE_ACCOUNT_KNOWN_SERVER |
        MT5BRIDGE_ACCOUNT_KNOWN_LOGIN | MT5BRIDGE_ACCOUNT_KNOWN_TRADE_ALLOWED |
        MT5BRIDGE_ACCOUNT_KNOWN_TRADE_EXPERT;
    value.supplied.account.trade_allowed = 1;
    value.supplied.account.trade_expert = 1;
    value.supplied.symbol_account = value.intent.account;
    text(value.supplied.symbol.symbol, value.intent.symbol);
    value.supplied.symbol.known_fields = MT5BRIDGE_SYMBOL_KNOWN_TRADE_MODE |
        MT5BRIDGE_SYMBOL_KNOWN_ORDER_MODE | MT5BRIDGE_SYMBOL_KNOWN_TRADE_EXEMODE |
        MT5BRIDGE_SYMBOL_KNOWN_FILLING_MODE | MT5BRIDGE_SYMBOL_KNOWN_VOLUME_LIMITS;
    value.supplied.symbol.trade_mode = 4;
    value.supplied.symbol.trade_exemode = 2;
    value.supplied.symbol.order_mode = 1;
    value.supplied.symbol.filling_mode = 3;
    value.supplied.symbol.volume_min = 0.01;
    value.supplied.symbol.volume_max = 100.0;
    value.supplied.symbol.volume_step = 0.01;
    value.supplied.symbol.volume_limit = 0.0;
    value.supplied.normalization = {2, 1};
    return value;
}

/// \brief Requires an exact failure outcome and absence of a numeric candidate.
void rejects(const Fixture &value, Status expected, const char *message) {
    const auto result = validate_typed_managed_open(value.intent, value.supplied);
    require(result.status == expected && !result.candidate, message);
}

/// \brief Returns a successful numeric candidate without claiming authority.
ManagedOpenCandidate candidate(const Fixture &value) {
    const auto result = validate_typed_managed_open(value.intent, value.supplied);
    require(result.status == Status::candidate && result.candidate,
            "consistent supplied values did not yield a numeric candidate");
    return *result.candidate;
}

/// \brief Preserves whole-lot semantics for BUY/SELL and canonical normalizations.
void accepts_exact_whole_lot_candidates() {
    for (const auto side : {ManagedOpenSide::buy, ManagedOpenSide::sell}) {
        auto value = fixture();
        value.intent.side = side;
        const auto result = candidate(value);
        require(result.intent.account == value.intent.account &&
                    result.intent.symbol == value.intent.symbol &&
                    result.intent.side == side && result.intent.logical_volume == 1 &&
                    result.broker_volume.units == 100 && result.broker_volume.scale == 2 &&
                    result.broker_volume.step_units == 1,
                "one legacy whole lot was confused with one broker step");
        value.supplied.normalization = {3, 10};
        require(candidate(value).broker_volume == result.broker_volume,
                "equivalent decimal domains yielded different canonical candidates");
    }

    auto value = fixture();
    value.supplied.normalization = {3, 1000};
    value.supplied.symbol.volume_step = 1.0;
    value.supplied.symbol.volume_min = 1.0;
    const auto whole = candidate(value);
    require(whole.broker_volume.units == 1 && whole.broker_volume.scale == 0 &&
                whole.broker_volume.step_units == 1,
            "whole-step normalization was not reduced canonically");

    value = fixture();
    value.intent.account.server = "\xd0\xa1" "erver";
    text(value.supplied.account.server, value.intent.account.server);
    value.supplied.symbol_account = value.intent.account;
    value.intent.symbol = "\xd0\x95URUSD";
    text(value.supplied.symbol.symbol, value.intent.symbol);
    candidate(value);
}

/// \brief Requires explicit policy and rejects malformed desired identities.
void rejects_invalid_intents() {
    const TypedManagedOpenIntent defaults;
    require(defaults.side == ManagedOpenSide::unspecified &&
                defaults.filling == ManagedOpenFilling::unspecified,
            "default initialization inferred a trading side or filling policy");
    auto value = fixture();
    value.intent.side = defaults.side;
    rejects(value, Status::invalid_intent, "unspecified side was accepted");
    value = fixture();
    value.intent.filling = defaults.filling;
    rejects(value, Status::invalid_intent, "unspecified filling policy was accepted");
    for (const int pending_or_unknown_side : {2, 3, 4, 5, 99}) {
        value = fixture();
        value.intent.side = static_cast<ManagedOpenSide>(pending_or_unknown_side);
        rejects(value, Status::invalid_intent, "non-market OPEN side was accepted");
    }
    for (const int return_boc_or_unknown : {2, 3, 99}) {
        value = fixture();
        value.intent.filling = static_cast<ManagedOpenFilling>(return_boc_or_unknown);
        rejects(value, Status::invalid_intent, "RETURN/BOC/unknown filling was accepted");
    }
    value = fixture();
    value.intent.logical_volume = 0;
    rejects(value, Status::invalid_intent, "zero logical volume was accepted");
    value = fixture();
    value.intent.account.login = 0;
    rejects(value, Status::invalid_intent, "zero account login was accepted");
    for (const auto &server : {std::string{}, std::string("bad\nserver"),
                              std::string("bad\0server", 10), std::string("\x80"),
                              std::string("\xc0\xaf"), std::string("\xed\xa0\x80"),
                              std::string("\xf4\x90\x80\x80"), std::string("\xe2\x82")}) {
        value = fixture();
        value.intent.account.server = server;
        rejects(value, Status::invalid_intent, "malformed desired server was accepted");
    }
    value = fixture();
    value.intent.account.server.assign(sizeof(value.supplied.account.server), 'x');
    rejects(value, Status::invalid_intent, "over-capacity desired server was accepted");
}

/// \brief Separates account identity provenance from explicit trading permission.
void rejects_missing_account_identity_and_permissions() {
    for (const std::uint64_t bit : {MT5BRIDGE_ACCOUNT_KNOWN_SERVER,
                                   MT5BRIDGE_ACCOUNT_KNOWN_LOGIN}) {
        auto value = fixture();
        value.supplied.account.known_fields &= ~bit;
        rejects(value, Status::account_mismatch, "unknown supplied account identity was used");
    }
    auto value = fixture();
    value.supplied.account.login = 78;
    rejects(value, Status::account_mismatch, "foreign account login was accepted");
    value = fixture();
    text(value.supplied.account.server, "Other-Server");
    rejects(value, Status::account_mismatch, "foreign account server was accepted");
    value = fixture();
    value.supplied.symbol_account.login = 78;
    rejects(value, Status::account_mismatch, "foreign symbol-account scope was accepted");
    value = fixture();
    value.supplied.symbol_account = {};
    rejects(value, Status::account_mismatch, "missing symbol-account scope was accepted");
    value = fixture();
    std::fill_n(value.supplied.account.server, sizeof(value.supplied.account.server), 'x');
    rejects(value, Status::account_mismatch, "unterminated supplied server was accepted");
    value = fixture();
    text(value.supplied.account.server, "\xed\xa0\x80");
    rejects(value, Status::account_mismatch, "invalid supplied server UTF-8 was accepted");
    for (const std::uint64_t bit : {MT5BRIDGE_ACCOUNT_KNOWN_TRADE_ALLOWED,
                                   MT5BRIDGE_ACCOUNT_KNOWN_TRADE_EXPERT}) {
        value = fixture();
        value.supplied.account.known_fields &= ~bit;
        rejects(value, Status::permission_denied, "unknown account permission was inferred");
    }
    value = fixture();
    value.supplied.account.trade_allowed = 0;
    rejects(value, Status::permission_denied, "denied account trading permission was ignored");
    value = fixture();
    value.supplied.account.trade_expert = 0;
    rejects(value, Status::permission_denied, "denied expert/API permission was ignored");
}

/// \brief Rejects mismatched or invalid bounded UTF-8 symbol names.
void rejects_invalid_symbols() {
    for (const auto &symbol : {std::string{}, std::string("bad\0symbol", 10),
                              std::string("bad\x7f"), std::string("\xc0\xaf"),
                              std::string("\xed\xa0\x80"), std::string("\xe2\x82")}) {
        auto value = fixture();
        value.intent.symbol = symbol;
        rejects(value, Status::invalid_symbol, "malformed desired symbol was accepted");
    }
    auto value = fixture();
    value.intent.symbol.assign(sizeof(value.supplied.symbol.symbol), 'x');
    rejects(value, Status::invalid_symbol, "over-capacity desired symbol was accepted");
    value = fixture();
    text(value.supplied.symbol.symbol, "GBPUSD");
    rejects(value, Status::invalid_symbol, "mismatched supplied symbol was accepted");
    value = fixture();
    text(value.supplied.symbol.symbol, "\x80");
    rejects(value, Status::invalid_symbol, "invalid supplied symbol UTF-8 was accepted");
    value = fixture();
    std::fill_n(value.supplied.symbol.symbol, sizeof(value.supplied.symbol.symbol), 'x');
    rejects(value, Status::invalid_symbol, "unterminated supplied symbol was accepted");
}

/// \brief Distinguishes symbol bit flags from request enum values and unknown modes.
void enforces_narrow_market_filling_policy() {
    for (const std::uint64_t bit : {MT5BRIDGE_SYMBOL_KNOWN_TRADE_MODE,
                                   MT5BRIDGE_SYMBOL_KNOWN_ORDER_MODE,
                                   MT5BRIDGE_SYMBOL_KNOWN_TRADE_EXEMODE,
                                   MT5BRIDGE_SYMBOL_KNOWN_FILLING_MODE}) {
        auto value = fixture();
        value.supplied.symbol.known_fields &= ~bit;
        rejects(value, Status::unsupported_policy, "missing symbol capability was inferred");
    }
    for (const std::uint32_t mode : {0U, 1U, 3U, 99U}) {
        auto value = fixture();
        value.supplied.symbol.trade_exemode = mode;
        rejects(value, Status::unsupported_policy, "unsupported execution mode was accepted");
    }
    for (const std::uint32_t mask : {0U, 2U, 128U, 129U}) {
        auto value = fixture();
        value.supplied.symbol.order_mode = mask;
        rejects(value, Status::unsupported_policy, "missing market or unknown order flag was accepted");
    }
    for (const std::uint32_t mask : {0U, 4U, 8U, 9U}) {
        auto value = fixture();
        value.supplied.symbol.filling_mode = mask;
        rejects(value, Status::unsupported_policy, "missing FOK or unknown filling flag was accepted");
    }
    auto value = fixture();
    value.supplied.symbol.filling_mode = 1;
    require(candidate(value).intent.filling == ManagedOpenFilling::fok,
            "SYMBOL_FILLING_FOK=1 was confused with ORDER_FILLING_FOK=0");
    value.intent.filling = ManagedOpenFilling::ioc;
    rejects(value, Status::unsupported_policy, "FOK-only flag was treated as IOC enum=1");
    value.supplied.symbol.filling_mode = 2;
    candidate(value);
    value.intent.filling = ManagedOpenFilling::fok;
    rejects(value, Status::unsupported_policy, "IOC-only flag was treated as FOK enum=0");

    for (const std::uint32_t mode : {0U, 3U, 5U, 99U}) {
        value = fixture();
        value.supplied.symbol.trade_mode = mode;
        rejects(value, Status::unsupported_policy, "disabled/close-only/unknown trade mode was accepted");
    }
    value = fixture();
    value.supplied.symbol.trade_mode = 1;
    candidate(value);
    value.intent.side = ManagedOpenSide::sell;
    rejects(value, Status::unsupported_policy, "long-only symbol admitted SELL");
    value.supplied.symbol.trade_mode = 2;
    candidate(value);
    value.intent.side = ManagedOpenSide::buy;
    rejects(value, Status::unsupported_policy, "short-only symbol admitted BUY");
}

/// \brief Keeps step, min/max, and conversion checks exact without clamping.
void rejects_inexact_quantities_and_bounds() {
    auto value = fixture();
    value.supplied.symbol.known_fields &= ~MT5BRIDGE_SYMBOL_KNOWN_VOLUME_LIMITS;
    rejects(value, Status::invalid_quantity, "unknown volume limits were accepted");
    for (const auto normalization : {mt5bridge::BrokerVolumeNormalization{10, 1},
                                     mt5bridge::BrokerVolumeNormalization{2, 0}}) {
        value = fixture();
        value.supplied.normalization = normalization;
        rejects(value, Status::invalid_quantity, "invalid normalization domain was accepted");
    }
    for (const double step : {0.0, -0.01, std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::infinity(),
                              std::nextafter(0.01, 0.0),
                              std::nextafter(0.01, std::numeric_limits<double>::infinity())}) {
        value = fixture();
        value.supplied.symbol.volume_step = step;
        rejects(value, Status::invalid_quantity, "inexact or invalid broker step was accepted");
    }
    value = fixture();
    value.supplied.symbol.volume_min = 1.0;
    value.supplied.symbol.volume_max = 1.0;
    candidate(value);
    value.intent.logical_volume = 2;
    rejects(value, Status::invalid_quantity, "oversized request was silently clamped");
    value = fixture();
    value.supplied.symbol.volume_min = 2.0;
    rejects(value, Status::invalid_quantity, "undersized request was silently raised");
    for (const double minimum : {0.0, -0.01, 0.015,
                                 std::nextafter(0.01, 0.0),
                                 std::numeric_limits<double>::quiet_NaN()}) {
        value = fixture();
        value.supplied.symbol.volume_min = minimum;
        rejects(value, Status::invalid_quantity, "invalid/non-grid minimum was accepted");
    }
    for (const double maximum : {0.0, -1.0, 1.005, 0.99,
                                 std::nextafter(100.0, std::numeric_limits<double>::infinity()),
                                 std::numeric_limits<double>::infinity()}) {
        value = fixture();
        value.supplied.symbol.volume_max = maximum;
        rejects(value, Status::invalid_quantity, "invalid/non-grid maximum was accepted");
    }
    value = fixture();
    value.supplied.symbol.volume_min = 2.0;
    value.supplied.symbol.volume_max = 1.0;
    rejects(value, Status::invalid_quantity, "reversed min/max bounds were accepted");
    value = fixture();
    value.supplied.normalization = {2, 3};
    value.supplied.symbol.volume_step = 0.03;
    value.supplied.symbol.volume_min = 0.03;
    value.supplied.symbol.volume_max = 3.0;
    rejects(value, Status::invalid_quantity, "one lot was rounded onto a three-cent grid");
    value.intent.logical_volume = 3;
    const auto exact = candidate(value);
    require(exact.broker_volume.units == 300 && exact.broker_volume.step_units == 3,
            "exact whole-lot multiple of a nondecimal-unit step was rejected");
}

/// \brief Rejects arithmetic overflow and ambiguous binary64 quantities near 2^53.
void rejects_overflow_and_binary64_ambiguity() {
    auto value = fixture();
    value.intent.logical_volume = (std::numeric_limits<std::uint64_t>::max)();
    rejects(value, Status::invalid_quantity, "whole-lot to broker-unit multiplication overflowed");
    value = fixture();
    value.supplied.normalization = {0, 1};
    value.supplied.symbol.volume_step = 1.0;
    value.supplied.symbol.volume_min = 1.0;
    value.supplied.symbol.volume_max = 9007199254740994.0;
    value.intent.logical_volume = UINT64_C(9007199254740992);
    rejects(value, Status::invalid_quantity, "ambiguous 2^53 binary64 quantity was accepted");
    value.intent.logical_volume = UINT64_C(9007199254740993);
    rejects(value, Status::invalid_quantity, "rounded 2^53+1 logical quantity was accepted");
    value.intent.logical_volume = UINT64_C(9007199254740994);
    require(candidate(value).broker_volume.units == value.intent.logical_volume,
            "unique exactly representable quantity above 2^53 was rejected blindly");
}

/// \brief Requires complete exact aggregate values for positive directional limits.
void enforces_directional_volume_limits() {
    auto value = fixture();
    candidate(value);
    value.supplied.symbol.volume_limit = 2.0;
    rejects(value, Status::incomplete_directional_volume,
            "positive limit accepted an absent directional aggregate");
    for (const double used : {0.0, 0.5, 0.99, 1.0}) {
        value.supplied.directional_volume = used;
        candidate(value);
    }
    for (const double used : {1.01, 2.0}) {
        value.supplied.directional_volume = used;
        rejects(value, Status::volume_limit_exceeded, "aggregate limit was exceeded silently");
    }
    for (const double used : {-0.0, -1.0, 0.005,
                              std::numeric_limits<double>::quiet_NaN(),
                              std::numeric_limits<double>::infinity(),
                              std::nextafter(1.0, 0.0),
                              std::nextafter(1.0, std::numeric_limits<double>::infinity())}) {
        value.supplied.directional_volume = used;
        rejects(value, Status::invalid_quantity, "invalid/near-grid aggregate was tolerated");
    }
    value.supplied.directional_volume = 0.0;
    value.supplied.symbol.volume_limit = 0.5;
    rejects(value, Status::volume_limit_exceeded, "request larger than the limit was accepted");
    for (const double limit : {-0.0, -1.0, 1.005,
                               std::numeric_limits<double>::quiet_NaN(),
                               std::numeric_limits<double>::infinity(),
                               std::nextafter(2.0, std::numeric_limits<double>::infinity())}) {
        value.supplied.symbol.volume_limit = limit;
        rejects(value, Status::invalid_quantity, "invalid/non-grid directional limit was accepted");
    }
}

/// \brief Shows copyable diagnostic candidates cannot authenticate edited inputs.
void candidate_remains_unauthenticated_data() {
    auto value = fixture();
    auto forged = candidate(value);
    forged.intent.account.login = 0;
    forged.broker_volume.units = 999;
    require(forged.broker_volume.units == 999,
            "diagnostic candidate unexpectedly became an opaque authority proof");
    const auto revalidated = validate_typed_managed_open(forged.intent, value.supplied);
    require(revalidated.status == Status::invalid_intent && !revalidated.candidate,
            "editing a numeric candidate authenticated its forged desired values");
    const auto retained = candidate(value);
    value.intent.symbol = "changed";
    value.supplied.symbol.volume_step = 123.0;
    require(retained.intent.symbol == "EURUSD" && retained.broker_volume.units == 100,
            "candidate retained mutable references to supplied inputs");
}

} // namespace

/// \brief Runs always-on native regressions without terminal/runtime dependencies.
int main() {
    try {
        accepts_exact_whole_lot_candidates();
        rejects_invalid_intents();
        rejects_missing_account_identity_and_permissions();
        rejects_invalid_symbols();
        enforces_narrow_market_filling_policy();
        rejects_inexact_quantities_and_bounds();
        rejects_overflow_and_binary64_ambiguity();
        enforces_directional_volume_limits();
        candidate_remains_unauthenticated_data();
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
