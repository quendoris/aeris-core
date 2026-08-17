# AERIS — Canonical Project Geometry

**Status:** DRAFT IMPLEMENTATION CONTRACT  
**Implemented project draft:** schema generation 3 / format 0.3

This document defines the first canonical feature-geometry representation inside `.aeris`. It builds on `PROJECT-PROVENANCE.md` and remains pre-1.0: draft files have no long-term migration promise until the stable format is frozen.

## 1. Authority boundary

Canonical project geometry is geographic source truth, not render output.

Generation 3 stores normalized WGS84 geodetic coordinates in radians. It does **not** store Sinusoidal, Mollweide, globe-screen, viewport, tessellation, GPU, Qt, or export coordinates as canonical feature geometry.

Projection and view geometry remain derived state and may be regenerated from the canonical project representation.

`AERIS::storage` also remains independent of `source::Feature` and renderer types. Its geometry API uses storage-native records so the SQLite format cannot become the ABI of the in-memory source model.

## 2. Explicit draft evolution

Canonical geometry changes the file format, so the project advances from:

```text
schema generation 2 / draft format 0.2
```

to:

```text
schema generation 3 / draft format 0.3
```

A generation-2 development file is not silently reinterpreted as generation 3. Pre-1.0 draft compatibility remains intentionally disposable.

## 3. Geometry model identity

Every persisted source geometry set records explicit interpretation identifiers:

```text
model_id    = aeris.geometry.wgs84-linear-ring.v1
encoding_id = aeris.coord.ieee754-binary64-le-radians.v1
```

A reader must reject an unknown model or encoding rather than guessing.

The current implementation requires an IEC 60559 / IEEE-754 64-bit `double` implementation at build time.

## 4. Source geometry marker

`aeris_source_geometry` contains one row for a source whose canonical geometry set has been durably recorded:

```text
source_id
model_id
encoding_id
feature_count
```

The marker is semantic. In particular:

```text
no marker       = geometry has not been recorded for this source
marker + count 0 = geometry was explicitly recorded and is empty
```

This distinction prevents an empty verified source result from being confused with a partial ingestion.

The marker references an already-persisted `aeris_source` provenance row.

## 5. Feature identity

`aeris_feature` records:

```text
source_id
stable_id
source_feature_id
ring_count
```

`stable_id` is the canonical project feature identity supplied by the normalized source result. `source_feature_id` preserves the adapter/source-record identity.

Both are unique within one source geometry set. Feature rows enumerate deterministically by `stable_id`; caller input order is not semantic.

Ring order **is** semantic and is preserved by `ring_index`.

## 6. Ring topology

`aeris_feature_ring` records each ordered canonical ring using explicit topology fields:

```text
source_id
stable_id
ring_index
role
interior_side
longitude_winding
closing_longitude_f64le
vertex_count
vertices_f64le
```

`role` distinguishes exterior and interior source rings.

`interior_side` preserves the canonical WGS84 ring interior convention (`unspecified`, `left`, or `right`). A nonzero longitude winding requires an explicit interior side.

`longitude_winding` and `closing_longitude_f64le` preserve unwrapped antimeridian/polar topology that cannot be reconstructed safely from principalized vertices alone.

Ring indices must be contiguous from zero. A sparse or reordered index sequence is invalid even when SQLite row counts and primary keys remain structurally valid.

## 7. Coordinate encoding

Coordinates are not stored one SQLite row per vertex. Each ring stores a compact versioned BLOB:

```text
vertices_f64le =
    longitude_0 binary64 little-endian
    latitude_0  binary64 little-endian
    longitude_1 binary64 little-endian
    latitude_1  binary64 little-endian
    ...
```

Units are radians.

`closing_longitude_f64le` is one binary64 little-endian value using the same coordinate convention.

The BLOB length is constrained to exactly:

```text
vertex_count * 16 bytes
```

This format is independent of host endianness and C++ object layout. No native struct dump or ABI serialization is canonical.

Finite binary64 values are preserved exactly. Mathematical zero is canonicalized so both `-0.0` and `+0.0` persist and compare as `+0.0`.

NaN and infinity are invalid canonical coordinates.

## 8. Canonical WGS84 constraints

The draft reader/writer enforces the same topological shape expected from normalized AERIS rings:

- at least three vertices per ring;
- latitude in `[-pi/2, +pi/2]`;
- finite coordinates and closing longitude;
- first longitude in `(-pi, +pi]`;
- every consecutive unwrapped longitude delta within one half-turn;
- numerically ambiguous exact half-turn edges rejected;
- closing edge subject to the same rule;
- `(closing_longitude - first_longitude) / 2pi` must agree with the stored integer winding;
- nonzero winding requires an explicit interior side.

These checks are performed on write and again when external project rows are read. A valid SQLite type is not treated as proof of valid cartographic geometry.

## 9. Draft bounds

Generation 3 currently bounds one stored geometry set to:

```text
features per source <= 1,000,000
rings per feature   <= 65,535
vertices per ring   <= 4,194,304
identifier bytes    <= 255
```

The bounds are part of defensive parsing, not a statement that recommended datasets should approach them.

## 10. Atomic geometry mutation

Canonical geometry for one source is immutable once recorded in this draft.

A new geometry set is acknowledged only after one SQLite transaction commits:

```text
source geometry marker
+ every feature row
+ every ring row and coordinate BLOB
+ exactly one project revision increment
+ project modified_utc update
```

A failure before commit acknowledges none of those changes.

An exact retry succeeds idempotently with:

```text
inserted = false
durably_committed = false
project revision unchanged
```

A different geometry set under the same source ID fails with `record_exists`; it is never silently replaced.

The geometry mutation requires the immutable source provenance parent to exist first. This storage-level API is intentionally lower-level than the future product ingestion transaction.

## 11. Read path

Two read surfaces are currently exposed:

- a metadata-only source geometry index, used to enumerate stable feature identities and ring counts without decoding all coordinates;
- lazy loading of one feature's complete canonical ring geometry.

Both use read-only SQLite connections and validate model, encoding, numeric types, bounds and topology before returning records.

The conformance suite checks that ordinary index/load reads do not rewrite project bytes.

## 12. Hostile-file semantics

The geometry contract does not rely on SQLite structural checks alone.

Test fixtures deliberately create SQLite-structurally readable projects and then corrupt semantic payloads below the AERIS API, including:

- an unknown geometry model identifier;
- sparse ring indices;
- a NaN injected into an otherwise length-correct coordinate BLOB.

Canonical geometry readers must reject these as schema-invalid data.

## 13. Current non-claims

Draft 0.3 does not yet claim:

- one atomic product transaction that persists verified source provenance and its geometry together;
- persistence of arbitrary feature attributes/properties;
- canonical layer ordering or layer configuration;
- styles, labels, symbols, or extension payloads;
- editing/replacement semantics for an existing immutable source geometry set;
- general embedded/external resource payload storage;
- frozen/portable projects;
- migration from draft 0.2;
- projected render caches as canonical data.

Most importantly, the existence of the storage geometry API does not mean callers should manually manufacture project geometry from UI data. The next orchestration boundary must map only a successfully validated source result and commit its provenance plus canonical geometry without exposing a partial acknowledged state.
