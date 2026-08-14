# AERIS — Globe Horizon Topology

**Status:** DRAFT IMPLEMENTATION CONTRACT  
**Implemented reference view:** authalic orthographic globe

This document defines the first AERIS reference contract for rendering canonical WGS84 curves on a visible globe hemisphere without delegating geographic visibility topology to a GUI or vector-file clipping primitive.

The current implementation is intended as a deterministic CPU reference path and conformance oracle. It is not yet a contract for filled visible land polygons.

## 1. Why the horizon is geometry, not an SVG clip

A closed geographic ring projected onto a globe is generally only partly visible.

Simply projecting the complete ring and applying a circular SVG clip can hide the back hemisphere visually, but it does not produce explicit finite geometry for the true visible boundary. In particular, downstream renderers would not know where a canonical source edge actually intersects the horizon.

AERIS therefore derives horizon intersections before export.

The reference path is:

```text
canonical wgs84-linear-v1 edge
        ↓
geodetic WGS84 latitude
        ↓
authalic latitude
        ↓
unit sphere / selected camera rotation
        ↓
orthographic view-space sample (x, y, depth)
        ↓
adaptive curve subdivision
        ↓
numerical depth(t) = 0 horizon roots
        ↓
visible open planar fragments
```

No artificial chord is added through the hidden hemisphere or along the globe limb.

## 2. Authalic globe

The globe uses the same WGS84 authalic transform and authalic radius as the equal-area reference core.

For a canonical geodetic point `(lambda, phi)`, AERIS first evaluates authalic latitude

```text
beta = authalic_latitude(phi)
```

and converts `(lambda, beta)` to the unit sphere.

This keeps the globe view on the same authalic surface used by the equal-area primitive reference math. It is not an ellipsoidal perspective renderer.

## 3. View-space convention

The existing orthographic view convention is:

```text
camera looks toward the origin along +X view axis
screen-right = +Y
screen-up    = +Z
positive depth = visible hemisphere
```

After the world-to-view rotation, a sphere point `v` becomes `v'` and the screen coordinates are

```text
x_screen = R_q * v'.y
y_screen = R_q * v'.z
depth    = R_q * v'.x
```

where `R_q` is the WGS84 authalic radius.

A point is on the mathematical horizon when

```text
depth = 0
```

and is on the visible hemisphere when

```text
depth >= 0.
```

## 4. Camera-center construction

For a requested camera center with longitude `lambda_0` and authalic latitude `beta_0`, the current reference camera uses

```text
world_to_view = R_y(beta_0) * R_z(-lambda_0)
```

under the existing AERIS rotation-matrix convention.

When the public input latitude is geodetic, it is converted to authalic latitude before constructing this rotation.

The pinned Natural Earth globe proof currently uses:

```text
camera center longitude:       +15 degrees
camera center geodetic latitude: +20 degrees
```

## 5. Canonical edge semantics remain unchanged

The horizon layer consumes the existing canonical `wgs84-linear-v1` edge.

For source endpoints `(lambda_0, phi_0)` and `(lambda_1, phi_1)`, the geographic interpolation remains

```text
lambda(t) = lambda_0 + t (lambda_1 - lambda_0)
phi(t)    = phi_0    + t (phi_1    - phi_0)
0 <= t <= 1.
```

Longitudes may already be unwrapped outside `[-pi, pi]`. The globe layer MUST NOT independently rewrap them and thereby alter source-edge traversal.

Authalic latitude is evaluated from each interpolated geodetic latitude sample. AERIS does not linearly interpolate authalic latitude as a replacement edge model.

## 6. Adaptive 3D subdivision

The reference implementation evaluates five samples for an interval:

```text
t = 0, 1/4, 1/2, 3/4, 1
```

relative to that recursive interval.

Geometric refinement is measured in full view-space `(x, y, depth)`, not only in the two screen coordinates. Intermediate samples are compared with the 3D chord joining the interval endpoints.

An interval can be emitted only when both of the following are satisfied:

1. the 3D geometric deviation is within the configured tolerance;
2. the five-sample visibility pattern is topologically resolved for the endpoint state:
   - equal endpoint visibility requires zero sampled visibility transitions;
   - different endpoint visibility requires exactly one sampled transition.

Otherwise the interval is subdivided recursively, subject to the configured depth and segment limits.

### 6.1 Important non-claim

This adaptive five-sample rule is a deterministic numerical reference contract. It is **not** interval arithmetic and does not constitute a formal proof that an adversarial smooth curve cannot contain an unsampled pair of horizon crossings inside an accepted interval.

Synthetic conformance cases, geometric-tolerance refinement, cross-platform tests, and pinned real-world datasets constrain the implementation, but AERIS MUST NOT describe the current classifier as a mathematically exhaustive root enclosure.

A future stronger reference may add analytical bounds or interval/root isolation without changing canonical geographic edge semantics.

## 7. Horizon root solve

When an accepted interval has one visible endpoint and one hidden endpoint, AERIS brackets a sign-changing horizon root and solves

```text
depth(t) = 0
```

by deterministic bisection.

The solve terminates only when the sampled absolute depth is within the configured `horizon_tolerance_m`, or when finite-precision parameter collapse still leaves one bracket endpoint within that same tolerance.

Failure to meet the contract within `max_root_iterations` is `horizon_non_convergence`.

A solved horizon point is emitted as the endpoint of the visible fragment. The hidden continuation is not emitted.

## 8. Visible edge and ring results

A single canonical edge may emit:

- one fully visible polyline;
- one visible fragment terminated at a horizon point;
- one visible fragment beginning at a horizon point;
- no visible geometry.

A complete canonical ring applies the same contract to every ordinary and closing edge, then merges adjacent visible fragments that remain continuous through source vertices.

If visibility is interrupted by the hidden hemisphere, the returned object remains an **open** planar polyline. The ring API does not invent a closing segment across the hidden hemisphere or along the circular limb.

Fully visible rings remain closed through ordinary edge continuity.

## 9. Closed-ring horizon parity

For ordinary sign-changing intersections, a closed continuous ring must enter and leave the visible hemisphere in pairs.

The real-world diagnostic therefore requires an even number of reported horizon crossings for every closed source ring.

This is an integration invariant for the currently supported sign-changing crossing class. A tangent contact may touch `depth = 0` without changing visibility and therefore is not necessarily counted as a crossing by this rule.

Exact source vertices on the horizon and tangent ownership remain topology cases to specify explicitly if they become semantically significant; they must not be hidden behind an arbitrary epsilon nudge.

## 10. Resource bounds and failure behavior

The current public options include:

```text
geometric_tolerance_m
horizon_tolerance_m
max_subdivision_depth
max_root_iterations
max_segments
```

Non-finite or non-positive quality tolerances and zero limits are invalid options.

Exceeding subdivision or segment limits fails explicitly with `limit_exceeded`. AERIS MUST NOT silently skip difficult edges, lower quality, or replace missing fragments with straight chords.

A non-convergent horizon solve also fails explicitly.

## 11. Pinned Natural Earth globe proof

The Source Compatibility workflow runs the same exact pinned Natural Earth `ne_110m_land` source bytes used by the planar conformance proof through the production Shapefile decoder and this globe-curve path.

The first successful reference globe used:

```text
source records:                 127
source rings:                   128
source vertices:                5015
normalized boundary coordinates: 11
camera center:                  15 E, 20 N geodetic
geometric tolerance:            5000 m
horizon root tolerance:         0.01 m
```

The resulting visible geometry contained:

```text
visible source rings:       66
visible planar fragments:   74
horizon crossings:          26
projected vertices:         2764
maximum subdivision level:  1
```

Every partially visible fragment was checked to terminate on the globe limb within the diagnostic radial tolerance, every emitted point remained inside the visible disk, and every closed source ring produced an even count of sign-changing horizon crossings.

The generated SVG contains coastline wireframe only. It does not use circular clipping to manufacture coastline termination.

## 12. Why land is not filled yet

A wireframe coastline can end at two horizon intersections without inventing any additional boundary.

A filled visible polygon is different: when the geographic interior reaches the hidden hemisphere, the visible region must be closed along the correct horizon arc. Which arc belongs to the interior is a topological decision analogous to projection-seam closure, not a styling operation.

Therefore the current globe proof deliberately does **not** fill land polygons.

A future visible-surface polygon contract must explicitly define:

- horizon-arc ownership from canonical ring interior semantics;
- multiple visible components;
- holes whose exterior or hole boundary crosses the horizon;
- tangent and exact-horizon contacts;
- orientation of derived limb arcs;
- signed/semantic area or other independent topology checks appropriate to an orthographic view.

Until that exists, adding a decorative fill would overstate what AERIS has actually proved.

## 13. Relationship to map projection topology

Projection seams and globe horizons are separate derived topologies.

A map projection seam is a cut introduced by a planar projection domain. A globe horizon is a visibility boundary introduced by the camera.

Both consume the same canonical WGS84 geometry and both must preserve that source geometry rather than mutate it, but their connector/closure rules are different and must remain separate modules.
