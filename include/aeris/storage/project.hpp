// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "aeris/storage/status.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace aeris::storage {

inline constexpr std::uint32_t kProjectApplicationId = 0x41455249U;  // "AERI"
inline constexpr int kProjectSchemaGeneration = 8;
inline constexpr int kDraftFormatMajor = 0;
inline constexpr int kDraftFormatMinor = 8;

struct ProjectMetadata {
    std::string project_uuid;
    int format_major{kDraftFormatMajor};
    int format_minor{kDraftFormatMinor};
    std::uint64_t revision{0};
    std::string created_utc;
    std::string modified_utc;
    std::string producer;
    std::string producer_version;
    std::string projection_id;
    std::string worldview_id;
    bool frozen{false};
};

struct ProjectCreateOptions {
    std::string timestamp_utc;
    std::string project_uuid;  // Empty requests generated UUIDv4.
    std::string producer{"aeris"};
    std::string producer_version{"0.1.0"};

    // Pre-1.0 compatibility field. Structured projection state is authoritative
    // from generation 7 onward, so creation accepts only explicit unspecified
    // here; use set_project_projection() after creation for a concrete model.
    std::string projection_id{"aeris.projection.unspecified"};
    std::string worldview_id{"unspecified"};

    // Retained during the pre-1.0 API transition only. Creating directly as
    // frozen is rejected: portable state is established by the verified
    // freeze_project() resource contract after creation.
    bool frozen{false};
};

struct ProjectMetadataUpdate {
    std::string modified_utc;

    // Retained only to fail closed at the old API boundary. Structured
    // projection state must be changed with set_project_projection(), which
    // updates the authoritative singleton and this metadata summary atomically.
    std::optional<std::string> projection_id;
    std::optional<std::string> worldview_id;

    // Retained during the pre-1.0 API transition only. Direct mutation is
    // rejected because frozen is a verified resource-state invariant, not a
    // caller-controlled metadata flag.
    std::optional<bool> frozen;
};

class ProjectStore;

struct ProjectStoreResult {
    Status status;
    std::unique_ptr<ProjectStore> store;

    [[nodiscard]] bool ok() const noexcept { return status.ok() && store != nullptr; }
};

class ProjectStore final {
public:
    ~ProjectStore();
    ProjectStore(ProjectStore&&) noexcept;
    ProjectStore& operator=(ProjectStore&&) noexcept;
    ProjectStore(const ProjectStore&) = delete;
    ProjectStore& operator=(const ProjectStore&) = delete;

    static ProjectStoreResult create(const std::filesystem::path& path, const ProjectCreateOptions& options);
    static ProjectStoreResult open(const std::filesystem::path& path);

    [[nodiscard]] const ProjectMetadata& metadata() const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

    Status update_metadata(const ProjectMetadataUpdate& update);
    Status refresh_metadata();
    Status verify_integrity() const;

private:
    class Impl;
    explicit ProjectStore(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] bool is_canonical_uuid(std::string_view value) noexcept;
[[nodiscard]] bool is_canonical_utc_timestamp(std::string_view value) noexcept;

}  // namespace aeris::storage
