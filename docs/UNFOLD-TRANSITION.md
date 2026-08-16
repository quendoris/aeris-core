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

It MUST NOT be used for:

- export geometry;
- area-preservation claims;
- project persistence of cartographic coordinates;
- GIS interchange;
- measurement;
- hit-testing that claims geographic precision.

The UI should identify the transition as such rather than label an intermediate frame `VERIFIED`.

## 3. Geographic guide

The first implementation uses a deterministic geographic guide consisting of:

- meridians every 30 degrees, excluding the active projection seam;
- parallels every 15 degrees from 75°S through 75°N;
- two explicit copies of the `-180°/+180°` seam meridian.

Each guide vertex carries three pieces of information derived from the same WGS84 longitude/geodetic-latitude sample:

1. its authalic-globe orthographic endpoint under the current camera;
2. its target equal-area planar endpoint;
3. its signed normalized globe depth.

The two seam guide lines are intentionally separate even though they are geographically coincident on the sphere. At the globe endpoint they occupy the same spherical meridian; at the planar endpoint they become the left and right projection boundaries. This makes the cut itself visible during unfolding.

The guide is not canonical project geometry. Changing guide density or presentation in a future application release does not change `.aeris` semantics.

## 4. Interpolation

Let `p` be UI progress clamped to `[0, 1]` and let

```text
s(p) = 6p^5 - 15p^4 + 10p^3
```

be the quintic smoothstep.

For a guide vertex with globe coordinate `G` and planar coordinate `P`, the displayed guide position is

```text
X(p) = G + s(p) (P - G)
```

This guarantees exact endpoint coordinates and zero first/second derivative at the endpoints for the explanatory motion.

Guide geometry that begins on the hidden globe hemisphere is not shown as if it were initially visible. Hidden guide content is introduced progressively as the surface opens; all guide content is visible by `p = 1`.

## 5. Land and coastline presentation

The verified globe and verified planar endpoint scenes remain separate scene objects. A renderer may cross-fade their fills/outlines around the interpolated geographic guide, but the cross-fade has no cartographic meaning.

The renderer MUST preserve exact endpoint behavior:

- `p = 0` displays the verified globe endpoint;
- `p = 1` displays the verified planar endpoint.

No intermediate fill topology is promoted to canonical geometry merely because it looks visually plausible.

## 6. Target projections

The first transition contract supports the independently verified Sinusoidal and Mollweide primitives.

This does **not** freeze the historical Philbrick Sinu-Mollweide composition. When the Philbrick region/orientation contract is resolved, it may become another verified planar target without changing the endpoint-separation principle in this document.

## 7. Cancellation and ownership

Preparing an unfold bundle may be computationally expensive because both endpoints are verified. It therefore follows the same cancellation/stale-result discipline as other viewer jobs.

A completed bundle for an obsolete camera or target MUST NOT replace newer viewer state.

Once a bundle has been accepted by the UI, per-frame interpolation is presentation-only and should not rerun geographic verification.

## 8. Conformance requirements before enabling the UI action

The `Unfold` action remains disabled until automated tests prove at least:

- both endpoints are verified production scenes;
- progress `0` and `1` reproduce guide endpoints exactly;
- the two seam sides coincide at the globe endpoint and separate at the planar endpoint;
- invalid globe-as-target requests fail explicitly;
- cancellation can prevent obsolete bundle delivery;
- the complete animation can be rendered without changing project/cartographic state.

The first enabled viewer milestone should also be exercised against the same exact pinned Natural Earth snapshot used by Viewer CI.
