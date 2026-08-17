// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/geometry/geographic.hpp"
#include "aeris/source/acquisition.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace aeris::source {

enum class Capability : std::uint32_t {
    none = 0U,
    land = 1U << 0U,
    admin0 = 1U << 1U,
    admin1 = 1U << 2U,
    disputed_boundaries = 1U << 3U,
    physical_relief = 1U << 4U,
    hydrography = 1U << 5U,
    imagery = 1U << 6U,
};

using CapabilityMask = std::uint32_t;

[[nodiscard]] constexpr CapabilityMask capability_bit(const Capability capability) noexcept {
    return static_cast<CapabilityMask>(capability);
}

[[nodiscard]] constexpr bool has_capability(
    const CapabilityMask mask,
    const Capability capability
) noexcept {
    return (mask & capability_bit(capability)) != 0U;
}

enum class TemporalClass : std::uint8_t {
    timeless_or_structural = 0U,
    slow_change,
    periodic,
    fast_change,
};

enum class RingRole : std::uint8_t {
    exterior = 0U,
    interior,
};

enum class SourceError : std::uint8_t {
    none = 0U,
    invalid_request,
    unsupported_capability,
    unsupported_worldview,
    unavailable_snapshot,
    malformed_source,
    provenance_incomplete,
    content_hash_mismatch,
    normalization_failed,
};

struct Provenance final {
    std::string provider;
    std::string dataset;
    std::string snapshot;
    std::string dataset_version;
    std::string source_uri;
    std::string license_id;
    std::string content_sha256;
    std::string retrieved_at_utc;
    std::string worldview;

    [[nodiscard]] bool complete() const noexcept;
};

struct AdapterDescriptor final {
    std::string_view adapter_id;
    std::string_view provider;
    CapabilityMask capabilities = 0U;
    TemporalClass temporal_class = TemporalClass::periodic;
};

struct FeatureRing final {
    geometry::LinearRing geometry;
    RingRole role = RingRole::exterior;
};

using FeaturePropertyValue = std::variant<bool, std::int64_t, double, std::string>;

struct FeatureProperty final {
    std::string key;
    FeaturePropertyValue value;
};

struct Feature final {
    std::string stable_id;
    std::string source_id;
    std::vector<FeatureRing> rings;
    std::vector<FeatureProperty> properties;
};

struct Request final {
    Capability capability = Capability::none;
    std::string snapshot;
    std::string worldview;
};

struct Result final {
    Provenance provenance{};
    std::vector<Feature> features;

    // False means this adapter did not provide a complete feature-attribute
    // channel for this load. In that state every Feature::properties list must
    // be empty, so partial attributes can never be mistaken for complete data.
    // True makes each feature list authoritative, including verified-empty lists.
    bool feature_properties_complete = false;

    SourceError error = SourceError::none;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept {
        return error == SourceError::none;
    }
};

// Adapters are intentionally snapshot-independent decoders. A single adapter
// implementation must be able to decode any compatible verified snapshot.
class Adapter {
public:
    virtual ~Adapter() = default;

    [[nodiscard]] virtual AdapterDescriptor descriptor() const noexcept = 0;
    [[nodiscard]] virtual Result load(
        const VerifiedSnapshot& snapshot,
        const Request& request
    ) const = 0;
};

[[nodiscard]] SourceError validate_result(
    const Adapter& adapter,
    const Request& request,
    const Result& result
) noexcept;

}  // namespace aeris::source
