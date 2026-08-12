// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/util/sha256.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>

namespace aeris::util {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants{{
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
}};

[[nodiscard]] constexpr std::uint32_t rotate_right(const std::uint32_t value, const unsigned count) noexcept {
    return (value >> count) | (value << (32U - count));
}
[[nodiscard]] constexpr std::uint32_t choose(const std::uint32_t x, const std::uint32_t y, const std::uint32_t z) noexcept {
    return (x & y) ^ (~x & z);
}
[[nodiscard]] constexpr std::uint32_t majority(const std::uint32_t x, const std::uint32_t y, const std::uint32_t z) noexcept {
    return (x & y) ^ (x & z) ^ (y & z);
}
[[nodiscard]] constexpr std::uint32_t big_sigma0(const std::uint32_t x) noexcept {
    return rotate_right(x, 2U) ^ rotate_right(x, 13U) ^ rotate_right(x, 22U);
}
[[nodiscard]] constexpr std::uint32_t big_sigma1(const std::uint32_t x) noexcept {
    return rotate_right(x, 6U) ^ rotate_right(x, 11U) ^ rotate_right(x, 25U);
}
[[nodiscard]] constexpr std::uint32_t small_sigma0(const std::uint32_t x) noexcept {
    return rotate_right(x, 7U) ^ rotate_right(x, 18U) ^ (x >> 3U);
}
[[nodiscard]] constexpr std::uint32_t small_sigma1(const std::uint32_t x) noexcept {
    return rotate_right(x, 17U) ^ rotate_right(x, 19U) ^ (x >> 10U);
}
[[nodiscard]] std::uint32_t load_be_u32(const unsigned char* data) noexcept {
    return (static_cast<std::uint32_t>(data[0]) << 24U) |
           (static_cast<std::uint32_t>(data[1]) << 16U) |
           (static_cast<std::uint32_t>(data[2]) << 8U) |
           static_cast<std::uint32_t>(data[3]);
}
void store_be_u32(unsigned char* destination, const std::uint32_t value) noexcept {
    destination[0] = static_cast<unsigned char>((value >> 24U) & 0xffU);
    destination[1] = static_cast<unsigned char>((value >> 16U) & 0xffU);
    destination[2] = static_cast<unsigned char>((value >> 8U) & 0xffU);
    destination[3] = static_cast<unsigned char>(value & 0xffU);
}
void store_be_u64(unsigned char* destination, const std::uint64_t value) noexcept {
    for (unsigned index = 0U; index < 8U; ++index) {
        destination[index] = static_cast<unsigned char>((value >> ((7U - index) * 8U)) & 0xffU);
    }
}

}  // namespace

std::string Sha256Digest::hex() const {
    constexpr char alphabet[] = "0123456789abcdef";
    std::string output(bytes.size() * 2U, '0');
    for (std::size_t index = 0U; index < bytes.size(); ++index) {
        const unsigned value = bytes[index];
        output[index * 2U] = alphabet[(value >> 4U) & 0x0fU];
        output[index * 2U + 1U] = alphabet[value & 0x0fU];
    }
    return output;
}

Sha256::Sha256() noexcept
    : state_{{0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
              0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U}} {}

HashError Sha256::update(const void* const data, const std::size_t size) noexcept {
    if (finalized_) {
        return HashError::length_overflow;
    }
    if (size == 0U) {
        return HashError::none;
    }
    if (data == nullptr) {
        return HashError::io_error;
    }

    constexpr std::uint64_t max_message_bytes = std::numeric_limits<std::uint64_t>::max() / 8ULL;
    if (total_bytes_ > max_message_bytes ||
        static_cast<std::uint64_t>(size) > max_message_bytes - total_bytes_) {
        return HashError::length_overflow;
    }

    const auto* input = static_cast<const unsigned char*>(data);
    std::size_t remaining = size;
    total_bytes_ += static_cast<std::uint64_t>(size);

    if (buffered_bytes_ != 0U) {
        const std::size_t take = std::min(buffer_.size() - buffered_bytes_, remaining);
        std::memcpy(buffer_.data() + buffered_bytes_, input, take);
        buffered_bytes_ += take;
        input += take;
        remaining -= take;
        if (buffered_bytes_ == buffer_.size()) {
            transform(buffer_.data());
            buffered_bytes_ = 0U;
        }
    }

    while (remaining >= buffer_.size()) {
        transform(input);
        input += buffer_.size();
        remaining -= buffer_.size();
    }

    if (remaining != 0U) {
        std::memcpy(buffer_.data(), input, remaining);
        buffered_bytes_ = remaining;
    }
    return HashError::none;
}

void Sha256::transform(const unsigned char* const block) noexcept {
    std::array<std::uint32_t, 64> words{};
    for (std::size_t index = 0U; index < 16U; ++index) {
        words[index] = load_be_u32(block + index * 4U);
    }
    for (std::size_t index = 16U; index < words.size(); ++index) {
        words[index] = small_sigma1(words[index - 2U]) + words[index - 7U] +
                       small_sigma0(words[index - 15U]) + words[index - 16U];
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];

    for (std::size_t index = 0U; index < words.size(); ++index) {
        const std::uint32_t temp1 = h + big_sigma1(e) + choose(e, f, g) + kRoundConstants[index] + words[index];
        const std::uint32_t temp2 = big_sigma0(a) + majority(a, b, c);
        h = g; g = f; f = e; e = d + temp1; d = c; c = b; b = a; a = temp1 + temp2;
    }

    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

Sha256Digest Sha256::finalize() noexcept {
    if (finalized_) {
        return digest_;
    }

    const std::uint64_t bit_length = total_bytes_ * 8ULL;
    buffer_[buffered_bytes_++] = 0x80U;
    constexpr unsigned char zero_byte = static_cast<unsigned char>(0);
    if (buffered_bytes_ > 56U) {
        std::fill(
            buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_bytes_),
            buffer_.end(),
            zero_byte
        );
        transform(buffer_.data());
        buffered_bytes_ = 0U;
    }
    std::fill(
        buffer_.begin() + static_cast<std::ptrdiff_t>(buffered_bytes_),
        buffer_.begin() + 56,
        zero_byte
    );
    store_be_u64(buffer_.data() + 56U, bit_length);
    transform(buffer_.data());

    for (std::size_t index = 0U; index < state_.size(); ++index) {
        store_be_u32(digest_.bytes.data() + index * 4U, state_[index]);
    }
    finalized_ = true;
    buffered_bytes_ = 0U;
    return digest_;
}

Sha256Digest sha256_bytes(const void* const data, const std::size_t size) noexcept {
    Sha256 hash{};
    if (hash.update(data, size) != HashError::none) {
        return {};
    }
    return hash.finalize();
}

Sha256FileResult sha256_file(const std::filesystem::path& path) {
    Sha256FileResult result{};
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        result.error = HashError::io_error;
        return result;
    }

    Sha256 hash{};
    std::array<unsigned char, 64U * 1024U> buffer{};
    while (input) {
        input.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0) {
            const HashError error = hash.update(buffer.data(), static_cast<std::size_t>(count));
            if (error != HashError::none) {
                result.error = error;
                return result;
            }
        }
    }
    if (!input.eof()) {
        result.error = HashError::io_error;
        return result;
    }
    result.digest = hash.finalize();
    return result;
}

}  // namespace aeris::util
