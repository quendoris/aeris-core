// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/natural_earth_cartography.hpp"

#include "aeris/source/dbf.hpp"
#include "aeris/source/natural_earth.hpp"

#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace aeris::source {
namespace {

[[nodiscard]] std::optional<std::size_t> field_index(
    const DbfTableResult& table,
    const std::string_view name
) {
    for (std::size_t index = 0U; index < table.fields.size(); ++index) {
        if (table.fields[index].name == name) return index;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<std::int64_t> positive_integer(
    const std::string& raw
) noexcept {
    const std::size_t first = raw.find_first_not_of(' ');
    if (first == std::string::npos) return std::nullopt;
    const std::size_t last = raw.find_last_not_of(' ');
    const char* begin = raw.data() + static_cast<std::ptrdiff_t>(first);
    const char* end = raw.data() + static_cast<std::ptrdiff_t>(last + 1U);

    std::int64_t value = 0;
    const auto parsed = std::from_chars(begin, end, value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value <= 0) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] Result failure(const SourceError error, std::string diagnostic) {
    Result result{};
    result.error = error;
    result.diagnostic = std::move(diagnostic);
    return result;
}

}  // namespace

AdapterDescriptor NaturalEarthAdmin0Cartography110mAdapter::descriptor() const noexcept {
    return {
        "natural-earth.ne-110m-admin0-cartography.shapefile-dbf.v1",
        "Natural Earth",
        capability_bit(Capability::admin0),
        TemporalClass::slow_change,
    };
}

Result NaturalEarthAdmin0Cartography110mAdapter::load(
    const VerifiedSnapshot& snapshot,
    const Request& request
) const {
    NaturalEarthAdmin0Countries110mAdapter canonical{};
    Result result = canonical.load(snapshot, request);
    if (!result.ok()) return result;

    const auto dbf_path = snapshot.resource_path("attributes.dbf");
    if (!dbf_path.has_value()) {
        return failure(
            SourceError::provenance_incomplete,
            "Natural Earth cartography adapter requires attributes.dbf"
        );
    }

    const DbfTableResult table = read_dbf_table(*dbf_path);
    if (!table.ok()) {
        return failure(
            SourceError::normalization_failed,
            "Natural Earth cartography DBF failed strict decoding: " + table.diagnostic
        );
    }
    if (table.records.size() != result.features.size()) {
        return failure(
            SourceError::malformed_source,
            "Natural Earth cartography DBF row count differs from canonical admin0 feature count"
        );
    }

    const auto mapcolor7_index = field_index(table, "MAPCOLOR7");
    if (!mapcolor7_index.has_value()) {
        return failure(
            SourceError::malformed_source,
            "Natural Earth admin0 DBF lacks MAPCOLOR7"
        );
    }
    const DbfField& field = table.fields[*mapcolor7_index];
    if (field.type != 'N' || field.decimal_count != 0U) {
        return failure(
            SourceError::malformed_source,
            "Natural Earth MAPCOLOR7 field has an unexpected DBF type"
        );
    }

    for (std::size_t index = 0U; index < result.features.size(); ++index) {
        const DbfRecord& record = table.records[index];
        if (record.deleted || *mapcolor7_index >= record.values.size()) {
            return failure(
                SourceError::malformed_source,
                "Natural Earth MAPCOLOR7 record alignment is incomplete"
            );
        }
        const auto value = positive_integer(record.values[*mapcolor7_index]);
        if (!value.has_value() || *value < 1 || *value > 7) {
            return failure(
                SourceError::malformed_source,
                "Natural Earth MAPCOLOR7 value lies outside the expected 1..7 palette domain"
            );
        }

        FeatureProperty property{};
        property.key = "mapcolor7";
        property.value = *value;
        result.features[index].properties.push_back(std::move(property));
    }

    const SourceError validation = validate_result(*this, request, result);
    if (validation != SourceError::none) {
        return failure(
            validation,
            "Natural Earth cartography adapter output failed common source-adapter validation"
        );
    }
    return result;
}

}  // namespace aeris::source
