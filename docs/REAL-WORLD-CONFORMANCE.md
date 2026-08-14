# AERIS — Real-World Conformance Proofs

**Status:** IMPLEMENTED DEVELOPMENT GATE  
**First proof dataset:** Natural Earth `ne_110m_land`, release snapshot `v5.1.2`

This document records reproducible real-world integration proofs executed by AERIS against exact third-party source bytes.

These proofs supplement synthetic unit/property/conformance tests. Their purpose is to expose assumptions ideal fixtures often miss: multipart geometry, antimeridian structure, polar winding, projection-seam splitting, globe-horizon visibility, finite filled-region topology, source floating-point tails, provider conventions, and long irregular coastlines.

## 1. Separation from ordinary CI

Ordinary AERIS CI remains network-independent.

Real upstream compatibility is checked by the separate `Source Compatibility` workflow. It obtains exact pinned bytes, verifies their identities, passes them through production acquisition/adapter/geometry/projection/view paths, and emits diagnostic artifacts.

Relevant source, geometry, projection, and view changes are checked on pull requests as well as on `main`, so pinned-source compatibility is a pre-merge development gate rather than only a post-merge signal.

A live network failure therefore does not redefine core mathematical correctness. Conversely, changes in source decoding, canonical geometry, projection topology, globe visibility, or globe fill wake the real-world proof.

## 2. Natural Earth 110m land identity

Repository:

```text
nvkelso/natural-earth-vector
```

Release snapshot:

```text
v5.1.2
```

Pinned upstream commit:

```text
f1890d9f152c896d250a77557a5751a93d494776
```

The upstream `ne_110m_land.VERSION.txt` contains:

```text
4.1.0
```

AERIS intentionally records release snapshot and internal dataset version separately.

### 2.1 Exact resources

```text
geometry.shp
  upstream: ne_110m_land.shp
  bytes:    89504
  sha256:   8689e6932b8e370e2ca4587cf3ba21e460b1235db37b6ed3c172c35b4a6088de

crs.prj
  upstream: ne_110m_land.prj
  bytes:    147
  sha256:   3259f0e55290a82b1350646f604e8a7ee1e2136c0320a40fad838ab40819fff8

dataset.version
  upstream: ne_110m_land.VERSION.txt
  bytes:    6
  sha256:   3b10b6ad566eadbcacadb33c591f1ec629593d6adf47442e56e0f61996829ef7
```

Aggregate verified snapshot/content identity:

```text
5a9d2b70be942d7d0602ef299afe0ef039463831ade478aae11091f8c202cf6e
```

This is derived from the verified resource manifest, not from Git object IDs.

## 3. Pipelines proved

### 3.1 Equal-area planar path

```text
pinned upstream bytes
        ↓
resource SHA-256 and size verification
        ↓
VerifiedSnapshot
        ↓
NaturalEarthLand110mAdapter
        ↓
strict Polygon Shapefile decoder
        ↓
provider topology -> canonical left/right interior semantics
        ↓
wgs84-linear-v1 canonical rings
        ↓
WGS84/authalic area reference
        ↓
projection seam topology
        ↓
zero-winding geographic pieces OR polar winding closure
        ↓
independently verified equal-area planar pieces
        ↓
final original-ring aggregate area-budget verification
        ↓
Sinusoidal / Mollweide diagnostic SVG
```

### 3.2 Authalic globe curve path

```text
verified canonical rings
        ↓
WGS84 geodetic -> authalic latitude
        ↓
reference world-to-view rotation
        ↓
orthographic (x, y, depth)
        ↓
adaptive 3D curve subdivision
        ↓
depth(t) = 0 horizon roots
        ↓
visible open coastline fragments
        ↓
wireframe globe SVG
```

### 3.3 Verified filled-globe path

```text
verified canonical rings + RingInteriorSide
        ↓
horizon-aware visible source chains
        ↓
orientation-owned globe-limb connectors
        ↓
finite closed visible components
        ↓
component-orientation verification
        ↓
consecutive topology-stability verification
        ↓
orthographic screen-area convergence verification
        ↓
independently recomputed coastline overlay
        ↓
filled globe SVG
```

Orthographic screen area is used only as a convergence observable between finite approximations. It is **not** compared with WGS84 geographic area as an equal-area invariant.

No GDAL/OGR, PROJ geometry transformation, or external polygon engine participates in these proof paths.

## 4. Real source findings that changed AERIS

The proof was intentionally allowed to fail until production assumptions matched real geometry.

### 4.1 Boundary floating-point tail

Natural Earth contains Antarctic coordinates such as:

```text
180.00000000000014 degrees
```

This is a few representable binary64 steps beyond exactly `180.0`, not a meaningful geographic excursion.

AERIS therefore accepts and canonicalizes only a tightly bounded number of representable floating-point steps outside exact WGS84 angular boundaries. The Shapefile reader permits at most eight outward `nextafter` steps and counts every normalization. Material violations remain errors.

The pinned snapshot requires eleven individual boundary-coordinate normalizations.

### 4.2 Real equal-area refinement depth

A real non-polar ring required more equal-area refinement rounds than the first synthetic fixtures.

The production area tolerance was not weakened. The proof ceiling was increased so the existing tolerance could be reached.

### 4.3 Polar winding and interior topology

Antarctica exposed non-zero longitude winding. A closed spherical/ellipsoidal curve does not by itself identify which of its two complementary regions is intended.

AERIS therefore added canonical `RingInteriorSide` and a winding-aware WGS84 area branch. It never silently chooses the smaller region.

### 4.4 Projection seam closure

Naively projecting Antarctic coastline and connecting final planar endpoints with a straight chord produced the wrong area even after extreme subdivision.

The error was topological, not resolution-related.

AERIS now rebases supported single-winding polar rings at the exact active projection-seam intersection and closes the derived planar region along both sides of the projection boundary through the selected pole.

The pinned Antarctic ring has one active-seam crossing at approximately:

```text
longitude: +180 degrees
latitude:  -84.71338 degrees
```

### 4.5 Generic zero-winding seam topology

The normal `lambda_c = 0` Natural Earth world does not force ordinary zero-winding seam splits, so it is not used as evidence for the generic splitter.

A second pass moves the active central meridian to `+90 degrees`, deliberately cutting real land geometry while keeping the exact same source bytes.

That pass exercises the production zero-winding splitter and high-level piecewise projector before merge.

### 4.6 Globe horizon is not a styling clip

AERIS derives actual horizon intersections from canonical geographic edges before SVG output. It does not project the hidden hemisphere and rely on circular clipping to hide it.

The reference adaptive five-sample classifier is deterministic numerical conformance, not interval arithmetic. Successful real-world proof must not be described as formal exhaustive root enclosure for all adversarial smooth curves.

### 4.7 Filled globe needs stricter finite geometry than wireframe

The first real filled-globe attempt reused the successful wireframe tolerances:

```text
curve geometric tolerance: 5000 m
horizon arc tolerance:       500 m
```

The derived Antarctic topology was structurally correct: ten horizon crossings produced five visible components. However, several components were extremely thin slivers adjacent to the lower globe limb. Coarse finite coastline chords crossed the derived limb boundary and changed component signed orientation.

This was not repaired by hardcoding a smaller “Antarctica tolerance”. AERIS added a separate verified fill layer that adaptively refines every source ring until component orientation, topology count, and consecutive orthographic screen-area approximations stabilize under the declared budget.

The same real Antarctic ring demonstrated the transition clearly:

```text
5000 m / 500 m  -> aggregate orientation wrong
2500 m / 250 m  -> aggregate orientation wrong
1000 m / 100 m  -> aggregate sign corrected, some components still wrong
 500 m /  50 m  -> some components still wrong
 100 m /  10 m  -> one tiny component still wrong
  50 m /   5 m  -> all five component orientations correct
  10 m /   1 m  -> stable refined result
```

The verified high-level path now discovers required refinement itself.

## 5. Normal equal-area whole-world metrics

The normal `lambda_c = 0` proof contains:

```text
features:            127
source rings:        128
projected pieces:    128
source vertices:     5015
winding rings:       1
ordinary splits:     0
ordinary crossings:  0
```

### 5.1 Sinusoidal

```text
projected vertices:       74990
semantic land area:       1.473627311683e+14 m^2
aggregate absolute error: 7.647068937500e+06 m^2
maximum ring error:       4.790256968750e+06 m^2
```

### 5.2 Mollweide

```text
projected vertices:       78692
semantic land area:       1.473627314412e+14 m^2
aggregate absolute error: 7.374163312500e+06 m^2
maximum ring error:       6.504465843750e+06 m^2
```

Shared WGS84 semantic land-area reference:

```text
1.473627388154e+14 m^2
```

The integration proof currently uses:

```text
relative per-ring area tolerance: 1e-7
absolute per-ring area floor:     10000 m^2
```

These are integration-proof settings, not a maximum-quality export ceiling.

## 6. Shifted-seam whole-world proof

With:

```text
central meridian: +90 degrees
```

both Sinusoidal and Mollweide report:

```text
source rings:       128
projected pieces:   135
split source rings: 5
physical crossings: 14
```

Aggregate absolute semantic-area errors:

```text
Sinusoidal: 2.400322000000e+06 m^2
Mollweide:  4.740994906250e+06 m^2
```

Both remain inside the configured original-source-ring area budgets.

## 7. Antarctic equal-area proof detail

Verified WGS84 signed source area:

```text
-1.2203198759204816e+13 m^2
```

The negative sign reflects its clockwise/right-side topology.

Successful normal-world proof:

```text
Sinusoidal
  refinement rounds: 10
  planar area:        -1.2203197597627400e+13 m^2
  absolute error:      1.161577416e+06 m^2
  allowed error:       1.220319876e+06 m^2
  projected vertices:  12652

Mollweide
  refinement rounds: 11
  planar area:        -1.2203198227191242e+13 m^2
  absolute error:      5.32013574e+05 m^2
  allowed error:       1.220319876e+06 m^2
  projected vertices:  23256
```

This is a direct final-polygon area check after seam closure, not only a local Jacobian test.

## 8. Authalic orthographic wireframe proof

Camera:

```text
longitude:         +15 degrees
geodetic latitude: +20 degrees
```

Initial wireframe settings:

```text
geometric curve tolerance: 5000 m
horizon root tolerance:    0.01 m
```

Result:

```text
records:                    127
rings:                      128
source vertices:            5015
boundary normalizations:      11
visible source rings:         66
visible fragments:            74
sign-changing crossings:      26
projected vertices:          2764
maximum subdivision level:      1
```

Every emitted point remains inside the visible disk; partially visible fragments terminate on the mathematical limb; hidden continuation is never replaced with a closing chord.

## 9. Verified filled-globe proof

The filled proof starts deliberately from coarse finite settings:

```text
curve geometric tolerance:     5000 m
horizon arc tolerance:           500 m
horizon root tolerance:         0.01 m
relative screen-area stability: 0.5%
absolute screen-area stability: 1 m^2
maximum proof rounds:           18
```

Every source ring is independently refined until verified. The final whole-world result is:

```text
features:                       127
source rings:                   128
source vertices:                5015
visible fill source rings:       66
partial fill source rings:        5
closed fill rings:               71
fill vertices:                 3078
derived horizon arc segments:   293
horizon crossings:               26
independent coastline parts:     74
independent coastline vertices: 2859
maximum refinement rounds used:  12
finest final curve tolerance:    2.44140625 m
finest final arc tolerance:      0.244140625 m
```

The maximum observed difference between the two accepted consecutive orthographic screen-area approximations for any source ring was:

```text
1.9383331331523438e+09 m^2
```

The largest corresponding allowed stability delta in the proof was:

```text
2.5346669551220059e+11 m^2
```

These numbers are view-space convergence telemetry. They are **not geographic area errors**.

The coastline overlay is recomputed independently using each ring's final verified curve tolerance. Its complete horizon-crossing count equals the filled geometry count.

Machine inspection of the accepted SVG found:

```text
clipPath elements:          0
feature-level land paths:  65
closed land subpaths:      71
open coastline paths:      74
coastline close commands:   0
```

Thus land closure comes from explicit derived horizon geometry, while coastlines remain open where they meet the limb.

## 10. What these proofs establish

The current proofs establish against exact pinned real-world land data:

- verified source identities and provenance boundaries;
- strict Shapefile normalization;
- canonical `wgs84-linear-v1` rings and explicit interior side;
- winding-aware WGS84 area;
- polar projection seam closure;
- generic zero-winding projection seam splitting;
- piecewise equal-area aggregate verification;
- independent Sinusoidal and Mollweide reference paths;
- horizon-aware orthographic coastline visibility;
- explicit derived filled-globe horizon arcs;
- multiple visible globe components;
- adaptive verified filled-globe refinement near the limb.

## 11. What these proofs do not establish

They do **not** establish:

- final historical Philbrick Sinu-Mollweide composite parameters;
- arbitrary multi-lobe/interrupted projection topology beyond the current periodic cut contract;
- formal interval-arithmetic isolation of every possible adversarial horizon root;
- complete ownership semantics for every tangent/exact-horizon degeneracy;
- proof that ring/crossing counts alone detect every theoretical topology change between globe-fill refinements;
- political-boundary freshness or worldview semantics;
- physical relief, imagery, or high-resolution coastline ingestion;
- production styling, lighting, or the animated `Unfold` transition;
- final production export/view tolerances;
- final GUI behavior.

These remain separate contracts and must not be inferred from successful gates.

## 12. Rule for future proofs

A future real-world proof must record enough information to reproduce exact input identity and understand what contract it exercised.

AERIS must never replace a failing historical pinned proof with a newer source snapshot merely to restore a green check. A source upgrade is a new explicit compatibility case.
