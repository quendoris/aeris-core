# AERIS — Sinu-Mollweide Projection Contract

**Status:** DRAFT — NOT YET A STABLE PROJECTION CONTRACT  
**Target:** first production equal-area projection implementation for AERIS  
**Historical reference:** Allen K. Philbrick, Sinu-Mollweide, 1953

This document defines the mathematical and engineering contract AERIS intends to implement for its first world-map projection path.

The purpose is not merely to reproduce a familiar-looking map. The purpose is to preserve geographic area as a verifiable invariant from canonical WGS84 geometry through final planar rendering, while keeping the historical Philbrick Sinu-Mollweide construction identifiable and auditable.

Nothing in this draft may be treated as frozen until the unresolved historical-layout items listed near the end of this document are closed against primary or otherwise authoritative evidence.

---

## 1. Normative language

The terms **MUST**, **MUST NOT**, **SHOULD**, **SHOULD NOT**, and **MAY** describe implementation requirements for the eventual stable contract.

While this document remains DRAFT, individual constants and composition details may still change. Once a stable projection contract is published, existing meanings must not be silently changed.

---

## 2. Design objective

AERIS treats preservation of area as the primary cartographic invariant.

For a geographic region with ellipsoidal area `A_e` on WGS84 and projected planar area `A_p`, the projection pipeline is intended to satisfy:

```text
A_p = A_e
```

up to explicitly bounded numerical, clipping, and polygon-discretization error.

AERIS does **not** claim simultaneous preservation of:

- local shape;
- angle;
- point-to-point distance;
- direction;
- scale in every direction.

Those quantities may be distorted. Relative area must not be intentionally distorted.

---

## 3. Historical target

AERIS presently targets the **Philbrick Sinu-Mollweide** family rather than treating Goode Homolosine as an interchangeable name.

Authoritative and institutional references establish the following minimum facts:

- Allen K. Philbrick is associated with the Sinu-Mollweide construction in 1953;
- the construction combines Sinusoidal and Mollweide equal-area components;
- the result is equal-area;
- Philbrick's projection is distinct from the ordinary Goode Homolosine arrangement;
- the projection is often shown interrupted, although an uninterrupted form also exists.

The University of Michigan Deep Blue repository preserves Philbrick's *This Human World* and explicitly identifies his "sinumollweide" projection as the basis of thematic maps in that work.

NASA GISS G.Projector independently lists **Philbrick Sinu-Mollweide** as an equal-area fusion and treats `Sinu-Mollweide` as an alias of the Philbrick construction.

### 3.1 Historical-parameter discrepancy

There is currently an unresolved discrepancy in modern descriptions of the composition boundary:

- NASA GISS G.Projector describes the Philbrick Sinu-Mollweide as joining Mollweide and Sinusoidal at **30°S**;
- other cartographic references describe Philbrick as fusing Sinusoidal and Mollweide at approximately **40°44′S**, then rotating the result and applying interruptions.

AERIS MUST NOT silently choose one description and later present it as historical fact.

Before the projection contract is frozen, the project must resolve this discrepancy by inspecting Philbrick's own material where possible and/or an implementation whose provenance is sufficiently authoritative to establish the intended construction.

Until then:

```text
historical_philbrick_layout = UNRESOLVED
```

The component mathematics and the ellipsoid-to-authalic-sphere pipeline below are nevertheless independently specifiable and testable.

---

## 4. Coordinate pipeline

Canonical geographic input is WGS84 geodetic longitude and latitude.

The required conceptual pipeline is:

```text
WGS84 geodetic geometry
        ↓
validate / normalize topology
        ↓
convert geodetic latitude φ to authalic latitude β
        ↓
map WGS84 ellipsoid to WGS84 authalic sphere
        ↓
apply projection-orientation rotation
        ↓
select Philbrick composition region
        ↓
Sinusoidal or Mollweide forward transform
        ↓
apply region translation / interruption layout
        ↓
planar vector geometry
        ↓
render / export
```

The project MUST retain canonical geographic geometry prior to projection. Projected geometry is derived data and may be regenerated.

---

## 5. WGS84 ellipsoid constants

The initial geodetic reference ellipsoid is WGS84.

```text
a       = 6378137.0 m
1/f     = 298.257223563
f       = 1 / 298.257223563
e²      = f (2 - f)
e       = sqrt(e²)
```

Implementations MUST NOT substitute a generic mean Earth radius while still claiming ellipsoidal area fidelity.

---

## 6. Authalic transformation

The spherical Sinusoidal and Mollweide formulas preserve area on a sphere. To preserve area originating on the WGS84 ellipsoid, AERIS first maps geodetic latitude to **authalic latitude**.

Let geodetic latitude be `φ`.

Define:

```text
q(φ) = (1 - e²) * [
          sin(φ) / (1 - e² sin²(φ))
          - (1 / (2e)) ln((1 - e sin(φ)) / (1 + e sin(φ)))
       ]
```

At the pole:

```text
q_p = q(π/2)
```

Authalic latitude is:

```text
β = asin(q(φ) / q_p)
```

The authalic sphere radius is:

```text
R_q = a * sqrt(q_p / 2)
```

For WGS84, the expected radius is approximately:

```text
R_q ≈ 6371007.18091875 m
```

The implementation must derive the value from the WGS84 constants rather than depending on the rounded decimal above as canonical truth.

This transformation is the bridge that makes a spherical equal-area projection meaningful for ellipsoidal WGS84 source geometry.

### 6.1 Pole handling

At `φ = ±π/2`, implementations MUST avoid numerically unstable expressions caused by floating-point values marginally outside the valid domain of `asin`.

The ratio `q/q_p` MAY be clamped only for floating-point roundoff to the closed interval `[-1, 1]`. Material out-of-range values are errors, not candidates for silent clamping.

---

## 7. Spherical rotation

Philbrick's characteristic layout is not merely an ordinary equator-centered pseudocylindrical projection. AERIS therefore treats orientation as a first-class mathematical transform.

Rotation MUST occur on the authalic sphere before the component projection formulas are evaluated.

The implementation SHOULD represent rotation as a 3D orthonormal rotation of unit vectors rather than as a chain of ad-hoc longitude/latitude formulas.

For authalic longitude `λ` and latitude `β`:

```text
v = [
    cos(β) cos(λ),
    cos(β) sin(λ),
    sin(β)
]
```

A versioned rotation matrix `M` produces:

```text
v' = M v
```

and rotated spherical coordinates are recovered with:

```text
β' = asin(clamp(v'_z, -1, 1))
λ' = atan2(v'_y, v'_x)
```

The final stable Philbrick orientation matrix is currently **TBD** pending historical-layout verification.

The stable specification MUST publish either the matrix itself or an unambiguous equivalent parameterization and rotation order.

---

## 8. Sinusoidal primitive

On a sphere of radius `R_q`, with longitude relative to the active region's central meridian `Δλ` and authalic latitude `β`, the spherical Sinusoidal forward projection is:

```text
x_s = R_q * Δλ * cos(β)
y_s = R_q * β
```

Angles are in radians.

The longitude difference MUST be normalized according to the active region's seam policy, not blindly wrapped globally after region assignment.

The Sinusoidal component is equal-area on the sphere.

---

## 9. Mollweide primitive

For authalic latitude `β`, solve for the auxiliary angle `θ`:

```text
2θ + sin(2θ) = π sin(β)
```

Then:

```text
x_m = (2√2 / π) * R_q * Δλ * cos(θ)
y_m = √2 * R_q * sin(θ)
```

The Mollweide component is equal-area on the sphere.

### 9.1 Numerical solution for θ

AERIS MUST define one deterministic reference solver.

Newton iteration is acceptable away from the poles, but the numerically stable derivative form SHOULD be used:

```text
d/dθ [2θ + sin(2θ)] = 2 + 2cos(2θ) = 4cos²(θ)
```

The implementation MUST include explicit handling near `β = ±π/2`, where `θ → ±π/2` and naive Newton arithmetic can become unstable.

The reference implementation SHOULD use:

1. an analytic pole result at sufficiently small angular distance from the pole;
2. bounded Newton iteration in the ordinary domain;
3. a deterministic bracketing fallback if Newton fails to converge or leaves the permitted interval.

Failure to converge is an explicit projection error. It MUST NOT return a guessed coordinate.

---

## 10. Composition model

The Philbrick projection is a **fusion** rather than a weighted visual blend.

AERIS MUST model the world as explicit projection regions. Each region contains at minimum:

```text
region_id
component_projection
spherical_domain
central_meridian
rotation_contract
planar_translation
seam_definition
priority / tie-break rule
```

A coordinate is first assigned to exactly one region, then transformed by that region's projection primitive.

No point may be projected by averaging Sinusoidal and Mollweide coordinates unless a future projection explicitly defines such behavior. That would be a different projection.

### 10.1 Composition continuity

Where two regions are intended to meet without a visible gap, their translated planar boundary coordinates MUST agree within a published numerical tolerance.

A seam may be topologically interrupted while both sides remain mathematically valid representations of adjacent geographic points.

---

## 11. Interruptions and seam ownership

Interruptions exist to relocate distortion into less important regions, usually oceans, without changing area.

AERIS MUST represent interruptions as explicit geographic boundaries in the rotated spherical domain. They are not renderer clipping tricks.

A polygon crossing an interruption MUST be split into region-owned pieces before final projection.

Required behavior:

- preserve polygon winding semantics;
- preserve holes;
- preserve multipolygon identity;
- preserve feature identity across pieces;
- avoid duplicate area at seams;
- avoid omitted slivers at seams;
- use deterministic ownership for points exactly on a seam;
- record which derived piece belongs to which projection region.

### 11.1 Exact-on-seam rule

The stable specification MUST define a half-open ownership convention or equivalent deterministic tie-break rule.

For example, a seam may belong to one region on its closed side and the neighboring region on its open side. The exact convention is TBD, but ambiguity is not permitted in the stable contract.

---

## 12. Antimeridian behavior

The WGS84 antimeridian is not automatically a projection seam.

Canonical geometry normalization MUST distinguish:

- a polygon genuinely crossing ±180° longitude;
- a polygon whose coordinate encoding merely wraps there;
- a Philbrick composition seam;
- a visual interruption created after spherical rotation.

AERIS MUST NOT split polygons solely because a raw longitude jumps from `+179°` to `-179°`.

Topological unwrapping occurs before projection-region clipping.

---

## 13. Forward transform contract

The eventual public forward transform conceptually behaves as:

```text
project_wgs84(lon, lat) -> one planar result
```

for points not located on an interruption ambiguity.

Internally:

```text
1. validate lon/lat
2. convert φ -> β
3. rotate authalic-sphere coordinate
4. choose region
5. compute local Δλ
6. execute component projection
7. apply deterministic planar translation
8. return x, y, region_id
```

The region ID is part of the internal result because inverse projection and topology debugging require knowledge of region membership.

---

## 14. Inverse transform contract

AERIS SHOULD support an inverse transform for all ordinary interior points of the final projection.

Because interrupted projections may map multiple disconnected regions into a composite planar layout, inverse projection must first determine region ownership in projected space.

The eventual inverse pipeline is:

```text
projected point
    ↓
identify planar region
    ↓
remove region translation
    ↓
invert Sinusoidal or Mollweide
    ↓
invert spherical rotation
    ↓
invert authalic latitude β -> geodetic latitude φ
```

Inverse authalic latitude may use a deterministic iterative method or a sufficiently accurate explicitly specified series, but the stable contract MUST define error bounds.

Ambiguous planar boundary points MAY require an explicit region hint.

---

## 15. Area invariant

Area preservation is not established by visual similarity.

AERIS MUST verify it at multiple levels.

### 15.1 Differential area test

For WGS84 geodetic latitude `φ`, the ellipsoidal surface-area element per `dφ dλ` is:

```text
dA_e / (dφ dλ)
    = a² (1 - e²) cos(φ)
      / (1 - e² sin²(φ))²
```

A numerical Jacobian of the full forward transform away from seams should reproduce the corresponding local area scale after accounting for orientation.

This test detects local violations that polygon-level testing might hide.

### 15.2 Polygon area test

For a test polygon:

```text
A_e = authoritative ellipsoidal geodesic area
A_p = signed planar area after projection and seam splitting
```

Define relative area error:

```text
ε_rel = |A_p - A_e| / |A_e|
```

For very small polygons, the test suite MUST also use an absolute-area tolerance so division by a near-zero reference area does not produce meaningless diagnostics.

### 15.3 Aggregate conservation test

When a feature is split across multiple projection regions:

```text
A_p,total = Σ A_p,piece
```

The sum must equal the unsplit source feature's ellipsoidal area within tolerance.

This specifically tests seam clipping, not merely the component projections.

---

## 16. Draft numerical targets

The following are **engineering targets, not frozen guarantees**:

- ordinary forward coordinates should be stable to near double-precision expectations;
- component inverse/forward round trips should target angular error on the order of `1e-12 rad` where numerically well-conditioned;
- analytic or smoothly generated polygons away from seams should target relative area error no worse than `1e-10`;
- real-world clipped polygons should target relative area error no worse than `1e-8`, with tighter bounds pursued where geometry precision permits;
- no production feature may pass merely because its error is visually unnoticeable.

Before 1.0, these thresholds MUST be justified empirically on representative geometry and tightened or reformulated if they mask implementation defects.

---

## 17. Numerical edge cases

The reference implementation and tests MUST explicitly cover:

- `φ = 0`;
- `φ -> ±π/2`;
- `λ` near wrap boundaries;
- points exactly on component boundaries;
- points exactly on interruption seams;
- polygons containing a pole;
- polygons crossing the antimeridian multiple times;
- tiny islands;
- very large polygons spanning multiple regions;
- holes that cross region boundaries;
- nearly collinear seam intersections;
- duplicate adjacent vertices;
- zero-area rings;
- NaN and Infinity;
- longitudes outside the accepted canonical input range;
- floating-point values one ULP outside trigonometric function domains.

Silent repair at the mathematical projection layer is prohibited.

---

## 18. Determinism

Given identical:

- canonical WGS84 coordinates;
- projection contract version;
- orientation matrix;
- region definitions;
- seam ownership rule;
- floating-point implementation contract;

AERIS MUST produce deterministic region selection and equivalent projected coordinates within the published tolerance.

Parallelism MUST NOT change topology, seam ownership, ring ordering semantics, or area results.

---

## 19. Precision model

The CPU reference implementation MUST use IEEE-754 binary64 or a numerically stronger representation for projection arithmetic.

Storage of source coordinates at lower precision is a separate ingestion decision and MUST be reflected in provenance/precision metadata.

GPU or SIMD implementations MAY use alternative execution paths only if they pass the same conformance suite and do not silently lower precision beyond the declared backend contract.

---

## 20. Renderer separation

Projection math MUST NOT depend on:

- screen DPI;
- viewport size;
- output image dimensions;
- CSS/device pixels;
- JPEG quality;
- antialiasing settings;
- display color profile.

The projection produces planar geometry in a stable coordinate space. Renderers map that geometry to SVG, PDF, PNG, JPEG, or interactive display coordinates afterward.

A 4K export and a 100000-pixel-wide export must describe the same projected geometry.

---

## 21. Conformance fixtures

Before the projection contract becomes stable, the repository MUST contain immutable reference fixtures covering at least:

```text
projection/
    points.json
    inverse-points.json
    seam-points.json
    tissot-grid.json
    polygons-simple.geojson
    polygons-seams.geojson
    polygons-polar.geojson
    world-lowres.geojson
```

Expected results SHOULD be stored with enough precision to detect accidental algorithm changes.

A stable fixture is not regenerated merely because the implementation changes.

---

## 22. Visual diagnostic suite

Visual tests complement but do not replace numerical tests.

AERIS SHOULD generate diagnostic renders containing:

- graticule;
- Tissot indicatrices;
- region boundaries;
- interruption seams;
- central meridians;
- source-country outlines;
- per-region debug coloring;
- local area-error heatmaps produced from Jacobian sampling.

These outputs are engineering diagnostics and are not canonical map styles.

---

## 23. Dependency rule for projection math

The stable projection contract must be implementable without requiring a large GIS framework at runtime.

External libraries MAY be used for:

- independent validation;
- test-oracle comparison;
- development tooling;
- optional import/export backends.

The core authalic transformation, spherical rotation, region selection, Sinusoidal projection, Mollweide projection, seam ownership, and deterministic clipping behavior SHOULD be owned by AERIS if doing so avoids version or deployment instability.

AERIS must not become unable to open or render its own historical projects because a third-party projection library removed or changed a method.

---

## 24. Historical reproducibility vs AERIS variants

AERIS may eventually support more than one equal-area composition.

The historical Philbrick target and any later AERIS-designed variant MUST have different projection identifiers.

For example, a future custom layout that preserves area but changes interruptions is not allowed to call itself simply `Philbrick Sinu-Mollweide`.

Conceptually:

```text
philbrick-sinu-mollweide/<stable-version>
aeris-sinu-mollweide/<stable-version>
```

Exact identifiers are TBD.

This protects both reproducibility and historical attribution.

---

## 25. Items blocking stable freeze

The following items MUST be closed before this document may become a stable projection contract:

1. verify Philbrick's original 1953 construction from the strongest available historical source;
2. resolve the modern `30°S` versus `40°44′S` descriptions;
3. publish the exact spherical rotation convention and constants;
4. publish the exact interruption geometry used for the historical Philbrick mode;
5. define exact region translations;
6. define seam ownership and boundary tie-breaking;
7. implement forward and inverse reference transforms;
8. validate WGS84 authalic area preservation independently;
9. build immutable conformance fixtures;
10. run differential, polygon, seam, polar, and destructive numerical tests;
11. compare against at least two independent implementations or references where possible;
12. document any intentional deviation from Philbrick as an AERIS-specific projection rather than silently changing history.

Until these are closed, AERIS may experiment with renders but must label them as draft projection output.

---

## 26. References and evidence hierarchy

AERIS prefers original and institutional technical sources over modern visual popularity.

Current references include:

- Allen K. Philbrick, *This Human World*; reprint preserved by the University of Michigan Deep Blue repository. The repository explicitly identifies Philbrick's "sinumollweide" projection as the basis for thematic maps in the work.  
  https://deepblue.lib.umich.edu/items/3d64fa5f-588c-4d14-a843-153e92952a22

- NASA Goddard Institute for Space Studies, G.Projector projection list. Lists **Philbrick Sinu-Mollweide** as an equal-area fusion and `Sinu-Mollweide` as its alias.  
  https://www.giss.nasa.gov/tools/gprojector/help/projections/

- NASA Goddard Institute for Space Studies, G.Projector changelog. Records addition of the Philbrick Sinu-Mollweide implementation.  
  https://www.giss.nasa.gov/tools/gprojector/changelog/changelog2.html

- John P. Snyder, *Map Projections — A Working Manual*, U.S. Geological Survey Professional Paper 1395. Used as an authoritative reference for standard projection mathematics and equal-area projection lineage.  
  https://pubs.usgs.gov/publication/pp1395

- EPSG, Lambert Azimuthal Equal Area method documentation. Used here as an authoritative published expression of the geodetic-to-authalic-latitude transform and authalic radius construction.  
  https://epsg.org/coord-operation-method_9820/Lambert-Azimuthal-Equal-Area.html

- PROJ documentation for spherical equal-area methods and authalic-sphere guidance.  
  https://proj.org/

Modern commercial or illustrative maps may be useful visual references, but they are not the authority for AERIS's mathematical contract.

---

## 27. Principle

The final implementation must be able to answer, for any rendered area:

```text
Where did these coordinates come from?
Which projection region produced them?
Which exact mathematics was used?
What source geometry was projected?
What numerical error was measured?
Does the planar area still equal the geographic area within the published bound?
```

If AERIS cannot answer those questions, the projection is not finished.
