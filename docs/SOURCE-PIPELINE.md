# AERIS — Source Acquisition and Normalization Pipeline

**Status:** DRAFT IMPLEMENTATION CONTRACT

AERIS separates transport, snapshot verification, adapter decoding, and source binding.

```text
network / archive / USB / local cache
                |
                v
           acquisition
                |
                v
       immutable local bytes
                |
                v
       snapshot verification
      size + SHA-256 + path safety
                |
                v
         VerifiedSnapshot
                |
                +------------------+
                |                  |
                v                  v
         SourceAdapter       project binding
      stateless decoder   adapter/snapshot/hash
                |                  |
                +--------+---------+
                         v
                 AdapterRegistry
                         |
                         v
                canonical features
```

## 1. Acquisition is not decoding

An acquisition backend may download a release, copy a directory, unpack an archive, read removable media, or recover an exact snapshot from a content-addressed cache. It does not interpret map semantics.

A source adapter performs no transport. It receives a `VerifiedSnapshot` and decodes any compatible snapshot of the provider/dataset format it declares.

## 2. Adapters are snapshot-independent

An adapter ID identifies a decoding/normalization contract, not a particular year of the world.

```text
adapter_id = natural-earth.ne-110m-land.shapefile.v1
snapshot   = v5.1.2
```

A later compatible Natural Earth snapshot should reuse the same adapter. If the source format or semantic contract changes incompatibly, that is when a new adapter version is justified.

## 3. Snapshot manifest and verification

Each resource declares a logical name, relative path, SHA-256, and optionally an exact size. `verify_local_snapshot()` rejects incomplete manifests, traversal/escape paths, duplicate resources, missing/non-regular files, size mismatches, and hash mismatches.

Only successful verification can construct a `VerifiedSnapshot`.

## 4. Aggregate content identity

AERIS computes a deterministic aggregate SHA-256 over the verified resource set. The aggregate includes logical names, portable normalized paths, resource hashes, and observed sizes. It excludes retrieval time and transport URI so identical bytes acquired through different routes retain identical content identity.

## 5. Project source binding

A persistent project does not store provider-specific decoder state. It stores a source binding containing at minimum:

```text
adapter_id
capability
snapshot
worldview
expected_content_sha256
```

The registry resolves the adapter, verifies that the capability is advertised, checks the pinned snapshot content identity, invokes the adapter, and re-validates returned provenance.

Thus a project can say exactly **which world snapshot it used** without embedding assumptions about how that provider works.

## 6. Freshness is not truth replacement

A newer snapshot may be offered by acquisition/UI logic, but it does not overwrite a saved binding. Updating a source is an explicit project mutation producing a new pinned content identity.

A different worldview is likewise a different source binding choice, not a silently newer truth.

## 7. Filesystem limitation

`VerifiedSnapshot` means verified at the boundary. Production caches should materialize snapshots into content-addressed, non-user-edited storage. Path verification alone is not claimed to defeat hostile concurrent filesystem mutation; a stronger threat model would require handle-based or copied-content semantics.

## 8. Principle

Transport can change without changing adapters.

Snapshots can change without changing adapters.

Providers can change without changing projection or rendering.

AERIS owns the stable contracts between all of them.
