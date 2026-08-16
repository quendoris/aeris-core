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

inline constexpr std::uint32_t kSessionApplicationId = 0x41455353U;  // "AESS"
inline constexpr int kSessionSchemaGeneration = 1;

struct SessionViewState {
    enum class Mode { globe, sinusoidal, mollweide };

    Mode mode{Mode::globe};
    double camera_longitude_rad{0.0};
    double camera_latitude_rad{0.0};
    double zoom{1.0};
};

struct SessionMetadata {
    std::string project_uuid;
    std::uint64_t revision{0};
    std::string modified_utc;
};

class SessionStore;

struct SessionStoreResult {
    Status status;
    std::unique_ptr<SessionStore> store;

    [[nodiscard]] bool ok() const noexcept { return status.ok() && store != nullptr; }
};

struct SessionViewResult {
    Status status;
    std::optional<SessionViewState> value;
    [[nodiscard]] bool ok() const noexcept { return status.ok(); }
};

class SessionStore final {
public:
    ~SessionStore();
    SessionStore(SessionStore&&) noexcept;
    SessionStore& operator=(SessionStore&&) noexcept;
    SessionStore(const SessionStore&) = delete;
    SessionStore& operator=(const SessionStore&) = delete;

    static SessionStoreResult open_or_create(
        const std::filesystem::path& project_path,
        std::string_view project_uuid,
        std::string_view timestamp_utc);

    [[nodiscard]] const SessionMetadata& metadata() const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept;

    SessionViewResult read_view_state() const;
    Status write_view_state(const SessionViewState& state, std::string_view modified_utc);
    Status verify_integrity() const;

private:
    class Impl;
    explicit SessionStore(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::filesystem::path adjacent_session_path(const std::filesystem::path& project_path);

}  // namespace aeris::storage
