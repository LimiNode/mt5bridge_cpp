#pragma once

#ifndef MT5BRIDGE_CPP_HEADER_SRC_DISPATCH_MANAGED_OPENING_SOURCE_DIGEST_HPP_INCLUDED
#define MT5BRIDGE_CPP_HEADER_SRC_DISPATCH_MANAGED_OPENING_SOURCE_DIGEST_HPP_INCLUDED

/// \file dispatch/managed_opening_source_digest.hpp
/// \brief Defines the deterministic non-authoritative OPEN source fingerprint.

#include <cstdint>
#include <string>
#include <vector>

namespace mt5bridge::dispatch::detail {

/// \class ManagedOpeningSourceDigest
/// \brief Computes the stable FNV-64 fingerprint used by B3B source proofs.
/// \warning This value detects source changes but is not an authority proof.
class ManagedOpeningSourceDigest final {
public:
    /// \brief Adds one little-endian unsigned integer to the digest.
    /// \param value Value to add.
    void add_u64(std::uint64_t value) {
        for (unsigned shift = 0; shift != 64; shift += 8) {
            value_ ^= static_cast<std::uint8_t>(value >> shift);
            value_ *= kFnvPrime;
        }
    }

    /// \brief Adds a length-prefixed byte sequence.
    /// \param bytes Bytes to add.
    void add_bytes(const std::vector<std::uint8_t> &bytes) {
        add_u64(static_cast<std::uint64_t>(bytes.size()));
        for (const auto byte : bytes) {
            value_ ^= byte;
            value_ *= kFnvPrime;
        }
    }

    /// \brief Adds a length-prefixed text sequence.
    /// \param value Text to add.
    void add_text(const std::string &value) {
        add_u64(static_cast<std::uint64_t>(value.size()));
        for (const auto byte : value) {
            value_ ^= static_cast<std::uint8_t>(byte);
            value_ *= kFnvPrime;
        }
    }

    /// \brief Returns the deterministic fingerprint.
    /// \return Non-zero FNV-64 fingerprint.
    std::uint64_t value() const { return value_ == 0 ? 1 : value_; }

private:
    static constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    std::uint64_t value_ = 1469598103934665603ULL;
};

} // namespace mt5bridge::dispatch::detail

#endif
