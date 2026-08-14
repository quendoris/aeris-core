# AERIS

**Authalic Equal-area Rendering & Ingestion System**

AERIS is an experimental cartographic system for building modern, interactive world maps while preserving relative geographic area as a hard mathematical invariant.

The project began from a simple personal goal: make a world map worth printing and hanging on a wall, without accepting the familiar area distortions of common compromise or conformal projections. The same tool should remain usable by anyone who wants to build such a map for themselves.

## Status

AERIS is in **early implementation**.

The reference core now includes WGS84/authalic mathematics, spherical rotation, independent Sinusoidal and Mollweide primitives, canonical geographic edge/ring semantics, adaptive finite-geometry verification, projection seam handling for supported polar winding rings, source acquisition/provenance, a strict minimal Polygon Shapefile reader, and the first pinned real-world whole-land integration proof.

The project format and final Philbrick Sinu-Mollweide composition remain drafts. No draft `.aeris` file or draft composite-projection output is promised long-term compatibility until the relevant contracts are explicitly frozen.

## Core principles

- **Area is invariant.** Rendering may trade shape, angle, or distance, but not relative area.
- **Provenance is first-class data.** Geographic input must have a known source, version, license, retrieval time, and content hash where practical.
- **No silent substitution.** A missing historical source version is not replaced by a newer one without an explicit user decision.
- **Project state is continuously durable.** There is no periodic "autosave window" in which accepted edits exist only in memory.
- **`Ctrl+S` means Export, not Save.** Project mutations are already durable; export is the explicit production action.
- **One stable project format.** Stable on-disk semantics are not casually changed to follow application releases.
- **Portable by design.** A frozen project can embed everything required to reproduce its map without network access.
- **Broad hardware support.** A correct CPU reference path is mandatory; optional acceleration must not become a hidden compatibility requirement.
- **Dependencies earn their place.** A small, well-tested internal implementation is preferred over dependency instability when an external library is needed only for a narrow function.
- **Attribution follows intellectual history, not visibility or marketing reach.**

## Implementation baseline

The reference core is written in portable **C++17** and built with CMake. Core mathematics has no Qt, Python, GIS-framework, GPU, or network runtime dependency.

AERIS intentionally keeps the future UI/toolkit layer outside the mathematical and project-model contracts. The GUI may evolve; historical project files and reference mathematics must remain independently readable and testable.

### Build and test

```sh
cmake -S . -B build -DAERIS_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

The normal CI gate runs on Linux, Windows, and macOS, with an additional Linux ASan/UBSan pass. It exercises authalic forward/inverse math, spherical rotations, equal-area primitives and Jacobians, canonical WGS84 ring area, adaptive subdivision, final polygon area budgets, source acquisition/registry contracts, Shapefile parsing, SHA-256, Natural Earth adapter behavior, globe reference projection, and diagnostic generation.

A separate networked **Source Compatibility** workflow verifies exact pinned third-party source bytes through the production ingestion and projection path without making ordinary CI depend on a live upstream service.

## First real-world milestone

AERIS has successfully processed the exact pinned Natural Earth `ne_110m_land` snapshot (`v5.1.2`) through its own verification, Shapefile decoding, canonical topology, WGS84/authalic area reference, polar seam handling, adaptive projection, and final area-budget checks.

The proof covers 127 features, 128 rings, and 5015 source vertices, including the winding Antarctic exterior ring. It emits verified Sinusoidal and Mollweide diagnostic world maps from the same canonical geometry.

See [Real-World Conformance Proofs](docs/REAL-WORLD-CONFORMANCE.md) for exact resource SHA-256 identities, aggregate snapshot identity, measured area errors, Antarctic seam details, and explicit non-claims.

## Planned project and export formats

### Project

- `.aeris` — canonical editable project file
- `.aeris.session` — adjacent, disposable workspace/session state for exact crash and restart restoration

### Render export

- SVG — vector master export
- PDF — vector-first print export
- PNG — lossless raster export
- JPEG — compact lossy raster export

### GIS interchange

- GeoPackage (`.gpkg`)
- GeoJSON
- GeoTIFF where applicable

AERIS will not impose an arbitrary "8K"-style ceiling. Large raster exports are intended to use bounded-memory tiled or striped rendering.

## Projection lineage

AERIS is presently exploring the **Philbrick Sinu-Mollweide** equal-area projection family as its primary visual and mathematical reference. Cartographic literature attributes this construction to **Allen K. Philbrick (1953)**. It combines sinusoidal and Mollweide ideas in a different arrangement from Goode Homolosine.

The implementation is not allowed to infer historical projection constants from appearance alone. The exact Philbrick composition, orientation, interruptions, seam ownership, and numerical contract must be verified and specified before the projection is declared stable.

See [ACKNOWLEDGEMENTS.md](ACKNOWLEDGEMENTS.md) for the historical lineage, [docs/PROJECTION-SINU-MOLLWEIDE.md](docs/PROJECTION-SINU-MOLLWEIDE.md) for the mathematical draft, and [docs/ATTRIBUTION-POLICY.md](docs/ATTRIBUTION-POLICY.md) for the project's attribution rules.

## Documents

- [AERIS Project Format — Draft](docs/AERIS-PROJECT-FORMAT.md)
- [Canonical Geographic Geometry](docs/CANONICAL-GEOMETRY.md)
- [Sinu-Mollweide Projection Contract — Draft](docs/PROJECTION-SINU-MOLLWEIDE.md)
- [Source Adapter Contract](docs/SOURCE-ADAPTERS.md)
- [Source Pipeline](docs/SOURCE-PIPELINE.md)
- [Real-World Conformance Proofs](docs/REAL-WORLD-CONFORMANCE.md)
- [UI Architecture Contract](docs/UI-ARCHITECTURE.md)
- [Engineering Policy](docs/ENGINEERING-POLICY.md)
- [Attribution Policy](docs/ATTRIBUTION-POLICY.md)
- [Acknowledgements](ACKNOWLEDGEMENTS.md)
- [Legal and licensing notice](NOTICE.md)

## License

AERIS source code is licensed under **GNU Affero General Public License v3.0 only (`AGPL-3.0-only`)** unless a file explicitly states otherwise.

Copyright © 2026 **quendoris**.

Third-party geographic data, imagery, fonts, flags, libraries, and other assets remain subject to their own licenses. AERIS does not relicense third-party material.