// SPDX-FileCopyrightText: 2026 quendoris
// SPDX-License-Identifier: AGPL-3.0-only

#include "aeris/source/registry.hpp"

#include <utility>

namespace aeris::source {

RegistryError AdapterRegistry::add(std::unique_ptr<Adapter> adapter) {
    if (!adapter) {
        return RegistryError::invalid_adapter;
    }
    const AdapterDescriptor descriptor = adapter->descriptor();
    if (descriptor.adapter_id.empty() || descriptor.provider.empty() || descriptor.capabilities == 0U) {
        return RegistryError::invalid_adapter;
    }
    if (find(descriptor.adapter_id) != nullptr) {
        return RegistryError::duplicate_adapter_id;
    }
    adapters_.push_back(std::move(adapter));
    return RegistryError::none;
}

const Adapter* AdapterRegistry::find(const std::string_view adapter_id) const noexcept {
    for (const auto& adapter : adapters_) {
        if (adapter->descriptor().adapter_id == adapter_id) {
            return adapter.get();
        }
    }
    return nullptr;
}

RegistryLoadResult AdapterRegistry::load(
    const SourceBinding& binding,
    const VerifiedSnapshot& snapshot
) const {
    RegistryLoadResult output{};
    if (binding.adapter_id.empty() || binding.capability == Capability::none || binding.snapshot.empty()) {
        output.error = RegistryError::invalid_binding;
        output.diagnostic = "source binding is incomplete";
        return output;
    }

    const Adapter* adapter = find(binding.adapter_id);
    if (adapter == nullptr) {
        output.error = RegistryError::adapter_not_found;
        output.diagnostic = "source adapter is not registered";
        return output;
    }
    if (!has_capability(adapter->descriptor().capabilities, binding.capability)) {
        output.error = RegistryError::unsupported_capability;
        output.diagnostic = "source binding requests a capability the adapter does not advertise";
        return output;
    }
    if (!binding.expected_content_sha256.empty() &&
        binding.expected_content_sha256 != snapshot.content_sha256()) {
        output.error = RegistryError::snapshot_content_mismatch;
        output.diagnostic = "verified snapshot content does not match the project binding";
        return output;
    }

    const Request request{binding.capability, binding.snapshot, binding.worldview};
    output.source = adapter->load(snapshot, request);
    if (!output.source.ok()) {
        output.error = RegistryError::adapter_failed;
        output.source_error = output.source.error;
        output.diagnostic = output.source.diagnostic;
        return output;
    }

    const SourceError validation = validate_result(*adapter, request, output.source);
    if (validation != SourceError::none ||
        output.source.provenance.content_sha256 != snapshot.content_sha256()) {
        output.error = RegistryError::result_validation_failed;
        output.source_error = validation != SourceError::none
            ? validation
            : SourceError::content_hash_mismatch;
        output.diagnostic = "adapter result failed registry-level provenance validation";
        return output;
    }

    return output;
}

}  // namespace aeris::source
