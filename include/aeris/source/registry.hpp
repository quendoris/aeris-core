// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include "aeris/source/adapter.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aeris::source {

enum class RegistryError : std::uint8_t {
    none = 0U,
    invalid_adapter,
    duplicate_adapter_id,
    invalid_binding,
    adapter_not_found,
    unsupported_capability,
    snapshot_content_mismatch,
    adapter_failed,
    result_validation_failed,
};

struct SourceBinding final {
    std::string adapter_id;
    Capability capability = Capability::none;
    std::string snapshot;
    std::string worldview;
    std::string expected_content_sha256;
};

struct RegistryLoadResult final {
    Result source{};
    RegistryError error = RegistryError::none;
    SourceError source_error = SourceError::none;
    std::string diagnostic;

    [[nodiscard]] bool ok() const noexcept {
        return error == RegistryError::none && source.ok();
    }
};

class AdapterRegistry final {
public:
    [[nodiscard]] RegistryError add(std::unique_ptr<Adapter> adapter);
    [[nodiscard]] const Adapter* find(std::string_view adapter_id) const noexcept;

    [[nodiscard]] RegistryLoadResult load(
        const SourceBinding& binding,
        const VerifiedSnapshot& snapshot
    ) const;

private:
    std::vector<std::unique_ptr<Adapter>> adapters_;
};

}  // namespace aeris::source
