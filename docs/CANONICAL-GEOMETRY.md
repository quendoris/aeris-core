# AERIS — Canonical Geographic Geometry

**Status:** DRAFT IMPLEMENTATION CONTRACT  
**Canonical edge model:** `wgs84-linear-v1`

This document defines the first explicit geographic edge semantics used by the AERIS reference core.

## 1. Why the edge model is explicit

A list of longitude/latitude vertices is not sufficient to define a geographic curve unless the interpolation rule between consecutive vertices is also known.

AERIS therefore treats edge semantics as part of canonical geometry rather than inheriting them from a renderer or GIS library.

## 2. `wgs84-linear-v1`

For two canonical WGS84 positions `(lambda0, phi0)` and `(lambda1, phi1)`, an ordinary edge is parameterized by

```text
lambda(t) = lambda0 + t (lambda1 - lambda0)
phi(t)    = phi0    + t (phi1    - phi0)
0 <= t <= 1
```

where longitude is first unwrapped into one continuous branch for the ring.

This is coordinate-space linear interpolation. It is **not** silently reinterpreted as a shortest ellipsoidal geodesic.

The choice deliberately matches the line semantics specified by IETF RFC 7946 for GeoJSON positions. Sources with different edge semantics must be explicitly normalized to `wgs84-linear-v1` with documented source and error provenance before becoming canonical AERIS geometry.

Reference: IETF RFC 7946, section 3.1.1.

## 3. Longitude unwrapping

Canonical ring construction normalizes longitude continuity edge by edge.

For every consecutive source pair, AERIS chooses the equivalent longitude delta in the principal interval around zero. An edge whose endpoints differ by exactly half a revolution is ambiguous under this rule and is rejected until the source supplies an explicit split or otherwise resolves the intended path.

The WGS84 antimeridian is therefore not automatically a geometric cut. A ring such as

```text
170 E -> 170 W -> 170 W -> 170 E
```

may normalize internally to

```text
170 -> 190 -> 190 -> 170
```

without creating a false 340-degree edge.

## 4. Ring closure

Canonical AERIS rings store each ordinary vertex once; a duplicate terminal copy of the first source vertex is removed during normalization when it is exactly equivalent.

The closing edge is implicit. Its longitude endpoint is the equivalent copy of the first longitude that continues the same unwrapped branch from the last vertex.

This permits the geometry core to detect non-zero longitude winding instead of destroying that information during normalization.

## 5. Polar and global winding

The first area implementation supports rings with zero net longitude winding.

A non-zero winding is currently a **fail-closed result**, not an invitation to guess which spherical/ellipsoidal region is intended. Polar caps, global masks, and other winding geometries require an explicit stable inside/outside and orientation contract before AERIS 1.0.

This restriction is temporary but normative for the current reference implementation.

## 6. WGS84 signed area for the canonical edge

For WGS84 geodetic latitude `phi`, AERIS already defines the authalic function `q(phi)`.

The ellipsoidal area element is

```text
dA = (a^2 / 2) q'(phi) dphi dlambda
```

For a zero-winding, positively oriented ring, Green/Stokes reduction gives the signed boundary integral

```text
A = -(a^2 / 2) integral_ring q(phi) dlambda
```

For one `wgs84-linear-v1` edge,

```text
dlambda = (lambda1 - lambda0) dt
phi(t)  = phi0 + t (phi1 - phi0)
```

and therefore

```text
A_edge = -(a^2 / 2) (lambda1 - lambda0)
         * integral_0^1 q(phi(t)) dt
```

AERIS evaluates this one-dimensional integral with its own deterministic adaptive Simpson reference integrator and reports the accumulated numerical error estimate.

Constant-longitude edges contribute exactly zero. Constant-latitude edges reduce to a constant `q(phi)` integral.

## 7. Projection-area invariant

The canonical edge is a continuous WGS84 curve. Rendering it requires:

```text
wgs84-linear-v1 edge
        -> authalic latitude
        -> selected equal-area primitive / composition
        -> adaptive planar subdivision
        -> finite planar polyline
```

Tests must compare the signed WGS84 boundary-integral area against the signed planar polygon area after adaptive subdivision.

This verifies the actual finite geometry consumed by SVG/PDF/raster paths, not merely the analytical point transform.

## 8. Import rule

An importer must identify or define the source edge semantics.

If they are already equivalent to `wgs84-linear-v1`, the vertices may be normalized directly.

If they differ, the importer must approximate the source curve into `wgs84-linear-v1` segments to a documented geographic error bound. AERIS must never silently change an ellipsoidal geodesic, projected-space curve, spline, or other source edge into coordinate-linear WGS84 geometry.

## 9. Stable-format rule

`wgs84-linear-v1` is a semantic identifier, not merely an implementation nickname.

Once persisted by a stable AERIS project format, its meaning cannot be changed. A future edge model must receive a new identifier rather than redefining this one.
