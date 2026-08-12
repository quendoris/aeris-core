# AERIS Project Format

**Status:** DRAFT — NOT YET A STABLE FILE FORMAT  
**Target stable version:** 1.0  
**Canonical extension:** `.aeris`

This document defines the design contract for the AERIS project file.

Until this specification is explicitly marked **AERIS Project Format 1.0 — STABLE**, draft files are development artifacts and carry no long-term compatibility promise.

Once 1.0 is frozen, its semantics become permanent.

---

## 1. Goals

The format exists to provide one durable, portable, inspectable project document that can survive application upgrades, operating-system changes, crashes, and long periods without active development.

A stable `.aeris` file must be:

- self-describing enough to validate without application-specific object serialization;
- transactionally writable;
- inspectable with ordinary SQLite tooling;
- independent of Python, Qt, Rust, Java, C++ ABI, or any other implementation runtime;
- capable of referencing immutable external resources;
- capable of embedding all required resources for frozen/offline reproduction;
- explicit about provenance and source licensing;
- independent from one specific renderer or UI layout;
- able to retain normalized geographic geometry prior to final map projection;
- backward-readable by future AERIS releases.

The format is intentionally small. Features that do not belong to the project document must stay outside it.

---

## 2. Container

An `.aeris` project is a **SQLite 3 database**.

AERIS does not invent a custom binary container around SQLite.

The stable format must not use implementation-native serialization such as:

- Python pickle or marshal;
- Qt object streams;
- Java object serialization;
- ABI-dependent struct dumps;
- language-specific binary object graphs;
- opaque framework persistence whose semantics depend on a particular library version.

Structured subdocuments may use explicitly versioned, schema-validated encodings such as canonical JSON where relational columns would add complexity without improving integrity.

---

## 3. File identity

The database must identify itself as an AERIS project through both SQLite-level metadata and the `aeris_meta` table.

### 3.1 SQLite `application_id`

The current **draft candidate** application ID is:

```text
0x41455249
```

which encodes ASCII `AERI`.

This value is **provisional** until the 1.0 format freeze. Before 1.0 it must be checked again for collision and, where practical, submitted to relevant public identification registries.

Draft implementations must not describe this value as externally registered.

### 3.2 SQLite `user_version`

`PRAGMA user_version` may mirror the core schema generation for tooling convenience, but the authoritative version remains the explicit metadata stored by AERIS.

---

## 4. Application version is not format version

AERIS application releases and AERIS project-format releases are separate namespaces.

Examples:

```text
AERIS application 1.0  -> AERIS Project Format 1.0
AERIS application 4.7  -> AERIS Project Format 1.0
AERIS application 9.2  -> AERIS Project Format 1.0
```

A program release must not change the file format merely because the program itself changed.

After a stable format generation is published:

- existing field semantics never change;
- deleted semantics remain readable forever;
- old fixtures remain in the test suite permanently;
- current AERIS releases retain readers for all stable generations;
- unknown future major formats are rejected rather than guessed;
- migrations are explicit and transactional.

A new stable format generation is a last resort.

---

## 5. Canonical project vs session state

AERIS separates project meaning from workspace/UI state.

For a project named:

```text
world.aeris
```

AERIS may maintain directly beside it:

```text
world.aeris.session
```

The files have different roles.

### `world.aeris`

Canonical project document. Contains the map, data provenance, normalized geometry, layers, styles, source references, resources, and project-level settings required to reproduce the work.

### `world.aeris.session`

Disposable but durably written workspace state. It may contain:

- current viewport and zoom;
- active selection;
- active tool;
- open panels and tabs;
- UI splitter positions;
- current layer focus;
- in-progress non-project UI state needed for exact restart restoration.

Deleting `.aeris.session` must never corrupt or semantically change `.aeris`.

The sidecar is intentionally adjacent to the project rather than buried inside an application-state directory. A user looking at the project location can see and move the relevant working state deliberately.

The sidecar must contain the canonical project UUID and must be ignored if that UUID does not match the project.

---

## 6. Continuous durability

There is no traditional periodic autosave operation.

Every acknowledged project mutation is committed to `world.aeris` immediately through an atomic database transaction.

Every acknowledged session mutation that matters for exact restart restoration is committed to `world.aeris.session` with the same durability philosophy.

The implementation must not acknowledge a mutation as complete while it exists only in volatile memory.

AERIS may coalesce extremely high-frequency transient events internally for performance, but the UI must distinguish those events from acknowledged durable state. Coalescing must never create an undocumented multi-minute or multi-second autosave window.

---

## 7. `Ctrl+S` semantics

AERIS has no manual project-save command for normal editing.

- `Ctrl+S` = **Export**.
- If a valid previous export target and profile exist, `Ctrl+S` repeats that export.
- If no valid export target/profile exists, `Ctrl+S` opens export configuration.
- `Ctrl+Shift+S` = **Export As / Configure Export**.

Project persistence is independent from export.

---

## 8. Draft core schema

The following table set is deliberately small and remains subject to change before the 1.0 freeze.

### 8.1 `aeris_meta`

Singleton project metadata and format identity.

Draft responsibilities:

- project UUID;
- format major/minor;
- creation timestamp;
- last modification timestamp;
- monotonically increasing project revision;
- producer application/version;
- projection identifier and versioned parameters;
- worldview identifier;
- portability/frozen-state marker.

### 8.2 `aeris_source`

One row per logical external source or immutable source snapshot.

Draft fields include:

- source ID;
- provider;
- dataset;
- dataset/snapshot version;
- retrieval timestamp;
- source locator;
- license identifier;
- required notice text or reference;
- content hash where practical;
- worldview/dispute metadata where applicable.

A source reference must be precise enough to prevent accidental substitution of a newer dataset.

### 8.3 `aeris_resource`

Content-addressed binary or textual resources.

Draft fields include:

- resource ID;
- SHA-256 digest;
- media type;
- byte length;
- storage mode (`embedded` or immutable external reference);
- payload or external content-addressed locator.

Resources required for a frozen project must be embedded.

### 8.4 `aeris_feature`

Normalized geographic features before final map projection.

AERIS stores canonical geographic geometry in an explicitly defined geodetic reference system rather than only storing the final 2D projected drawing.

The initial target is normalized WGS84 geographic coordinates with explicit handling for antimeridian topology, polygon rings, multipolygons, and disputed/worldview-specific feature identity.

Derived projected geometry is reproducible cache data, not canonical truth.

### 8.5 `aeris_layer`

Logical render layers and their ordering/visibility/project-level configuration.

Examples:

- countries;
- national borders;
- disputed borders;
- rivers;
- cities;
- flags;
- labels;
- graticule;
- raster imagery.

Layer data must not hard-code one presentation style.

### 8.6 `aeris_style`

Versioned declarative style definitions and project-specific overrides.

A style controls presentation, not geographic truth.

Examples may include:

- Political;
- Political Dark;
- Monochrome;
- Physical;
- Earth;
- Satellite;
- user-defined styles.

Style payloads may use canonical schema-validated JSON if that remains the smallest stable representation at 1.0.

### 8.7 `aeris_extension`

Reserved extension registry.

The core format should not use extensions as an excuse to avoid finishing the specification. The extension mechanism exists for genuinely separable future capabilities.

Each extension records at minimum:

- extension identifier;
- extension version;
- whether support is required for semantic correctness;
- optional schema/namespace metadata.

Unknown required extension:

```text
open for write -> forbidden
semantic render -> forbidden unless safely understood
```

Unknown optional extension may be ignored only if doing so cannot alter project meaning and the implementation can preserve its data intact. Otherwise the project must be opened read-only or rejected.

---

## 9. Canonical geometry and projection

AERIS must not treat a rendered 2D map as the source of geographic truth.

Canonical flow:

```text
source data
    -> normalization
    -> canonical geographic geometry
    -> ellipsoid/authalic transform
    -> equal-area projection
    -> render scene
    -> export
```

The initial equal-area direction is the Philbrick Sinu-Mollweide family, with exact implementation details to be defined in a separate projection specification.

Projection parameters and algorithm version must be stored explicitly enough to reproduce an historical render.

---

## 10. Area invariant

Relative geographic area preservation is a project-level mathematical invariant, not a visual preference.

Before a projection implementation is accepted, automated tests must compare geodetic reference area against projected planar area under a single global scale factor across representative and adversarial geometries.

The tolerance must be numerically specified before the first stable release.

AERIS must not display an "area verified" status unless the active projection/data path passed the applicable invariant checks.

---

## 11. Source immutability and provenance

Normal projects may refer to externally cached resources, but such references must be immutable and content-addressed where practical.

Bad:

```text
natural-earth/latest/countries
```

Acceptable conceptually:

```text
provider = Natural Earth
version = <exact version>
sha256 = <exact content hash>
```

If the required resource is absent locally, AERIS may attempt to retrieve that exact resource. If it cannot, it reports the missing resource.

It must not silently replace it with a newer version.

---

## 12. Normal vs frozen/portable projects

Both modes use the same `.aeris` file format.

### Normal project

May use immutable external/cache references for large source resources. The project still records exact provenance and hashes.

### Frozen / portable project

Embeds every resource required to reproduce the project without network access.

A frozen file may be much larger. That does not justify inventing a second project format.

A frozen project must remain a normal valid `.aeris` file.

---

## 13. Caches are never canonical

Render caches, spatial indexes that can be rebuilt, thumbnails, decompressed raster tiles, temporary projections, and other derived data may be stored outside the canonical project or in explicitly noncanonical cache structures.

Deleting caches must not change project meaning.

A cache format may evolve more freely than the stable project format because it is disposable and reproducible.

---

## 14. Export profiles

A project may store reusable export profiles, but exported artifacts are not the project itself.

Planned output families:

- SVG;
- vector-first PDF;
- PNG;
- JPEG;
- GeoPackage;
- GeoJSON;
- GeoTIFF where applicable.

An export profile may include:

- format;
- target path;
- dimensions or physical size;
- DPI;
- color profile;
- bit depth;
- antialiasing/supersampling;
- JPEG quality and chroma settings;
- vector/raster preservation rules;
- attribution inclusion;
- metadata inclusion.

Export must use atomic replacement where practical so a failed export does not destroy the previous valid artifact.

---

## 15. No arbitrary quality ceiling

The file format must not encode product-tier concepts such as "maximum 8K".

Dimensions and physical print sizes are explicit numeric parameters subject only to documented implementation and file-format limits.

Large raster renderers should use bounded-memory tiled or striped output pipelines.

Vector output should retain vector geometry whenever the target format supports it.

---

## 16. Timestamps and identifiers

Stable format timestamps must use an unambiguous UTC representation.

Project and entity identifiers must not depend on filenames, localized labels, database row order, or mutable display names.

The project UUID is the stable identity used to bind `world.aeris.session` to `world.aeris`.

---

## 17. Integrity

At open time and at appropriate lifecycle points, AERIS may verify:

- SQLite integrity;
- schema invariants;
- foreign-key integrity;
- project UUID validity;
- resource SHA-256 values;
- required source/resource presence;
- extension compatibility;
- geometry validity appropriate to the normalized model.

A failed integrity check must never be silently "fixed" by replacing source data with unrelated newer content.

---

## 18. Migration rules

Before 1.0, draft schema migration may be destructive and is not a public compatibility contract.

After 1.0:

- migration must run in an atomic transaction;
- irreversible migrations must first create or require an explicit recoverable copy;
- migration must validate the result before commit;
- failure must roll back cleanly;
- current AERIS must retain the ability to read the original stable generation;
- a migration must never silently reinterpret an existing field with new semantics.

---

## 19. Compatibility fixtures

Every stable format generation must ship with immutable historical fixtures.

Example future tree:

```text
tests/format/
  v1/
    minimal.aeris
    political.aeris
    frozen.aeris
    custom-style.aeris
```

Those files are not regenerated when the implementation changes.

Corruption/adversarial fixtures should also be retained:

```text
corrupt-header.aeris
broken-resource-hash.aeris
unknown-required-extension.aeris
future-major.aeris
```

A stable format is considered supported only if compatibility is continuously exercised in CI.

---

## 20. Power-loss behavior

The on-disk design must support an implementation in which an acknowledged transaction survives abrupt process or machine termination under the documented filesystem/storage assumptions.

The reference implementation should favor SQLite durability settings appropriate to that contract and must test forced termination during mutation.

AERIS must not use a faster configuration if doing so knowingly creates a window in which an acknowledged commit can disappear after power loss without making that tradeoff explicit.

---

## 21. Security

An `.aeris` file is untrusted input.

The stable format must not require execution of embedded code.

Readers must enforce reasonable bounds on:

- BLOB sizes;
- geometry counts and nesting;
- JSON/document depth;
- text lengths;
- numeric ranges;
- decompression ratios where compressed embedded resources are ever introduced.

No path stored in a project may permit path traversal outside an explicitly allowed project/cache context.

---

## 22. Things intentionally outside the core format

The following do not belong in stable `.aeris` semantics unless later proven necessary:

- application window coordinates;
- panel splitter widths;
- recent-files lists;
- application preferences;
- machine-specific GPU settings;
- disposable render caches;
- network credentials;
- authentication tokens;
- absolute cache paths;
- framework-specific widget state.

Workspace state belongs in `.aeris.session`; machine-global preferences belong in normal application configuration.

---

## 23. Freeze criteria for AERIS Project Format 1.0

The `DRAFT` marker may be removed only after all of the following are complete:

1. core schema is implemented;
2. canonical geometry model is specified;
3. project UUID and file identification are finalized;
4. source/provenance model is implemented;
5. normal and frozen projects are demonstrated;
6. continuous durability is destructively tested;
7. session sidecar restoration is tested;
8. stable style serialization is defined;
9. extension behavior is tested;
10. corruption handling is tested;
11. security limits are documented;
12. at least one independent reader/inspection path exists outside high-level application state;
13. historical fixtures are committed;
14. the full specification has been reviewed against the implementation rather than retrofitted afterward.

Only then may the project write publicly stable `AERIS Project Format 1.0` files.
