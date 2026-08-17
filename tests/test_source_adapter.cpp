// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/adapter.hpp"

#include <cstdlib>
#include <limits>
#include <iostream>
#include <string>
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

const aeris::source::Request kRequest{
    aeris::source::Capability::land,
    "fixture-v1",
    "",
};

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
    expect_true("valid geometry-only result accepted",
                aeris::source::validate_result(adapter, kRequest, result) == aeris::source::SourceError::none);
}

void test_wrong_provider_rejected() {
    const FakeAdapter adapter{};
    auto result = valid_result();
    result.provenance.provider = "other-provider";
    expect_true("wrong provider rejected",
                aeris::source::validate_result(adapter, kRequest, result) == aeris::source::SourceError::malformed_source);
}

void test_missing_provenance_rejected() {
    const FakeAdapter adapter{};
    auto result = valid_result();
    result.provenance.content_sha256.clear();
    expect_true("missing provenance rejected",
                aeris::source::validate_result(adapter, kRequest, result) == aeris::source::SourceError::provenance_incomplete);
}

void test_incomplete_property_channel_rejected() {
    const FakeAdapter adapter{};
    auto result = valid_result();
    result.features.front().properties.push_back({"name", std::string("Alpha")});
    expect_true("partial property payload without completeness marker rejected",
                aeris::source::validate_result(adapter, kRequest, result) == aeris::source::SourceError::malformed_source);
}

void test_complete_empty_property_channel_accepted() {
    const FakeAdapter adapter{};
    auto result = valid_result();
    result.feature_properties_complete = true;
    expect_true("complete verified-empty property channel accepted",
                aeris::source::validate_result(adapter, kRequest, result) == aeris::source::SourceError::none);
}

void test_complete_typed_properties_accepted() {
    const FakeAdapter adapter{};
    auto result = valid_result();
    result.feature_properties_complete = true;
    auto& properties = result.features.front().properties;
    properties.push_back({"name", std::string("Åland")});
    properties.push_back({"admin_level", static_cast<std::int64_t>(0)});
    properties.push_back({"claimed", true});
    properties.push_back({"weight", 0.5});
    expect_true("complete typed property channel accepted",
                aeris::source::validate_result(adapter, kRequest, result) == aeris::source::SourceError::none);
}

void test_duplicate_property_key_rejected() {
    const FakeAdapter adapter{};
    auto result = valid_result();
    result.feature_properties_complete = true;
    result.features.front().properties.push_back({"name", std::string("Alpha")});
    result.features.front().properties.push_back({"name", std::string("Beta")});
    expect_true("duplicate feature property key rejected",
                aeris::source::validate_result(adapter, kRequest, result) == aeris::source::SourceError::malformed_source);
}

void test_nonfinite_property_rejected() {
    const FakeAdapter adapter{};
    auto result = valid_result();
    result.feature_properties_complete = true;
    result.features.front().properties.push_back({
        "weight", std::numeric_limits<double>::quiet_NaN()});
    expect_true("non-finite feature property rejected",
                aeris::source::validate_result(adapter, kRequest, result) == aeris::source::SourceError::malformed_source);
}

void test_invalid_utf8_and_nul_rejected() {
    const FakeAdapter adapter{};

    auto invalid_key = valid_result();
    invalid_key.feature_properties_complete = true;
    invalid_key.features.front().properties.push_back({std::string("na\0me", 5U), std::string("Alpha")});
    expect_true("NUL in feature property key rejected",
                aeris::source::validate_result(adapter, kRequest, invalid_key) == aeris::source::SourceError::malformed_source);

    auto invalid_text = valid_result();
    invalid_text.feature_properties_complete = true;
    invalid_text.features.front().properties.push_back({"name", std::string("\xC0\xAF", 2U)});
    expect_true("overlong invalid UTF-8 feature property rejected",
                aeris::source::validate_result(adapter, kRequest, invalid_text) == aeris::source::SourceError::malformed_source);
}

}  // namespace

int main() {
    test_descriptor_and_capabilities();
    test_success_validation();
    test_wrong_provider_rejected();
    test_missing_provenance_rejected();
    test_incomplete_property_channel_rejected();
    test_complete_empty_property_channel_accepted();
    test_complete_typed_properties_accepted();
    test_duplicate_property_key_rejected();
    test_nonfinite_property_rejected();
    test_invalid_utf8_and_nul_rejected();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "source_adapter: PASS\n";
    return EXIT_SUCCESS;
}
