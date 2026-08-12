// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace aeris::util {

enum class HashError : std::uint8_t {
    none = 0U,
    io_error,
    length_overflow,
};

struct Sha256Digest final {
    std::array<unsigned char, 32> bytes{};

    [[nodiscard]] std::string hex() const;
};

struct Sha256FileResult final {
    Sha256Digest digest{};
    HashError error = HashError::none;

    [[nodiscard]] constexpr bool ok() const noexcept {
        return error == HashError::none;
    }
};

class Sha256 final {
public:
    Sha256() noexcept;

    [[nodiscard]] HashError update(const void* data, std::size_t size) noexcept;
    [[nodiscard]] Sha256Digest finalize() noexcept;

private:
    void transform(const unsigned char* block) noexcept;

    std::array<std::uint32_t, 8> state_{};
    std::array<unsigned char, 64> buffer_{};
    std::uint64_t total_bytes_ = 0U;
    std::size_t buffered_bytes_ = 0U;
    bool finalized_ = false;
    Sha256Digest digest_{};
};

[[nodiscard]] Sha256Digest sha256_bytes(const void* data, std::size_t size) noexcept;
[[nodiscard]] Sha256FileResult sha256_file(const std::filesystem::path& path);

}  // namespace aeris::util
