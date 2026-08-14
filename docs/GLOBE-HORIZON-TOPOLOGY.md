# AERIS — Globe Horizon Topology

**Status:** DRAFT IMPLEMENTATION CONTRACT  
**Implemented reference view:** authalic orthographic globe

This document defines the AERIS reference contract for deriving visible coastline curves and filled visible geographic regions on an authalic orthographic globe without delegating geographic visibility topology to a GUI, raster clip, or vector-file clipping primitive.

The current implementation is a deterministic CPU reference path and conformance oracle. It is not an interval-arithmetic proof of every possible adversarial horizon root.

## 1. Horizon topology is geometry

A closed geographic ring projected onto a globe is generally only partly visible.

Projecting the complete ring and applying a circular SVG clip can conceal the hidden hemisphere visually, but it does not derive the actual visible geometry. In particular, downstream renderers would not know:

- where a canonical source edge intersects the mathematical horizon;
- which pieces of the source boundary remain visible;
- which oriented globe-limb arcs close a visible filled region;
- how several visible components relate to one canonical ring.

AERIS therefore derives horizon intersections and closure topology before export.

The reference curve path is:

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

The filled-region layer then consumes those directed fragments:

```text
visible directed source chains
        +
canonical RingInteriorSide
        ↓
ordered horizon endpoints
        ↓
orientation-preserving limb connectors
        ↓
closed visible planar region components
        ↓
verified finite refinement
```

No hidden continuation is replaced by a straight chord, and no circular clip path is used to manufacture semantic geometry.

## 2. Authalic globe

The globe uses the same WGS84 authalic transform and authalic radius as the equal-area reference core.

For a canonical geodetic point `(lambda, phi)`, AERIS first evaluates

```text
beta = authalic_latitude(phi)
```

and converts `(lambda, beta)` to the unit sphere.

This places the view on the same authalic surface used by the equal-area primitive reference math. It does not make the orthographic screen projection itself equal-area, and the screen area of a visible globe polygon MUST NOT be compared with WGS84 geographic area as an equal-area invariant.

## 3. View-space convention

The reference orthographic convention is:

```text
camera looks toward the origin along +X view axis
screen-right = +Y
screen-up    = +Z
positive depth = visible hemisphere
```

After the world-to-view rotation, a sphere point `v` becomes `v'` and

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

under the AERIS rotation-matrix convention.

When the public input latitude is geodetic, it is converted to authalic latitude before constructing this rotation.

The pinned Natural Earth globe proofs currently use:

```text
camera center longitude:         +15 degrees
camera center geodetic latitude: +20 degrees
```

Free camera rotation is view state. It does not by itself change the mathematical orientation of a future flat-map projection.

## 5. Canonical edge semantics remain unchanged

The horizon layer consumes the existing canonical `wgs84-linear-v1` edge.

For source endpoints `(lambda_0, phi_0)` and `(lambda_1, phi_1)`, interpolation remains

```text
lambda(t) = lambda_0 + t (lambda_1 - lambda_0)
phi(t)    = phi_0    + t (phi_1    - phi_0)
0 <= t <= 1.
```

Longitudes may already be unwrapped outside `[-pi, pi]`. The globe layer MUST NOT independently rewrap them and thereby alter source traversal.

Authalic latitude is evaluated from each interpolated geodetic latitude sample. AERIS does not replace the canonical source edge with linear interpolation in authalic latitude.

Consecutive identical source coordinates define a zero-length edge and contribute no visible segment.

## 6. Adaptive 3D curve subdivision

The reference implementation evaluates five samples for each recursive interval:

```text
t = 0, 1/4, 1/2, 3/4, 1
```

relative to that interval.

Geometric refinement is measured in full view-space `(x, y, depth)`, not only in screen coordinates. Intermediate samples are compared with the 3D chord joining the interval endpoints.

An interval can be emitted only when:

1. the 3D geometric deviation is within the configured tolerance; and
2. the sampled visibility pattern is topologically resolved for the endpoint state:
   - equal endpoint visibility requires zero sampled visibility transitions;
   - different endpoint visibility requires exactly one sampled transition.

Otherwise the interval is subdivided, subject to explicit depth and segment ceilings.

### 6.1 Important non-claim

This five-sample classifier is a deterministic numerical reference contract. It is **not** interval arithmetic and does not formally prove that an adversarial smooth curve cannot contain an unsampled pair of horizon crossings inside an accepted interval.

Synthetic conformance cases, geometric refinement, cross-platform tests, and pinned real-world data constrain the implementation, but AERIS MUST NOT describe the current classifier as exhaustive root enclosure.

A future stronger reference may add analytical bounds or interval/root isolation without changing canonical geographic edge semantics.

## 7. Horizon root solving

When an accepted interval has one visible endpoint and one hidden endpoint, AERIS brackets a sign-changing root of

```text
depth(t) = 0
```

and solves it by deterministic bisection.

The solve terminates only when sampled absolute depth is within `horizon_tolerance_m`, or when finite-precision parameter collapse still leaves a bracket endpoint within that same tolerance.

Failure within `max_root_iterations` is explicit non-convergence.

The solved horizon point becomes an endpoint of the visible fragment. Hidden continuation is not emitted.

## 8. Visible curve results

A canonical edge may emit:

- one fully visible polyline;
- one visible fragment terminated at the horizon;
- one visible fragment beginning at the horizon;
- no visible geometry.

A complete ring applies the same contract to ordinary and closing edges, then merges adjacent visible fragments that remain continuous through source vertices.

If visibility is interrupted by the hidden hemisphere, returned pieces remain **open** planar polylines. The curve layer does not invent a limb closure.

Fully visible rings remain closed through ordinary source-edge continuity.

## 9. Closed-ring horizon parity

For ordinary sign-changing intersections, a closed continuous ring must enter and leave the visible hemisphere in pairs.

The real-world integration proof therefore requires an even number of reported sign-changing horizon crossings for every closed source ring.

A tangent contact may touch `depth = 0` without changing visibility and is not necessarily counted as a crossing by this rule.

Exact source vertices on the horizon and tangent ownership remain explicit degeneracy classes. They must not be silently moved by an arbitrary epsilon.

## 10. Filled visible-region topology

A filled geographic region cannot stop at an open horizon fragment. The visible intersection of the geographic interior with the front hemisphere must be closed along the globe limb.

The fill layer consumes the directed visible source chains produced by the curve layer. It does not re-project the hidden source boundary into the plane.

Canonical `RingInteriorSide` is mandatory because the same boundary can describe complementary geographic regions.

For a partially visible ring:

- chain starts and ends must lie on the mathematical limb;
- endpoints are ordered by angular position on the limb;
- `RingInteriorSide::left` connects an end to the next compatible start in increasing limb angle;
- `RingInteriorSide::right` connects in decreasing limb angle;
- a ring with several visibility intervals may produce several independent visible output rings.

AERIS therefore does **not** use the heuristic “connect the two crossings with the shorter arc”. The same geographic boundary can intentionally select the long complementary limb arc when its canonical interior lies on the other side.

Synthetic conformance includes a four-crossing case that produces two visible components and a same-boundary left/right case that proves short-region versus long-complement behavior.

## 11. Zero-crossing minor and major regions

When a ring has no sign-changing horizon crossings, curve visibility alone does not distinguish every possible interior.

AERIS uses the canonical WGS84 semantic area and `RingInteriorSide` to classify whether the intended source region is smaller or larger than one half of the physical authalic Earth.

The comparison uses

```text
2 * pi * R_q^2
```

where `R_q` is the physical WGS84 authalic radius. It MUST NOT use a caller-selected display radius.

The supported zero-crossing cases are:

```text
minor + fully hidden  -> empty visible region
minor + fully visible -> source boundary
major + fully hidden  -> full visible disk
major + fully visible -> full limb plus source boundary as exclusion/hole
```

An area too close to exactly one hemisphere for the current numerical uncertainty is rejected as ambiguous rather than guessed.

## 12. Finite limb approximation

Derived horizon arcs are emitted as explicit finite polylines.

`horizon_arc_tolerance_m` is a sagitta-style geometric tolerance on the globe limb. The implementation determines a maximum angular step and derives the number of required arc segments from the requested angular span.

A full disk is therefore represented by a finite regular polygon, not an ideal analytical circle hidden inside the renderer.

For a full-limb polygon with `N` equal segments and view radius `R`, the exact finite polygon area is

```text
A_N = (N R^2 / 2) sin(2 pi / N)
```

and tests compare against this finite object rather than pretending the emitted geometry has exact area `pi R^2`.

Arc segment ceilings are explicit. Quality is never silently reduced to fit a resource limit.

## 13. Why one finite fill is not enough

A coarse finite coastline approximation may be acceptable for a wireframe but still be topologically unsafe for a filled region extremely close to the horizon.

Pinned Natural Earth Antarctica exposed this distinction. With an initial coastline tolerance of `5000 m` and limb-arc tolerance of `500 m`, the correct five visible components were found, but some extremely thin near-limb components acquired the wrong signed planar orientation because their finite chords crossed the derived limb boundary.

Increasing point count by a fixed arbitrary rule would hide rather than solve this problem.

AERIS therefore separates:

```text
finite globe polygon projection
```

from

```text
verified globe polygon projection
```

The former is a deterministic one-shot approximation at caller-provided tolerances. The latter proves that the finite result has stabilized sufficiently for use as verified visible fill geometry.

## 14. Verified adaptive fill refinement

The high-level verified fill starts from explicit finite tolerances and repeatedly halves both:

```text
curve geometric tolerance
horizon arc tolerance
```

The horizon root tolerance, root-iteration ceiling, subdivision depth ceiling, segment ceilings, and output-ring ceiling are not silently relaxed.

At least two successful finite approximations are required.

For partial-horizon geometry, verification requires:

1. **component orientation** — every output component has the signed orientation implied by `RingInteriorSide`;
2. **topology stability** — consecutive acceptable refinements have the same horizon-crossing count and output-ring count;
3. **screen-area convergence** — the absolute signed planar-area difference between consecutive acceptable approximations is within the declared stability budget.

The convergence observable is

```text
Delta_A = |A_n - A_(n-1)|
```

with allowed delta

```text
max(
    absolute_area_stability_tolerance,
    relative_area_stability_tolerance * max(|A_n|, |A_(n-1)|, 1),
    explicit binary64 numerical floor
)
```

This is only a convergence check between finite orthographic approximations. **Orthographic screen area is not a geographic area invariant and is never compared with WGS84 semantic area as though the view were equal-area.**

A low-level `orientation_mismatch` is refineable because it can arise from a coarse finite chord around a true thin near-limb region. Other low-level geometric failures remain fail-closed.

If stability is not reached before `max_refinement_rounds`, the verified result fails. There is no automatic quality downgrade.

## 15. Synthetic verified stress case

The verified conformance suite includes a deliberately thin region whose visible boundary is only `0.1 degree` inside the identity-camera horizon.

The proof starts deliberately coarse:

```text
curve tolerance:       5000 m
horizon arc tolerance:  500 m
relative area stability: 0.5%
absolute area stability: 1 m^2
```

With a twelve-round ceiling this fixture was correctly **rejected** because the two latest finite screen areas had not converged enough. Increasing only the proof ceiling to eighteen rounds allowed the same unchanged stability budget to converge.

This test exists specifically to prevent the verified wrapper from declaring a visually plausible but numerically unstable near-limb fill complete.

## 16. Pinned Natural Earth proofs

The Source Compatibility workflow runs the same exact pinned Natural Earth `ne_110m_land` bytes through both curve and filled-region paths.

### 16.1 Wireframe globe

```text
source records:                   127
source rings:                     128
source vertices:                  5015
normalized boundary coordinates: 11
camera center:                    15 E, 20 N geodetic
curve tolerance:                  5000 m
horizon root tolerance:           0.01 m

visible source rings:             66
visible planar fragments:         74
horizon crossings:                26
projected vertices:               2764
maximum subdivision level:        1
```

### 16.2 Verified filled globe

The filled proof starts from:

```text
curve tolerance:                  5000 m
horizon arc tolerance:             500 m
horizon root tolerance:           0.01 m
relative area stability:          0.5%
absolute area stability:          1 m^2
maximum proof rounds:             18
```

The verified whole-world result contains:

```text
visible fill source rings:        66
partial fill source rings:         5
closed fill rings:                71
fill vertices:                  3078
derived horizon arc segments:    293
horizon crossings:                26
independent coastline parts:      74
independent coastline vertices: 2859
maximum refinement rounds used:   12
finest final curve tolerance:      2.44140625 m
finest final arc tolerance:        0.244140625 m
```

The renderer artifact is produced only after every source ring passes verified fill refinement.

The coastline overlay is recomputed independently with the final curve tolerance selected for each verified source ring. Its horizon-crossing count must equal the fill layer's count before the SVG is accepted.

The resulting SVG has:

```text
clipPath elements:       0
feature-level land paths: 65
closed land subpaths:    71
open coastline paths:    74
```

Every land subpath closes through explicit derived geometry. Coastline paths remain open at the limb and contain no artificial `Z` closure.

## 17. Resource bounds and failure behavior

The curve layer exposes:

```text
geometric_tolerance_m
horizon_tolerance_m
max_subdivision_depth
max_root_iterations
max_segments
```

The fill layer additionally exposes:

```text
horizon_arc_tolerance_m
max_horizon_arc_segments
max_output_rings
```

The verified layer adds:

```text
relative_area_stability_tolerance
absolute_area_stability_tolerance_m2
max_refinement_rounds
```

Non-finite or invalid options fail at the API boundary.

Exceeding resource ceilings, horizon non-convergence, unresolved topology, unstable component orientation, or failure to reach the convergence budget are explicit errors. AERIS MUST NOT silently omit difficult geometry, select a different region, or lower quality.

## 18. Relationship to projection topology

Projection seams and globe horizons are separate derived topologies.

A map-projection seam is a cut introduced by a planar projection domain. A globe horizon is a visibility boundary introduced by the camera.

Both consume the same canonical WGS84 geometry and canonical interior semantics, but their connector rules are independent and live in separate modules.

The globe fill proof does not change or weaken any equal-area flat-map invariant.

## 19. Remaining non-claims

The current implementation and real-world proof do not establish:

- formal interval-arithmetic isolation of every possible horizon root;
- complete semantics for every tangent or exact-horizon source degeneracy;
- a proof that output-ring count alone captures every possible adversarial topology change between refinement levels;
- production styling, lighting, terrain, or 3D surface relief;
- the final animated `Unfold` transition;
- final UI/toolkit behavior.

These remain separate contracts. Successful Natural Earth conformance must not be generalized beyond what the implemented checks actually prove.
