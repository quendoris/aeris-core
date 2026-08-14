# AERIS — Canonical Geographic Geometry

**Status:** DRAFT IMPLEMENTATION CONTRACT  
**Canonical edge model:** `wgs84-linear-v1`

This document defines the first explicit geographic edge and ring semantics used by the AERIS reference core.

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

Longitude values outside `[-pi, pi]` are therefore valid internal canonical coordinates after unwrapping. They represent a continuous branch, not invalid geographic positions.

## 4. Ring closure and winding

Canonical AERIS rings store each ordinary vertex once; a duplicate terminal copy of the first source vertex is removed during normalization when it is exactly equivalent.

The closing edge is implicit. Its longitude endpoint is the equivalent copy of the first longitude that continues the same unwrapped branch from the last vertex.

The net longitude change across the complete closed traversal is retained as an integer winding number `w`:

```text
w = (lambda_close - lambda_start) / (2 pi)
```

up to a tightly bounded floating-point verification tolerance.

AERIS does not destroy this information by wrapping every vertex independently back into `[-pi, pi]`.

## 5. A ring needs an interior side

A closed curve on a sphere or ellipsoid separates two valid regions. The vertex sequence alone does not, in general, identify which one is intended.

Canonical AERIS geometry therefore has an explicit topological property:

```text
RingInteriorSide = unspecified | left | right
```

`left` and `right` are relative to traversal direction on the oriented WGS84 longitude/latitude surface.

For an ordinary local zero-winding ring, signed boundary area can be evaluated without choosing a global branch. For a non-zero-winding ring, however, `interior_side` is mandatory. If it is `unspecified`, area evaluation fails closed with `longitude_winding_unsupported`.

AERIS MUST NOT silently choose the smaller of the two regions. A valid explicit region may exceed one hemisphere.

### 5.1 Where interior-side semantics come from

Canonicalization does not invent `interior_side`.

The source adapter is responsible for translating a provider's documented polygon topology into AERIS `left`/`right` semantics. A low-level file-format decoder may expose source ring orientation, but it must not pretend that byte-format orientation alone is universal geographic meaning.

For the current Natural Earth Shapefile adapter, the ESRI Polygon convention is translated as:

```text
clockwise exterior      -> right
counter-clockwise hole  -> left
```

This translation belongs to the provider adapter, not to the generic Shapefile reader.

## 6. WGS84 signed area for the canonical edge

For WGS84 geodetic latitude `phi`, AERIS defines the authalic function `q(phi)`.

The ellipsoidal area element is

```text
dA = (a^2 / 2) q'(phi) dphi dlambda
```

Define the dimensionless boundary integral

```text
I = integral_ring q(phi) dlambda
```

For a zero-winding branch, the signed area is

```text
A0 = -(a^2 / 2) I
```

For one `wgs84-linear-v1` edge,

```text
dlambda = (lambda1 - lambda0) dt
phi(t)  = phi0 + t (phi1 - phi0)
```

and therefore

```text
I_edge = (lambda1 - lambda0)
         * integral_0^1 q(phi(t)) dt
```

AERIS evaluates this one-dimensional integral with its own deterministic adaptive Simpson reference integrator.

Constant-longitude edges contribute exactly zero. Constant-latitude edges reduce to a constant `q(phi)` integral.

## 7. Global winding branch

The total WGS84 ellipsoid surface area under the same authalic formulation is

```text
S = 2 pi a^2 q_p
  = 4 pi R_q^2
```

where `q_p = q(pi/2)` and `R_q` is the WGS84 authalic radius.

For integer longitude winding `w`, the topological branch before selecting the intended side is

```text
A_branch = A0 + w S / 2
```

AERIS then selects an equivalent representative modulo `S` according to explicit interior side:

```text
left  -> non-negative representative in [0, S)
right -> non-positive representative in (-S, 0]
```

This is not a "minor area" rule. The right-side complement of a small left-side polar cap is intentionally representable as a region whose magnitude exceeds `S/2`.

The implementation combines winding terms in dimensionless space before multiplication by `a^2`. This avoids unnecessary cancellation between world-scale square-metre values.

## 8. Numerical error contract

`GeographicAreaResult::estimated_abs_error_m2` is part of the reference contract, not decorative telemetry.

It includes at least:

- adaptive quadrature error accumulated across source edges;
- a conservative binary64 evaluation floor for `q(phi)` values;
- floating-point uncertainty introduced by global winding/topology terms.

The reference implementation may use wider intermediate arithmetic where a platform provides it, but correctness must not depend on `long double` being wider than `double`. In particular, MSVC implementations where both have the same width remain supported.

Analytical conformance tests compare against this published numerical budget rather than assuming an unrealistically uniform relative epsilon at planetary area scale.

## 9. Projection-area invariant

The canonical edge is a continuous WGS84 curve. Rendering it requires:

```text
wgs84-linear-v1 edge
        -> authalic latitude
        -> selected equal-area primitive / composition
        -> seam/topology handling where required
        -> adaptive planar subdivision
        -> finite planar geometry
```

Tests compare the signed WGS84 boundary-integral area against the signed planar area of the finite geometry actually consumed by SVG/PDF/raster paths.

For geometry split into multiple planar pieces, the invariant applies to the signed sum of all pieces.

This verifies the delivered geometry rather than merely the analytical point transform.

## 10. Canonical topology is not projection topology

The WGS84 antimeridian and a projection seam are different concepts.

Canonical geometry remains continuous and projection-independent. A projection may later need to:

- cyclically rebase a winding ring at its active seam;
- close a polar region along the projection-domain boundary and pole;
- split a zero-winding ring into multiple planar pieces when it crosses the active seam.

Those operations are derived projection topology. They MUST NOT mutate the canonical WGS84 ring or replace its source provenance.

## 11. Import rule

An importer must identify or define the source edge semantics and polygon topology.

If edges are already equivalent to `wgs84-linear-v1`, the vertices may be normalized directly.

If they differ, the importer must approximate the source curve into `wgs84-linear-v1` segments to a documented geographic error bound. AERIS must never silently change an ellipsoidal geodesic, projected-space curve, spline, or other source edge into coordinate-linear WGS84 geometry.

Likewise, provider-specific ring orientation or polygon-side rules must be translated explicitly into canonical topology rather than assumed globally.

## 12. Stable-format rule

`wgs84-linear-v1` and the eventual persisted ring-topology semantics are semantic contracts, not implementation nicknames.

Once persisted by a stable AERIS project format, their meanings cannot be changed. A future edge or topology model must receive a new semantic identifier rather than redefining an existing one.
