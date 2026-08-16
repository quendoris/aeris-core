# AERIS — Globe Horizon Topology

**Status:** DRAFT IMPLEMENTATION CONTRACT  
**Implemented reference view:** authalic orthographic globe

This document defines the AERIS reference contract for deriving visible coastline curves and filled visible geographic regions on an authalic orthographic globe without delegating geographic visibility topology to a GUI, raster clip, or vector-file clipping primitive.

The current implementation is a deterministic CPU reference path and conformance oracle. It is not an interval-arithmetic proof of every possible adversarial horizon root.

## 1. Horizon topology is geometry

A closed geographic ring projected onto a globe is generally only partly visible. Merely projecting the complete ring and applying a circular renderer clip does not derive the actual visible geometry.

AERIS therefore derives horizon intersections, visible open source-boundary fragments, oriented globe-limb arcs required to close visible filled regions, and multiple visible components belonging to one canonical geographic ring.

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
        ↓
RingInteriorSide + ordered horizon endpoints
        ↓
orientation-preserving limb connectors
        ↓
closed visible planar components
        ↓
verified finite refinement
```

No hidden continuation is replaced by a straight chord, and no circular clip path manufactures semantic geometry.

## 2. Authalic globe and view-space convention

The globe uses the same WGS84 authalic transform and authalic radius as the equal-area reference core. For a canonical geodetic point `(lambda, phi)`:

```text
beta = authalic_latitude(phi)
```

The reference orthographic convention is:

```text
camera looks toward the origin along +X view axis
screen-right = +Y
screen-up    = +Z
positive depth = visible hemisphere
```

After world-to-view rotation:

```text
x_screen = R_q * v'.y
y_screen = R_q * v'.z
depth    = R_q * v'.x
```

A point is on the mathematical horizon when `depth = 0` and visible when `depth >= 0`.

The orthographic screen projection is **not** equal-area. Its screen polygon area is never compared with WGS84 geographic area as an equal-area invariant.

## 3. Camera construction

For requested camera longitude `lambda_0` and authalic latitude `beta_0`:

```text
world_to_view = R_y(beta_0) * R_z(-lambda_0)
```

Public geodetic latitude is converted to authalic latitude first. Free camera rotation is view/session state and does not silently mutate flat-map projection parameters.

## 4. Canonical source-edge semantics remain unchanged

The horizon layer consumes canonical `wgs84-linear-v1` edges:

```text
lambda(t) = lambda_0 + t (lambda_1 - lambda_0)
phi(t)    = phi_0    + t (phi_1    - phi_0)
0 <= t <= 1
```

Longitudes may already be unwrapped outside `[-pi, pi]`; the globe layer MUST NOT independently rewrap them. Authalic latitude is evaluated from each interpolated geodetic-latitude sample. Consecutive identical source coordinates define a zero-length edge.

## 5. Adaptive 3D curve subdivision

Each recursive interval evaluates five samples:

```text
t = 0, 1/4, 1/2, 3/4, 1
```

Refinement is measured in full view-space `(x, y, depth)`.

An interval may be emitted only when 3D geometric deviation is within tolerance and the sampled visibility pattern is topologically resolved. Equal endpoint visibility requires zero sampled visibility transitions; different endpoint visibility requires exactly one. Otherwise the interval subdivides subject to explicit resource ceilings.

This classifier is deterministic but is **not interval arithmetic**. AERIS does not claim formal enclosure of every adversarial unsampled horizon root.

## 6. Horizon root solving and visible curves

A sign-changing horizon root of `depth(t)=0` is solved by deterministic bisection. Failure within `max_root_iterations` is explicit non-convergence.

A canonical edge may emit a fully visible polyline, a fragment ending or beginning at the horizon, or no visible geometry. Hidden continuation is not emitted. If a closed ring is interrupted by the hidden hemisphere, returned visible fragments remain open until the fill layer derives limb closure.

Ordinary sign-changing crossings of a closed continuous ring occur in pairs. Tangent contacts and exact source vertices on the horizon remain explicit degeneracy classes rather than being shifted by arbitrary epsilons.

## 7. Filled visible-region topology

A visible geographic region must close along the globe limb. `RingInteriorSide` is mandatory because one boundary can describe complementary geographic interiors.

For partial visibility, horizon endpoints are ordered by limb angle. `left` connects compatible endpoints in increasing angle; `right` connects in decreasing angle. Several visibility intervals may produce several independent output rings.

AERIS does not use “always take the shorter arc”. Synthetic conformance includes a four-crossing/two-component case and a same-boundary left/right case proving short-region versus long-complement behavior.

## 8. Zero-crossing minor and major regions

When a ring has no sign-changing crossings, AERIS uses canonical WGS84 semantic area and `RingInteriorSide` to classify the intended region against one half of the physical authalic Earth:

```text
2 * pi * R_q^2
```

This MUST use the physical WGS84 authalic radius, not display scale.

```text
minor + fully hidden  -> empty visible region
minor + fully visible -> source boundary
major + fully hidden  -> full visible disk
major + fully visible -> full limb plus source boundary as exclusion/hole
```

A region numerically too close to exactly one hemisphere is rejected as ambiguous.

## 9. Finite limb approximation

Derived horizon arcs are explicit finite polylines. `horizon_arc_tolerance_m` is a sagitta-style tolerance that determines maximum angular step.

For a full-limb polygon with `N` equal segments and view radius `R`:

```text
A_N = (N R^2 / 2) sin(2 pi / N)
```

Tests compare against this emitted finite polygon rather than an ideal analytical circle. Arc segment ceilings are explicit; quality is never silently reduced.

## 10. Why verified refinement is required

A finite coastline approximation can be visually adequate as a stroke while still inverting a very thin filled component near the horizon. Natural Earth Antarctica exposed this distinction.

AERIS therefore separates a deterministic one-shot finite globe polygon projection from a high-level verified projection that refines until numerical and topological observables stabilize.

## 11. Verified adaptive fill refinement

The verifier repeatedly halves:

```text
curve geometric tolerance
horizon arc tolerance
```

while horizon-root tolerance and all resource ceilings remain fixed. At least two acceptable finite approximations are required.

### 11.1 Significant and numerically negligible components

For each finite result AERIS computes a conservative binary64 screen-area resolution floor:

```text
F = 2048 * epsilon_double * max(
    1,
    |A_total|,
    |A_visible_disk|
)
```

For partial-horizon geometry:

```text
|A_component| > F   -> significant
|A_component| <= F  -> numerically negligible
```

A **significant** component MUST have the orientation implied by canonical `RingInteriorSide`.

A **numerically negligible** component remains present in finite geometry, but AERIS does not invent or claim a meaningful orientation for an area below the screen-area resolution of the binary64 numerical model.

This is not a deletion threshold and is not renderer simplification.

### 11.2 Stable topology and significance classification

Consecutive acceptable refinements must preserve:

- horizon-crossing count;
- output-ring count;
- ordered significant/negligible classification.

A change in any item requires further refinement. Thus a negligible classification itself must converge; a transient coarse small area cannot be accepted merely because it happens to fall below the floor once.

### 11.3 Per-component screen-area convergence

Every corresponding derived component must independently converge. For component areas `A_i,n-1` and `A_i,n`, allowed delta is:

```text
max(
    absolute_area_stability_tolerance,
    relative_area_stability_tolerance * max(
        |A_i,n-1|,
        |A_i,n|,
        1
    ),
    max(F_n-1, F_n)
)
```

The verifier records the largest per-component delta as telemetry. This prevents cancellation between unrelated components from hiding instability in an aggregate check.

### 11.4 Aggregate screen-area convergence

Aggregate signed orthographic screen area must also converge under the same absolute/relative/binary64-floor principle.

Orthographic screen area is only a convergence observable between finite approximations of the same view-space region. It is never treated as WGS84 geographic area.

### 11.5 Failure behavior

A low-level `orientation_mismatch` is refineable because a coarse chord can invert a true thin region. Other low-level geometric failures remain fail-closed.

Verification fails explicitly if significant orientation, topology/significance classification, per-component area, or aggregate area does not stabilize before `max_refinement_rounds`. There is no automatic quality downgrade.

## 12. Synthetic verified stress cases

The suite contains two complementary stress classes.

A thin but numerically significant region starts deliberately coarse at `5000 m` curve tolerance and `500 m` limb-arc tolerance. Twelve rounds were correctly insufficient; eighteen rounds allow the unchanged stability budget to converge.

A second fixture isolates a visible strip entering the front hemisphere by only `1e-7 rad`. It requires one negligible component, zero significant components, stable classification, and per-component area convergence within the explicit numerical floor.

Together these prevent both false acceptance of unresolved significant slivers and false sign claims below numerical resolution.

## 13. Pinned Natural Earth proofs

At the primary `15°E, 20°N` geodetic camera, exact pinned Natural Earth `ne_110m_land` produces:

```text
source records:                    127
source rings:                      128
source vertices:                   5015
visible source rings:               66
wireframe fragments:                74
horizon crossings:                  26
wireframe projected vertices:     2764
closed verified fill rings:         71
fill vertices:                    3078
derived horizon arc segments:      293
maximum fill refinement rounds:     12
finest final curve tolerance:        2.44140625 m
finest final arc tolerance:          0.244140625 m
```

The accepted SVG uses zero `clipPath` elements. Land closes through explicit derived geometry; coastlines remain open at the limb.

### 13.1 Arbitrary-camera numerical regression

The first executable Unfold lifecycle test used a second camera at `45°E, 10°N` and exposed Natural Earth record 111.

Across an extended diagnostic ladder its topology remained fixed at six horizon crossings and three visible rings. Two components converged to approximately:

```text
-1.66549e6 m²
-2.30124e8 m²
```

while a third horizon sliver converged around only:

```text
-28 m²
```

The conservative binary64 screen-area resolution floor for that planet-scale view is about `58 m²`. Earlier refinements therefore legitimately oscillated in sign while the component converged toward numerical zero.

This observation motivated the explicit significant/negligible semantics above. `45°E, 10°N` remains a permanent real-world Viewer CI regression camera.

## 14. Resource bounds and failure behavior

The curve layer exposes:

```text
geometric_tolerance_m
horizon_tolerance_m
max_subdivision_depth
max_root_iterations
max_segments
```

The fill layer adds:

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

Invalid options fail at the API boundary. Exceeding resource ceilings, horizon non-convergence, unresolved topology, unstable orientation of significant components, unstable significance classification, or failure of component/aggregate convergence are explicit errors.

AERIS MUST NOT silently omit difficult source geometry, choose a different geographic region, or lower quality.

## 15. Relationship to projection seams and Unfold

Projection seams and globe horizons are separate derived topologies. Both consume the same canonical WGS84 geometry and interior semantics but use independent connector rules.

The globe proof does not change any equal-area flat-map invariant.

The viewer's `Unfold` consumes independently verified globe and planar endpoint scenes. Intermediate frames are explanatory only and are specified separately in [UNFOLD-TRANSITION.md](UNFOLD-TRANSITION.md).

## 16. Remaining non-claims

The current implementation and real-world proofs do not establish:

- formal interval-arithmetic isolation of every possible horizon root;
- complete semantics for every tangent or exact-horizon source degeneracy;
- a formal proof that the current finite topology observables capture every adversarial topology change;
- production lighting, terrain, 3D surface relief, or final visual styling;
- final UI/accessibility/localization behavior.

Successful Natural Earth conformance must not be generalized beyond what the implemented checks actually prove.
