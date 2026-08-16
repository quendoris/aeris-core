# AERIS — Unfold Transition Contract

**Status:** DRAFT — explanatory viewer contract

`Unfold` explains how one canonical Earth representation is viewed first as an authalic globe and then as a selected equal-area plane. It is deliberately separated from normative projection and export semantics.

## 1. Normative endpoints

An unfold bundle contains two independently computed **verified** scenes:

1. a verified authalic orthographic globe at the current camera orientation;
2. a verified target planar scene produced directly by the selected equal-area projection pipeline.

The final planar scene MUST be computed by the ordinary projection engine. It MUST NOT be reconstructed from, sampled from, or otherwise derived from an animation frame.

Likewise, the globe endpoint uses the ordinary verified globe visibility/fill pipeline. The animation is not an alternate implementation of either endpoint.

## 2. Intermediate frames are explanatory

For progress `p` in the open interval `(0, 1)`, displayed transition geometry is **non-normative**.

It MUST NOT be used for export geometry, area-preservation claims, project persistence of cartographic coordinates, GIS interchange, measurement, or precision hit-testing.

The UI labels intermediate frames `UNFOLD` / `Transition (non-normative)`, never `VERIFIED`.

## 3. Geographic guide

The first implementation uses a deterministic geographic guide consisting of meridians every 30 degrees excluding the active projection seam, parallels every 15 degrees from 75°S through 75°N, and two explicit copies of the `-180°/+180°` seam meridian.

Each guide vertex carries its authalic-globe orthographic endpoint, target equal-area planar endpoint, and signed normalized globe depth, all derived from the same WGS84 longitude/geodetic-latitude sample.

The two seam guide lines are intentionally separate even though they are geographically coincident on the sphere. At the globe endpoint they occupy the same spherical meridian; at the planar endpoint they become the left and right projection boundaries. This makes the cut itself visible during unfolding.

The guide is not canonical project geometry. Changing guide density or presentation in a future application release does not change `.aeris` semantics.

## 4. Interpolation

Let `p` be UI progress clamped to `[0, 1]` and let

```text
s(p) = 6p^5 - 15p^4 + 10p^3
```

be the quintic smoothstep. For a guide vertex with globe coordinate `G` and planar coordinate `P`, the displayed guide position is

```text
X(p) = G + s(p) (P - G)
```

This guarantees exact endpoint coordinates and zero first/second derivative at the endpoints for explanatory motion.

Guide geometry that begins on the hidden globe hemisphere is introduced progressively as the surface opens. The guide itself fades to zero at both normative endpoints so endpoint presentation remains exactly the verified scene presentation.

## 5. Land and coastline presentation

The verified globe and verified planar endpoint scenes remain separate scene objects. The first renderer cross-fades these authoritative endpoint fills/outlines while the geographic guide visibly opens between them.

The cross-fade has no cartographic meaning. It is a presentation device chosen specifically so AERIS does not invent intermediate land topology and then accidentally promote it to a map.

The renderer preserves exact endpoint behavior:

- `p = 0` displays the verified globe endpoint;
- `p = 1` displays the verified planar endpoint.

## 6. Runtime ownership

Preparing an unfold bundle is a background job because both endpoints are verified. It has its own generation/cancellation ownership contract. A completed bundle for an obsolete camera or target is discarded and cannot overwrite newer viewer state.

Once a bundle has been accepted, a 1400 ms UI timer advances presentation progress only. Geographic verification is not rerun for each frame.

View-changing actions are disabled during the short accepted animation. At completion, the already verified flat endpoint becomes ordinary current viewer state.

## 7. Target projections

The first enabled transition supports the independently verified Sinusoidal and Mollweide primitives. The target is the most recently selected flat view; Mollweide is the initial default before a flat view has been chosen.

This does **not** freeze the historical Philbrick Sinu-Mollweide composition. When the Philbrick region/orientation contract is resolved, it may become another verified planar target without changing the endpoint-separation principle.

## 8. Conformance

Viewer CI proves against the exact pinned Natural Earth snapshot that:

- both endpoints are verified production scenes;
- progress `0` and `1` reproduce guide endpoints exactly;
- the two seam sides coincide at the globe endpoint and separate at the planar endpoint;
- invalid globe-as-target requests fail explicitly;
- stale background unfold generations are not delivered;
- scene and unfold jobs retain independent cancellation ownership;
- an offscreen midpoint frame renders through the same `MainWindow`/`MapCanvas` presentation path used interactively;
- intermediate frames remain labeled non-normative.

The transition remains an explanatory UI feature. Nothing in this contract changes equal-area, project-format, or export guarantees.
