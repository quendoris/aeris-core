// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/storage/resource.hpp"

#include <chrono>
#include <fstream>
#include <limits>
#include <random>
#include <system_error>

namespace aeris::storage {
namespace {

class EphemeralResourceStaging final {
public:
    EphemeralResourceStaging() = default;
    EphemeralResourceStaging(const EphemeralResourceStaging&) = delete;
    EphemeralResourceStaging& operator=(const EphemeralResourceStaging&) = delete;

    ~EphemeralResourceStaging() {
        if (directory_.empty()) return;
        std::error_code ignored;
        (void)std::filesystem::remove_all(directory_, ignored);
    }

    [[nodiscard]] Status create() {
        std::error_code ec;
        const std::filesystem::path root = std::filesystem::temp_directory_path(ec);
        if (ec || root.empty()) {
            return {StorageError::filesystem_failure,
                    "could not resolve temporary directory for generated resource staging"};
        }

        std::random_device random;
        const auto stamp = std::chrono::high_resolution_clock::now()
                               .time_since_epoch()
                               .count();
        for (unsigned attempt = 0U; attempt < 16U; ++attempt) {
            const std::uint64_t nonce =
                (static_cast<std::uint64_t>(random()) << 32U) ^
                static_cast<std::uint64_t>(random());
            const std::filesystem::path candidate = root /
                ("aeris-resource-bytes-" + std::to_string(stamp) + "-" +
                 std::to_string(nonce) + "-" + std::to_string(attempt));
            ec.clear();
            if (std::filesystem::create_directory(candidate, ec)) {
                directory_ = candidate;
                return Status::success();
            }
            if (ec && ec != std::make_error_code(std::errc::file_exists)) {
                return {StorageError::filesystem_failure,
                        "could not create generated resource staging directory: " +
                            ec.message()};
            }
        }
        return {StorageError::filesystem_failure,
                "could not allocate unique generated resource staging directory"};
    }

    [[nodiscard]] std::filesystem::path payload_path() const {
        return directory_ / "payload.bin";
    }

private:
    std::filesystem::path directory_;
};

}  // namespace

ResourceMutationResult embed_resource_bytes(
    ProjectStore& project,
    const ProjectResourceIdentity& resource,
    const void* data,
    const std::size_t size,
    const std::string_view modified_utc) {
    if ((size != 0U && data == nullptr) ||
        size > static_cast<std::size_t>(std::numeric_limits<std::streamsize>::max())) {
        return {{StorageError::invalid_argument,
                 "generated embedded resource requires addressable bytes within stream bounds"},
                false, false, false};
    }
    if (resource.size_bytes != static_cast<std::uint64_t>(size)) {
        return {{StorageError::integrity_failed,
                 "generated embedded resource byte count differs from declared immutable identity"},
                false, false, false};
    }

    EphemeralResourceStaging staging;
    Status status = staging.create();
    if (!status) return {std::move(status), false, false, false};

    const std::filesystem::path payload = staging.payload_path();
    {
        std::ofstream output(payload, std::ios::binary | std::ios::trunc);
        if (!output) {
            return {{StorageError::filesystem_failure,
                     "could not open generated resource staging payload"},
                    false, false, false};
        }
        if (size != 0U) {
            output.write(
                static_cast<const char*>(data),
                static_cast<std::streamsize>(size));
        }
        output.flush();
        if (!output) {
            return {{StorageError::filesystem_failure,
                     "could not write generated resource staging payload"},
                    false, false, false};
        }
    }

    return embed_resource_file(project, resource, payload, modified_utc);
}

}  // namespace aeris::storage
