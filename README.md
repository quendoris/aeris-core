# AERIS

**Authalic Equal-area Rendering & Ingestion System**

AERIS is an experimental cartographic system for building modern, interactive world maps while preserving relative geographic area as a hard mathematical invariant.

The project began from a simple personal goal: make a world map worth printing and hanging on a wall, without accepting the familiar area distortions of common compromise or conformal projections. The same tool should remain usable by anyone who wants to build such a map for themselves.

## Status

AERIS is in **early implementation**.

The reference core now includes WGS84/authalic mathematics, spherical rotation, independent Sinusoidal and Mollweide primitives, canonical geographic edge/ring semantics, adaptive finite-geometry verification, projection seam handling for supported polar winding and general zero-winding rings, horizon-aware authalic orthographic globe curves, explicit filled visible-globe horizon topology with verified adaptive refinement, source acquisition/provenance, a strict minimal Polygon Shapefile reader, and pinned real-world whole-land integration proofs.

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

The normal CI gate runs on Linux, Windows, and macOS, with an additional Linux ASan/UBSan pass. It exercises authalic forward/inverse math, spherical rotations, equal-area primitives and Jacobians, canonical WGS84 ring area, adaptive subdivision, geographic seam partitioning, piecewise final area budgets, split exterior/hole semantics, horizon-aware visible globe curves and root solving, filled-globe horizon closure, verified near-limb fill refinement, source acquisition/registry contracts, Shapefile parsing, SHA-256, Natural Earth adapter behavior, and diagnostic generation.

A separate networked **Source Compatibility** workflow verifies exact pinned third-party source bytes through the production ingestion/projection/view paths without making ordinary CI depend on a live upstream service. Relevant pull requests run this proof before merge.

## First real-world milestones

AERIS has successfully processed the exact pinned Natural Earth `ne_110m_land` snapshot (`v5.1.2`) through its own verification, Shapefile decoding, canonical topology, WGS84/authalic area reference, polar seam handling, adaptive projection, horizon-aware globe visibility, filled visible-region topology, and final verification gates.

The normal equal-area world proof covers 127 features, 128 rings, and 5015 source vertices, including the winding Antarctic exterior ring. A second pass moves the projection central meridian to `+90°`, forcing the same real source geometry through the general zero-winding seam splitter: 5 source rings are split across 14 physical seam crossings into 135 planar pieces. Both Sinusoidal and Mollweide remain inside the original-ring aggregate area budgets.

The same pinned geometry also passes the authalic orthographic globe path. With a camera centered at `15°E, 20°N` geodetic, the wireframe reference produces 66 visible source rings, 74 visible fragments, 26 sign-changing horizon crossings, and 2764 projected vertices. Horizon endpoints are solved from canonical geographic edges before SVG output; coastline termination is not manufactured with a circular clip path.

The verified filled-globe proof starts deliberately coarse at `5000 m` curve tolerance and `500 m` horizon-arc tolerance, then adaptively proves each source ring rather than using a fixed special-case quality setting. The accepted world contains 71 closed fill rings and 3078 fill vertices; the most difficult real ring required 12 refinement rounds and reached approximately `2.441 m` curve tolerance and `0.244 m` limb-arc tolerance. The final diagnostic uses 71 explicit closed land subpaths, 74 independently recomputed open coastline paths, and zero SVG `clipPath` elements.

See [Real-World Conformance Proofs](docs/REAL-WORLD-CONFORMANCE.md) for exact resource SHA-256 identities and measured integration evidence, [Projection Seam Topology](docs/PROJECTION-SEAM-TOPOLOGY.md) for planar cut semantics, and [Globe Horizon Topology](docs/GLOBE-HORIZON-TOPOLOGY.md) for the curve/fill/verification contract and its explicit numerical non-claims.

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
- [Projection Seam Topology](docs/PROJECTION-SEAM-TOPOLOGY.md)
- [Globe Horizon Topology](docs/GLOBE-HORIZON-TOPOLOGY.md)
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