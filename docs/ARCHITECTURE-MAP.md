# AERIS Architecture Map

Status: **INDEX — NON-NORMATIVE**

This document is a navigation and traceability map for AERIS. It does not create a new file-format rule, geometry rule, projection invariant, or UI contract. When this index and a normative contract disagree, the normative contract and executable conformance checks win.

The purpose of this map is to answer four questions quickly:

1. Where is a design decision specified?
2. Which implementation boundary owns it?
3. Where is it verified?
4. Which important runtime decisions still lack a canonical contract?

AERIS intentionally keeps those questions separate. A feature is not considered architecturally settled merely because code for it exists, and a document is not considered implemented merely because it describes a desired state.

## 1. Repository boundary

`aeris-core` owns render-neutral and frontend-neutral semantics:

- canonical geographic geometry and WGS84 semantics;
- source/adaptor verification and provenance;
- projection mathematics and seam topology;
- render-neutral scene/surface geometry;
- `.aeris` storage, integrity and portability contracts;
- numerical elevation codec/storage primitives;
- conformance fixtures and invariant tests.

Frontend repositories consume those contracts. They may own interaction, presentation, caching, job orchestration and platform integration, but they must not invent an alternative interpretation of canonical `.aeris` state.

In particular:

- `aeris-desktop` owns Qt desktop presentation, map interaction, isolated data jobs, acquisition UX, presentation caches and offscreen UI acceptance;
- `aeris-android` owns Android lifecycle, storage-access integration, mobile presentation and viewport scheduling;
- neither frontend is a second project format or a second canonical geometry engine.

## 2. Contract hierarchy

The documents below are grouped by responsibility rather than by chronology.

### 2.1 Project identity, durability and storage

| Contract | Primary responsibility | Implementation surface | Representative verification |
| --- | --- | --- | --- |
| `AERIS-PROJECT-FORMAT.md` | Top-level `.aeris` format goals, versioning and durable project model | `include/aeris/storage/*`, `src/storage/*` | Storage CI, project reopen/integrity fixtures |
| `STORAGE-FOUNDATION.md` | SQLite durability, acknowledged mutations, integrity and transaction expectations | storage project/resource/layer/dataset implementations | `storage_abrupt`, `storage_race`, atomicity and corruption tests |
| `PROJECT-PROVENANCE.md` | Durable origin, immutable source identity and provenance semantics | source/provenance storage + project bridge | `provenance_race`, source/project compatibility tests |
| `PROJECT-GEOMETRY.md` | Canonical geometry persisted in a project and reconstructed without source files | geometry storage + durable source reader | geometry storage/corruption/reopen tests |

The top-level dependency direction is:

```text
AERIS-PROJECT-FORMAT
        |
        +--> STORAGE-FOUNDATION
        |        |
        |        +--> PROJECT-PROVENANCE
        |        +--> PROJECT-GEOMETRY
        |
        +--> canonical source/geometry contracts
```

Storage is an implementation substrate for canonical state, not an authority that may reinterpret geometry or provenance.

### 2.2 Source ingestion and canonical geometry

| Contract | Primary responsibility | Implementation surface | Representative verification |
| --- | --- | --- | --- |
| `SOURCE-PIPELINE.md` | Boundary between acquisition, verification, adaptation and canonical result | `include/aeris/source/*`, `src/source/*`, project bridge | source compatibility workflow, acquisition tests |
| `SOURCE-ADAPTERS.md` | Adapter responsibilities and fail-closed source interpretation | source adapters/registry | adapter/unit + pinned real-source compatibility |
| `CANONICAL-GEOMETRY.md` | Geometry representation and invariants before projection | `include/aeris/geometry/*`, `src/geometry/*` | geographic-area, geometry corruption and topology tests |
| `REAL-WORLD-CONFORMANCE.md` | Real-data acceptance beyond synthetic geometry fixtures | source/view/project tools and fixtures | pinned Natural Earth compatibility and real-world probes |

The intended data direction is one-way:

```text
acquisition bytes
    -> verified acquisition identity
    -> source adapter / registry validation
    -> canonical WGS84 result
    -> acknowledged project mutation
    -> durable adapter-free reconstruction
```

A renderer must never need the original SHP/DBF/TIFF solely to reinterpret a successfully materialized canonical source. Acquisition caches are machine-local conveniences, not an alternate project state.

### 2.3 Projection and topology

| Contract | Primary responsibility | Implementation surface | Representative verification |
| --- | --- | --- | --- |
| `PROJECTION-SINU-MOLLWEIDE.md` | Mathematical/engineering contract for Sinu-Mollweide | `include/aeris/projection/*`, `src/projection/*` | projection, area and inverse/round-trip tests |
| `PROJECTION-SEAM-TOPOLOGY.md` | Seam crossing, partitioning and topology-preserving projection behavior | projection ring/seam implementations | seam, piecewise ring and movable-cut regressions |
| `GLOBE-HORIZON-TOPOLOGY.md` | Horizon clipping/topology for Globe view | `include/aeris/view/*`, `src/view/*` | globe polygon/curve/horizon tests |
| `CANONICAL-GEOMETRY.md` | Input geometry invariants that projection code may rely on | geometry layer | geometry and geographic-area tests |

Projection code consumes canonical WGS84 geometry. It does not repair arbitrary malformed input by changing the mathematical request. Seam handling, polar handling and area verification belong to the projection/topology boundary.

The current render-neutral public scene boundary is owned by `aeris/view/*`. It is responsible for producing frontend-independent Globe and planar scene geometry while preserving canonical feature identity and requested projection parameters.

### 2.4 Physical surface semantics

| Contract | Primary responsibility | Implementation surface | Representative verification |
| --- | --- | --- | --- |
| `SURFACE-CLASSIFICATION.md` | Separate numerical elevation from water/land/grounded-ice/floating-ice semantics | canonical semantic surface layer + frontend presentation caches | classification/source fixtures, durable reopen proof, Desktop Globe/planar pixel regressions and coordinate-level inspector acceptance |

The draft surface-classification contract is intentionally independent of the numerical elevation codec. Elevation answers height/depth; semantic classification answers what physical/material class occupies the geographic point; presentation decides how to style the combination.

### 2.5 Unfold and frontend interaction boundary

| Contract | Primary responsibility | Implementation surface | Representative verification |
| --- | --- | --- | --- |
| `UNFOLD-TRANSITION.md` | Meaning of verified endpoints vs explanatory intermediate transition | `aeris/view` unfold helpers | endpoint/transition tests and frontend acceptance |
| `UI-ARCHITECTURE.md` | UI as a command/view surface rather than canonical truth | frontend command/orchestration boundary | desktop/android interaction and lifecycle acceptance |

Important separation:

```text
canonical project state
        |
        v
render-neutral scene request/result
        |
        v
frontend presentation/cache/interaction
```

Camera motion, viewport pan/zoom, hover state, animation frames and presentation caches are not automatically project mutations. Conversely, an acknowledged project mutation must not exist only as temporary widget state.

## 3. Cross-contract roots

A small number of concepts sit near the root of many other decisions.

### 3.1 Canonical geometry

Canonical geometry is the common input to storage reconstruction, projection, Globe rendering and real-world conformance. Changes here have a wide blast radius and require geometry, projection and source compatibility checks.

Primary contract: `CANONICAL-GEOMETRY.md`.

### 3.2 Durable project state

The `.aeris` project is the durable boundary. Successful materialization/import must result in a project whose canonical interpretation does not depend on a machine-local acquisition path.

Primary contracts: `AERIS-PROJECT-FORMAT.md`, `STORAGE-FOUNDATION.md`, `PROJECT-PROVENANCE.md`, `PROJECT-GEOMETRY.md`.

### 3.3 Verified source identity

Source provenance is not a UI label. It is part of the durable explanation of what exact external material produced canonical project state.

Primary contracts: `SOURCE-PIPELINE.md`, `SOURCE-ADAPTERS.md`, `PROJECT-PROVENANCE.md`.

### 3.4 Projection request identity

Projection model, active cut/seam parameters and camera state are distinct concepts. A frontend may edit presentation state before applying a projection, but a verified planar result must correspond to the exact request that crossed the core boundary.

Primary contracts: projection documents plus `UI-ARCHITECTURE.md` and `UNFOLD-TRANSITION.md`.

## 4. Verification layers

AERIS deliberately uses more than one kind of proof. No single test class replaces the others.

### 4.1 Unit/invariant tests

These test compact mathematical or storage invariants in isolation. Examples include elevation codec/grid checks, geographic area checks, geometry corruption rejection, projection math and storage mutation rules.

### 4.2 Transaction/race/abrupt-termination tests

These exercise acknowledged-state semantics under failure and concurrency. Representative fixtures include:

- `tests/storage_abrupt.cpp`;
- `tests/storage_race.cpp`;
- `tests/layer_stack_race.cpp`;
- `tests/provenance_race.cpp`;
- dataset/property atomicity tests.

Their purpose is not UI coverage; they prove that the durable boundary itself behaves coherently.

### 4.3 Real-source compatibility

Pinned real-world source material is used to catch assumptions that synthetic fixtures cannot expose. Natural Earth compatibility and real-world projection probes belong here.

This layer has already exposed topology cases that passed synthetic tests, so it must remain independent rather than being replaced by larger synthetic corpora.

### 4.4 Frontend acceptance

Desktop and Android must separately prove lifecycle, cancellation, stale-result rejection, project ownership and presentation behavior. Those checks live in the frontend repositories because core must remain Qt/Android independent.

Frontend acceptance may prove that a core contract is used correctly; it must not redefine that contract.

## 5. Change-impact map

Use this section as a minimum review checklist, not as an exhaustive dependency solver.

| If this changes... | Re-check at minimum... |
| --- | --- |
| `.aeris` schema or acknowledged mutation semantics | project format, storage foundation, integrity/reopen, abrupt/race tests, frontend compatibility |
| source identity/provenance | source pipeline/adapters, provenance, project reader/bridge, real-source compatibility |
| canonical geometry representation | geometry storage, corruption checks, area/topology tests, every verified projection, Globe scene |
| seam/polar topology | projection seam contract, polar regressions, real-world conformance, frontend projection acceptance |
| public scene request/result | core view tests plus every frontend scene controller and stale-generation boundary |
| elevation codec/resource semantics | elevation grid tests, storage resources, Desktop durable terrain import/reopen/LOD acceptance |
| surface classification semantics | surface-classification contract, durable source/provenance, Globe/planar consistency, Antarctic real-data regression |
| command/UI state ownership | `UI-ARCHITECTURE.md`, frontend lifecycle and project-mutation acceptance |

## 6. Policies

`ENGINEERING-POLICY.md` and `ATTRIBUTION-POLICY.md` are policy documents rather than mathematical contracts. They still affect implementation and release work, especially provenance, third-party datasets, reproducibility and evidence requirements.

They should not be used as substitutes for a missing technical contract. If a runtime invariant matters to file compatibility, geometry correctness or reproducibility, it belongs in a dedicated technical contract and executable acceptance.

## 7. Known documentation gaps

This index intentionally records gaps rather than silently pretending the documentation graph is complete.

### 7.1 Acquisition/job orchestration

Desktop has evolved an isolated data-worker model with resumable acquisition, progress reporting, hard cancellation and retry/recovery behavior. Most of that behavior currently lives in frontend code, CI and PR history rather than one canonical architecture note.

A future contract/ADR should distinguish:

- acquisition cache state;
- immutable upstream identity;
- verified bytes;
- project mutation stages;
- cancellation semantics;
- crash/retry semantics;
- UI progress state.

This belongs at the frontend/application boundary unless a rule becomes part of `.aeris` semantics.

### 7.2 Surface/material classification

`SURFACE-CLASSIFICATION.md` defines the draft semantic boundary and its first production slice is now implemented. Core owns canonical generation-1 class IDs, a verified Antarctic floating-ice-shelf source, durable property persistence/reopen, and idempotent semantic-layer composition. Desktop consumes that durable channel, keeps numerical elevation out of material classification, and exercises Globe/planar pixel plus coordinate-level probe acceptance.

The five-class model is not yet complete as a globally resolved classifier: explicit canonical `water`, `land`, and `grounded_ice` coverage and deterministic reconciliation across future overlapping semantic providers still need implementation and acceptance. Documentation must therefore distinguish the implemented first slice from full generation-1 classification coverage.

### 7.3 Presentation-resource loading

Large optional presentation resources (for example embedded country flags) need an explicit lazy/cache ownership story. Their durable bytes belong to the project resource model; their decoded images and viewport-bounded caches belong to the frontend.

### 7.4 Cross-repository compatibility matrix

Core, Desktop and Android currently have explicit pinned/stacked development relationships, but there is no single release-compatibility matrix that says which released frontend version consumes which stable core contract generation. That should be added before AERIS starts publishing stable multi-repository releases.

## 8. What this map deliberately does not do

This file does not:

- assign stability to a contract that still calls itself draft;
- turn implementation comments into normative format rules;
- claim that every PR-level runtime decision is already canonical;
- duplicate the mathematical text of projection specifications;
- duplicate the SQLite schema description;
- make frontend caches durable;
- make acquisition caches part of `.aeris`;
- resolve open semantic questions by choosing whichever current implementation happens to do.

Its job is traceability. When an invariant gains or loses an implementation/proof, update this map so the mismatch is visible.