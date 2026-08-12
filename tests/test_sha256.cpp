// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/util/sha256.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect_true(const std::string_view name, const bool condition) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL " << name << '\n';
    }
}

void expect_hash(
    const std::string_view name,
    const std::string_view input,
    const std::string_view expected
) {
    const auto digest = aeris::util::sha256_bytes(input.data(), input.size());
    if (digest.hex() != expected) {
        ++failures;
        std::cerr << "FAIL " << name << ": actual=" << digest.hex()
                  << " expected=" << expected << '\n';
    }
}

void test_known_vectors() {
    expect_hash(
        "empty SHA-256",
        "",
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
    );
    expect_hash(
        "abc SHA-256",
        "abc",
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    );
    expect_hash(
        "multi-block SHA-256",
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"
    );
}

void test_incremental_equivalence() {
    aeris::util::Sha256 hash{};
    const std::string input = "the same bytes must hash identically regardless of chunking";
    for (const char character : input) {
        expect_true("incremental update succeeds", hash.update(&character, 1U) == aeris::util::HashError::none);
    }
    expect_true(
        "incremental hash equals one-shot",
        hash.finalize().hex() == aeris::util::sha256_bytes(input.data(), input.size()).hex()
    );
}

void test_file_hash() {
    const auto stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("aeris-sha256-" + std::to_string(stamp) + ".bin");
    {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << "abc";
    }

    const auto result = aeris::util::sha256_file(path);
    expect_true("file hash succeeds", result.ok());
    if (result.ok()) {
        expect_true(
            "file hash matches vector",
            result.digest.hex() ==
                "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
        );
    }

    std::error_code ignored{};
    std::filesystem::remove(path, ignored);
}

}  // namespace

int main() {
    test_known_vectors();
    test_incremental_equivalence();
    test_file_hash();

    if (failures != 0) {
        std::cerr << failures << " test assertion(s) failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "sha256: PASS\n";
    return EXIT_SUCCESS;
}
