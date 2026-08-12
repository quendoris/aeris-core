# AERIS — Source Acquisition and Normalization Pipeline

**Status:** DRAFT IMPLEMENTATION CONTRACT

AERIS separates **transport**, **snapshot verification**, and **provider-specific decoding**.

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
                v
          SourceAdapter
                |
                v
      canonical AERIS geometry
```

## 1. Acquisition is not decoding

An acquisition backend may download a release, copy a directory, unpack an archive, read removable media, or recover an exact snapshot from a content-addressed cache.

It does not interpret coastlines, borders, political policy, CRS semantics, or polygon topology.

A source adapter does not perform network access. It receives a verified local snapshot and interprets only the provider/dataset format it declares.

This division keeps offline use, reproducible builds, testing, and future transport mechanisms independent of map semantics.

## 2. Snapshot manifest

A snapshot manifest identifies one exact dataset snapshot and every byte resource needed by an adapter.

Each resource declares:

```text
logical_name
relative_path
sha256
optional exact size
```

Logical names are adapter-facing roles such as `geometry.shp`, `crs.prj`, or `dataset.version`. The adapter does not need to know how the files were acquired.

The manifest also records:

```text
provider
dataset
snapshot
source_uri
retrieved_at_utc
```

Acquisition time is supplied by the acquisition layer. Adapters must not call the system clock to manufacture provenance during decoding.

## 3. Verification boundary

`verify_local_snapshot()` is the common trust boundary for all acquisition mechanisms.

It MUST reject:

- incomplete manifests;
- absolute or traversal resource paths;
- symlink/path resolution escaping the snapshot root;
- duplicate logical resource names;
- duplicate normalized paths;
- missing resources;
- non-regular-file resources;
- exact-size mismatches where a size is specified;
- non-canonical or mismatching SHA-256 values.

Only a successful verification operation can construct a `VerifiedSnapshot`.

## 4. Aggregate content identity

A dataset may require several resources. AERIS therefore does not identify a snapshot merely by the hash of its primary geometry file.

After every resource is verified, AERIS computes a deterministic aggregate SHA-256 over the sorted logical resource manifest using a versioned domain separator.

The aggregate depends on:

- logical resource names;
- normalized portable relative paths;
- verified per-resource SHA-256 hashes;
- exact observed byte sizes.

It deliberately does not depend on retrieval time or transport location. The same resource set acquired on two machines therefore has the same content identity.

Individual resource hashes remain available in the snapshot manifest.

## 5. Immutability contract

`VerifiedSnapshot` means **verified at the boundary**, not that the host filesystem has magically become immutable.

Production acquisition/cache backends should materialize snapshots into content-addressed, non-user-edited storage and avoid mutating verified paths in place. If hostile concurrent filesystem mutation is part of a future threat model, AERIS must use stronger handle-based or copied-content semantics rather than pretending pathname verification alone eliminates TOCTOU races.

This limitation is explicit and must not be hidden behind the type name.

## 6. Source adapters

A source adapter consumes a `VerifiedSnapshot` and must still validate semantic requirements that byte hashing cannot establish, including:

- expected provider and dataset identity;
- required logical resources;
- CRS meaning;
- dataset-internal version metadata;
- source geometry structure;
- edge semantics and normalization;
- requested worldview/capability support.

The adapter emits canonical AERIS features and provenance whose `content_sha256` identifies the whole verified resource set.

## 7. Principle

Transport changes without changing adapters.

Providers change without changing projection or rendering.

Snapshots change without changing the project format.

AERIS owns the stable contracts between all three.
