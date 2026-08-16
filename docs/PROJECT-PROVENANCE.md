# AERIS — Project Provenance Persistence

**Status:** DRAFT IMPLEMENTATION CONTRACT  
**Implemented project draft:** schema generation 2 / format 0.2

This document defines the durable source-provenance layer inside `.aeris` and the implemented orchestration boundary that is allowed to move already verified source identity into it. It builds on `STORAGE-FOUNDATION.md` and remains pre-1.0: development files have no long-term compatibility promise yet.

## 1. Dependency direction

Persistence and cartographic source decoding remain separate layers.

```text
AERIS::core
  acquisition / VerifiedSnapshot / adapters / registry
             ↓
       AERIS::project
  verified-source orchestration
             ↓
AERIS::storage
  neutral provenance records / SQLite
```

`AERIS::storage` does not include `source::Result`, `VerifiedSnapshot`, Natural Earth types, or source capability enums. The storage DTO uses neutral fixed-width numeric capability/temporal values plus exact strings.

`AERIS::project` is the explicit composition layer that may depend on both `AERIS::core` and `AERIS::storage`. `AERIS_BUILD_PROJECT=ON` requires `AERIS_BUILD_STORAGE=ON`; Project CI contains a negative configure test for that rule.

This prevents SQLite schema details from becoming part of adapter contracts and prevents source adapters from acquiring a persistence dependency.

## 2. Explicit draft schema evolution

Adding canonical provenance is a file-format change, even before 1.0.

The project therefore advances from:

```text
schema generation 1 / draft format 0.1
```

to:

```text
schema generation 2 / draft format 0.2
```

AERIS does not silently reinterpret a generation-1 development file as generation 2. Pre-1.0 compatibility remains intentionally disposable until the stable format is frozen.

## 3. `aeris_source`

One row represents one immutable logical source snapshot used by the project.

Current fields are:

```text
source_id
adapter_id
capability_bits
temporal_class
provider
dataset
snapshot
dataset_version
source_uri
license_id
content_sha256
retrieved_at_utc
worldview
```

`source_id` is a project-level identity. It is not the same namespace as an adapter feature's `source_id` field.

The provenance content hash is the aggregate verified snapshot identity. It must use canonical lowercase 64-character SHA-256 syntax.

The retrieval timestamp uses the same canonical Gregorian UTC contract as project/session metadata.

An empty worldview string is valid when worldview is semantically not applicable. Identifiers that must carry identity are non-empty and bounded.

## 4. `aeris_source_resource`

The second table records the verified resource manifest beneath a source snapshot:

```text
source_id
logical_name
sha256
size_bytes (nullable)
```

The primary key is `(source_id, logical_name)`. The source foreign key uses `ON DELETE CASCADE`.

This table records **input content identity**, not yet general project resource storage.

Generation 2 does not store cache-local absolute paths, opaque downloader state, embedded payloads, media-type policy, or frozen/portable storage mode. A verified acquisition-relative path is also not copied into the project as a machine-local locator.

The aggregate snapshot hash produced by source verification already commits to the normalized portable relative path together with logical name, resource hash, and verified byte size. The project therefore preserves the verified aggregate identity without turning one cache layout into a canonical project path field.

## 5. Atomic source mutation

A new source snapshot is acknowledged only after one SQLite transaction has committed all of the following:

```text
aeris_source row
+ every aeris_source_resource row
+ exactly one project revision increment
+ project modified_utc update
```

A failure before commit leaves none of those semantic changes acknowledged. The open `ProjectStore` refreshes its metadata after a successful provenance commit.

## 6. Idempotent retry

`source_id` names immutable project provenance.

If an insertion is retried and an existing row with that `source_id` has exactly the same canonical source fields and resource manifest, the operation succeeds idempotently:

```text
inserted = false
durably_committed = false
project revision unchanged
```

Resource order in the caller is not semantic; resources are canonicalized by logical name before equality comparison and storage enumeration.

If the same `source_id` already names different immutable provenance, the operation fails with `record_exists`. A conflicting retry never overwrites the existing source.

When another project handle won the identical insertion concurrently, the idempotent loser refreshes its metadata before returning success so it does not continue with a stale project revision.

## 7. Bounds and canonical values

The current draft API rejects before mutation:

- zero capability mask;
- empty required identity fields;
- NUL-containing bounded text;
- overlong identity/URI fields;
- noncanonical SHA-256 spelling;
- impossible/noncanonical UTC timestamps;
- resource byte lengths outside SQLite signed-integer range;
- duplicate logical resource names;
- more than 4096 manifest resources for one source.

The same canonical constraints are checked again when rows are read from an existing project. SQLite types and integer ranges are not trusted merely because the database has an AERIS application identifier.

## 8. Read-only enumeration

Listing project source snapshots opens a separate SQLite read-only connection.

Before returning data, AERIS rechecks SQLite `quick_check`, project application identity, exact supported schema generation, equality with the UUID of the validated `ProjectStore`, and stored row types/ranges/canonical hashes and timestamps.

Sources enumerate by `source_id`; resources enumerate by `logical_name`. The conformance test compares project bytes before and after enumeration to ensure the read path does not implicitly reconfigure or rewrite the database.

## 9. Project open-time integrity

Generation-2 `ProjectStore::open` verifies the required source table/column surface before accepting the file and runs `PRAGMA foreign_key_check` in addition to SQLite quick-check and metadata validation.

Only after the external file has passed acceptance does AERIS configure the writable durability PRAGMAs.

## 10. Multi-handle semantics

Project mutations are serialized by SQLite `BEGIN IMMEDIATE` rather than by trusting an in-memory revision cache.

Metadata mutation reloads the current metadata row after obtaining the write transaction, applies the requested fields to that current state, and increments the then-current revision.

This prevents two independently opened project handles from both deriving `revision + 1` from stale cached state and prevents an update of one metadata field from reverting an unrelated field changed by another writer.

The provenance conformance suite similarly exercises concurrent identical snapshot insertion through two independent project handles. Exactly one transaction inserts; the other becomes an idempotent reader of the committed immutable record.

## 11. Verified-source project bridge

Product code does not need to construct a neutral `SourceSnapshotRecord` from UI text. The implemented project bridge accepts:

```text
validated ProjectStore
+ AdapterRegistry
+ SourceBinding
+ VerifiedSnapshot
+ project-local source_id
+ canonical mutation timestamp
```

It deliberately does **not** accept an arbitrary caller-supplied `source::Result`.

The bridge invokes `AdapterRegistry::load` itself, so ordinary adapter/request/capability/provenance validation and the verified aggregate content-hash check run before persistence is considered.

After registry success, the bridge additionally requires adapter provenance to describe the exact acquisition manifest. The provider, dataset, snapshot label, source URI, retrieval timestamp, and aggregate content SHA-256 must agree, and the binding snapshot label must equal the verified manifest snapshot.

This extra check is intentional: an adapter result may be internally valid according to the general registry contract while still describing a different acquisition identity than the verified bytes presented to the bridge.

The bridge maps only the selected binding capability bit, adapter temporal class, exact provenance, and verified resource-manifest identity into neutral storage values. It preserves storage's atomic and idempotent mutation semantics.

## 12. Executed bridge proof

Dedicated Project CI builds and tests the composition layer on Ubuntu, Windows, and macOS plus Linux ASan+UBSan.

The current contract tests prove:

- verified local bytes pass acquisition verification before the bridge can receive a `VerifiedSnapshot`;
- the registry is invoked by the bridge rather than bypassed;
- exact source/resource identity reaches `.aeris`;
- an exact retry does not create a second project revision;
- after the original `ProjectStore` is destroyed, reopening the project still exposes the committed provenance;
- a wrong pinned aggregate content hash fails closed before storage mutation;
- adapter dataset drift that survives ordinary registry validation is rejected by the bridge's acquisition-manifest cross-check;
- a binding snapshot label that disagrees with the verified manifest is rejected;
- configuring the project layer with storage disabled is rejected explicitly.

## 13. Current non-claims and next boundary

Schema generation 2 and the project bridge do not yet prove or implement canonical feature geometry persistence, layer ordering/configuration, styles, extension payloads, general embedded/external `aeris_resource` storage, frozen/portable projects, source removal/replacement semantics, or viewer source-management UX.

Most importantly, a successful provenance bridge operation means **the verified source identity is durably recorded**. It does not mean the adapter's feature geometry has been persisted.

The next persistence boundary should therefore be canonical geographic feature/ring storage tied to an immutable source record. Derived planar projection geometry and globe-view tessellation remain caches/results, not geographic truth.
