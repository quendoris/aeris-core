// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/adapter.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <utility>

namespace {

int failures = 0;

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

class FakeAdapter final : public aeris::source::Adapter {
public:
    [[nodiscard]] aeris::source::AdapterDescriptor descriptor() const noexcept override {
        return {
            "test.fake.v1",
            "test-provider",
            aeris::source::capability_bit(aeris::source::Capability::land),
            aeris::source::TemporalClass::slow_change,
        };
    }

    [[nodiscard]] aeris::source::Result load(
        const aeris::source::VerifiedSnapshot&,
        const aeris::source::Request&
    ) const override {
        return {};
    }
};

aeris::source::Result valid_result() {
    aeris::source::Result result{};
    result.provenance.provider = "test-provider";
    result.provenance.dataset = "land";
    result.provenance.snapshot = "fixture-v1";
    result.provenance.source_uri = "fixture://land";
    result.provenance.license_id = "CC0-1.0";
    result.provenance.content_sha256 = "0123456789abcdef";
    result.provenance.retrieved_at_utc = "2026-08-12T00:00:00Z";

    aeris::source::Feature feature{};
    feature.stable_id = "land-1";
    feature.source_id = "source-land-1";
    aeris::source::FeatureRing ring{};
    ring.geometry = aeris::geometry::LinearRing{{
        {-0.1, -0.1}, {0.1, -0.1}, {0.1, 0.1}, {-0.1, 0.1},
    }};
    ring.role = aeris::source::RingRole::exterior;
    feature.rings.push_back(std::move(ring));
    result.features.push_back(std::move(feature));
    return result;
}

void test_descriptor_and_capabilities() {
    const FakeAdapter adapter{};
    const auto descriptor = adapter.descriptor();
    expect_true("adapter id is stable", descriptor.adapter_id == "test.fake.v1");
    expect_true("land capability advertised", aeris::source::has_capability(descriptor.capabilities, aeris::source::Capability::land));
    expect_true("imagery capability not advertised", !aeris::source::has_capability(descriptor.capabilities, aeris::source::Capability::imagery));
}

void test_success_validation() {
    const FakeAdapter adapter{};
    const auto result = valid_result();
    const aeris::source::Request request{aeris::source::Capability::land, "fixture-v1", ""};
    expect_true("valid result accepted", aeris::source::validate_result(adapter, request, result) == aeris::source::SourceError::none);
}

void test_wrong_provider_rejected() {
    const FakeAdapter adapter{};
    auto result = valid_result();
    result.provenance.provider = "other-provider";
    const aeris::source::Request request{aeris::source::Capability::land, "fixture-v1", ""};
    expect_true("wrong provider rejected", aeris::source::validate_result(adapter, request, result) == aeris::source::SourceError::malformed_source);
}

void test_missing_provenance_rejected() {
    const FakeAdapter adapter{};
    auto result = valid_result();
    result.provenance.content_sha256.clear();
    const aeris::source::Request request{aeris::source::Capability::land, "fixture-v1", ""};
    expect_true("missing provenance rejected", aeris::source::validate_result(adapter, request, result) == aeris::source::SourceError::provenance_incomplete);
}

}  // namespace

int main() {
    test_descriptor_and_capabilities();
    test_success_validation();
    test_wrong_provider_rejected();
    test_missing_provenance_rejected();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "source_adapter: PASS\n";
    return EXIT_SUCCESS;
}
