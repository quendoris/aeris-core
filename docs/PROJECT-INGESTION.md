# AERIS — Atomic Verified Dataset Ingestion

**Status:** DRAFT IMPLEMENTATION CONTRACT  
**Project format:** schema generation 3 / format 0.3 (unchanged)

This contract defines the first product-level operation that may acknowledge a verified cartographic source as fully ingested into `.aeris`.

## 1. Product meaning

A source is not fully ingested merely because its provenance row exists.

For the current generation-3 project model, one acknowledged dataset consists of:

```text
verified source provenance
+ verified resource manifest identity
+ explicit canonical geometry marker
+ every canonical feature/ring payload
+ exactly one project revision
```

The product ingestion path must publish all of that state in one SQLite transaction or publish none of it.

## 2. Dependency direction

The transaction does not collapse source, project, and storage into one layer.

```text
VerifiedSnapshot + SourceBinding
          ↓
     AdapterRegistry
          ↓
validated source::Result
          ↓
     AERIS::project
  exact mapping/cross-check
          ↓
neutral SourceDatasetRecord
          ↓
     AERIS::storage
 one SQLite transaction
```

`AERIS::core` remains SQLite-free. `AERIS::project` does not know the SQLite schema or use `sqlite3*`. `AERIS::storage` does not know adapter or Natural Earth types.

## 3. Protected public path

The full product API accepts the same protected inputs as the verified provenance bridge:

```text
validated ProjectStore
+ AdapterRegistry
+ VerifiedSnapshot
+ SourceBinding
+ project-local source_id
+ canonical mutation timestamp
```

It deliberately does not accept an arbitrary caller-created `source::Result`.

The shared verified-source preparation path invokes `AdapterRegistry::load`, preserves registry errors, and cross-checks the resulting provenance against the exact acquisition manifest before any project mapping or storage mutation occurs.

The legacy provenance-only bridge reuses this same preparation function; there are not two independent interpretations of verified acquisition identity.

## 4. Canonical geometry mapping

After registry validation, the project layer transfers normalized feature geometry into storage-native generation-3 records without recomputing topology:

```text
Feature.stable_id        -> canonical stable feature ID
Feature.source_id        -> source feature identity
FeatureRing.role         -> stored source ring role
LinearRing.interior_side -> stored interior side
longitude_winding        -> stored int32 winding
closing_longitude_rad    -> canonical closing longitude
vertices                 -> WGS84 lon/lat radians
```

The mapping is lossless for the current source geometry contract. It does not project, tessellate, principalize, infer winding, or choose a new ring interior.

Storage remains the final canonical-format trust boundary and validates the mapped WGS84/topology record again before mutation.

## 5. One transaction

`store_source_dataset()` first canonicalizes/validates provenance and geometry using the same internal serializers as the established low-level APIs. It then acquires one SQLite `BEGIN IMMEDIATE` transaction.

For a new source, the transaction contains:

```text
aeris_source
+ aeris_source_resource rows
+ aeris_source_geometry marker
+ aeris_feature rows
+ aeris_feature_ring rows/BLOBs
+ one aeris_meta revision increment
+ one modified_utc update
```

Only one `COMMIT` acknowledges the dataset.

A failure at any point before that commit rolls back all earlier writes in the same transaction. In particular, a late geometry SQL failure after provenance rows were already inserted must leave the provenance catalog and project revision unchanged.

## 6. Immutable state machine

The combined operation inspects provenance and geometry under the same write transaction.

```text
provenance absent    + geometry absent    -> insert both
provenance identical + geometry identical -> idempotent success
anything else                              -> fail closed
```

This includes deliberately rejecting pre-existing low-level partial state such as `provenance identical + geometry absent`.

The operation does not silently complete such a source because doing so would falsely reclassify historical two-transaction state as one atomic ingestion.

## 7. Empty verified sources

A successful adapter result may contain zero features.

That is still a complete dataset ingestion. The transaction stores provenance plus an explicit canonical geometry marker with `feature_count = 0` and advances one project revision.

Therefore:

```text
no geometry marker = geometry not recorded
empty marker        = verified geometry recorded and empty
```

## 8. Idempotence and concurrency

An exact retry of a fully stored dataset is successful without another commit or project revision.

Writers are serialized with SQLite `BEGIN IMMEDIATE`, not with an in-memory mutex or cached revision assumption. Concurrent identical ingestions through independent project handles must converge to one inserted dataset and one project revision; the losing writer observes the complete committed state and returns idempotently.

A conflicting immutable provenance or geometry identity is never overwritten.

## 9. Executable proofs

The storage/project test suites cover:

- one verified dataset produces provenance and geometry with exactly one revision;
- exact retry is idempotent;
- close/reopen preserves the complete dataset;
- explicit empty geometry is atomic;
- invalid canonical geometry leaves provenance absent;
- a registry-valid adapter result that violates project geometry identity constraints is rejected without leaking provenance;
- provenance-only low-level history is not silently completed by the product API;
- a deliberately induced late geometry SQL failure rolls back provenance rows already written earlier in the same transaction;
- deep project integrity succeeds for a valid combined dataset;
- concurrent identical combined ingestion converges to one durable dataset/revision.

The late-failure fixture is a transaction rollback proof, not a claim about sudden machine power loss. The separate `_Exit()` durability fixture continues to define the narrower process-death evidence already documented by the storage foundation.

## 10. Low-level APIs remain intentional

`store_source_snapshot()` and `store_source_geometry()` remain available as lower-level storage capabilities for diagnostics, fixtures, controlled import/recovery work, and future tooling.

Their existence does not make two sequential calls equivalent to `store_source_dataset()`.

Product code that intends to acknowledge a complete source ingestion must use the combined path.

## 11. Current non-claims

This milestone does not yet implement:

- arbitrary feature attribute/property persistence;
- canonical layer ordering/configuration;
- styles, labels, or symbols;
- replacement/removal UX for immutable source datasets;
- project-level source-management UI;
- frozen/portable embedded resource payloads;
- whole-machine power-loss proof;
- draft migration from 0.2 or earlier;
- canonical projected render caches.

The next product layer may build source/layer management and viewer lifecycle on this ingestion primitive without weakening its transaction boundary.
