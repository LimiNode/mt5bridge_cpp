#pragma once

/// \file fake_observation_provider.hpp
/// \brief Provides the deterministic observation sequence fake shared by tests.

#include <mt5bridge/reconciliation/coordinator.hpp>

#include <cstddef>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mt5bridge_test_support {

/// \class FakeObservationProvider
/// \brief Replays a fixed sequence of observation batches.
class FakeObservationProvider final : public mt5bridge::ObservationProvider {
public:
    /// \brief Creates a provider with batches returned in order.
    /// \param batches Observation batches to replay.
    explicit FakeObservationProvider(std::vector<mt5bridge::ObservationBatch> batches)
        : batches_(std::move(batches)) {}

    /// \brief Returns the next batch in the configured sequence.
    /// \param request Collection request; ignored by this deterministic fake.
    /// \return Next configured batch.
    mt5bridge::ObservationBatch collect(
        const mt5bridge::ObservationCollectionRequest &) override {
        if (next_ == batches_.size())
            throw std::runtime_error("observation provider exhausted");
        return std::move(batches_[next_++]);
    }

private:
    std::vector<mt5bridge::ObservationBatch> batches_;
    std::size_t next_ = 0;
};

} // namespace mt5bridge_test_support
