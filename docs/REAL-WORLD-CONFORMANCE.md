# AERIS — Real-World Conformance Proofs

**Status:** IMPLEMENTED DEVELOPMENT GATE  
**First proof dataset:** Natural Earth `ne_110m_land`, release snapshot `v5.1.2`

This document records reproducible real-world integration proofs executed by AERIS against exact third-party source bytes.

These proofs supplement, but do not replace, synthetic unit/property/conformance tests. Their purpose is to expose assumptions that ideal fixtures do not contain: real multipart geometry, antimeridian structure, polar winding, projection-seam splitting, globe-horizon visibility, source floating-point tails, provider conventions, and long irregular coastlines.

## 1. Separation from ordinary CI

The ordinary AERIS CI remains network-independent.

Real upstream compatibility is checked by the separate `Source Compatibility` workflow. That workflow obtains exact pinned bytes, verifies their identities, passes them through the production acquisition/adapter/geometry/projection/view paths, and emits diagnostic artifacts.

Relevant source, geometry, projection, and view changes are checked on pull requests as well as on `main`, so pinned-source compatibility is a pre-merge development gate rather than only a post-merge signal.

Failure of a live mirror or network path therefore does not redefine mathematical correctness of the core. Conversely, a change in source decoding, canonical geometry, projection topology, or visible-globe topology wakes the real-world gate so those assumptions are exercised again.

## 2. Natural Earth 110m land proof

### 2.1 Upstream identity

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

The Natural Earth file `ne_110m_land.VERSION.txt` inside that snapshot contains:

```text
4.1.0
```

AERIS intentionally records release snapshot and internal dataset version as separate values.

### 2.2 Exact resource identities

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

The AERIS aggregate manifest/content identity for this exact verified set is:

```text
5a9d2b70be942d7d0602ef299afe0ef039463831ade478aae11091f8c202cf6e
```

This aggregate identity is derived from the verified manifest/resource identities, not from Git object IDs.

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

### 3.2 Visible authalic-globe path

The same pinned Shapefile bytes are also passed through:

```text
strict Polygon Shapefile decoder
        ↓
wgs84-linear-v1 canonical rings
        ↓
WGS84 geodetic latitude -> authalic latitude
        ↓
reference world-to-view rotation
        ↓
orthographic (x, y, depth)
        ↓
adaptive 3D curve subdivision
        ↓
depth(t) = 0 horizon root solve
        ↓
visible open coastline fragments
        ↓
wireframe globe SVG
```

The globe diagnostic does not use a circular clip path to manufacture coastline termination and deliberately does not fill land polygons.

No GDAL/OGR, PROJ geometry transformation, or external polygon engine participates in either proof path.

## 4. Real source findings that changed AERIS

The proof was intentionally allowed to fail until the production assumptions matched the real geometry.

### 4.1 Boundary floating-point tail

Natural Earth contains coordinates such as:

```text
180.00000000000014 degrees
```

in the Antarctic record.

This is five representable binary64 steps beyond exactly `180.0`, not a meaningful geographic excursion.

AERIS therefore accepts and canonicalizes only a tightly bounded number of representable floating-point steps beyond exact WGS84 angular boundaries. The current Shapefile reader permits at most eight outward `nextafter` steps and counts every such normalization for auditability. Material violations remain errors.

The pinned Natural Earth snapshot currently requires eleven individual boundary-coordinate normalizations.

### 4.2 Real refinement depth

A real non-polar ring demonstrated that the integration proof needed more refinement rounds than the first synthetic fixtures.

The production area tolerance was not weakened. The proof ceiling was increased so the existing tolerance could be reached.

### 4.3 Polar winding and interior topology

Antarctica exposed non-zero longitude winding. A closed spherical/ellipsoidal curve does not by itself identify which of its two regions is intended.

AERIS therefore added explicit canonical `RingInteriorSide` topology and a global winding-aware WGS84 area branch. It does not silently choose the smaller region.

### 4.4 Projection seam closure

Naively projecting the Antarctic coastline and connecting its final planar endpoints with a straight chord produced the wrong area even after hundreds of thousands of subdivision vertices.

The error was topological, not a lack of sampling density.

AERIS now cyclically rebases a supported single-winding polar ring at its exact active projection-seam intersection and closes the derived planar region along both sides of the projection boundary through the selected pole. The seam boundary itself is adaptively subdivided.

The pinned Antarctic ring has one exact active-seam crossing at approximately:

```text
longitude: +180 degrees
latitude:  -84.71338 degrees
```

The source ring begins elsewhere, so this case also proves that polar seam topology is independent of the provider's arbitrary starting vertex.

### 4.5 Generic zero-winding seam topology

The normal Natural Earth 110m land snapshot is already organized conveniently around the usual `lambda_c = 0` world cut. Under that view its zero-winding rings do not require a generic split.

AERIS therefore does not treat the normal successful render as evidence for general seam splitting.

The same exact pinned source bytes are additionally projected with a deliberately shifted central meridian of `+90 degrees`. This moves the active cut through ordinary real land geometry without changing the source dataset.

That shifted-seam proof exercises the production zero-winding splitter and its high-level piecewise projector before merge.

### 4.6 Globe horizon is not a styling clip

The visible globe path is likewise required to derive actual horizon intersections from canonical geographic edges before SVG output.

AERIS does not project the hidden hemisphere and rely on a circular SVG clip to conceal it. Each visible/hidden transition is bracketed in view-space depth and solved numerically to a `depth = 0` horizon point. Visible fragments remain open when the source ring continues across the hidden hemisphere.

The current reference uses deterministic adaptive five-sample subdivision. This is a numerical conformance contract, not interval arithmetic; its successful real-world proof must not be misrepresented as a formal exhaustive root enclosure for every adversarial smooth curve.

## 5. Successful normal-world metrics

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

The shared WGS84 semantic land-area reference for the same canonical source geometry is:

```text
1.473627388154e+14 m^2
```

The real-world gate validates every original source ring against its configured absolute/relative aggregate area budget after any derived piecewise projection. It also checks the complete semantic world-area difference against the sum of those original-ring budgets.

The diagnostic proof currently uses:

```text
relative per-ring area tolerance: 1e-7
absolute per-ring area floor:     10000 m^2
```

These are integration-proof settings, not a declaration that future maximum-quality export is limited to this tolerance.

## 6. Shifted-seam whole-world proof

The same pinned snapshot is also run with:

```text
central meridian: +90 degrees
```

For both Sinusoidal and Mollweide the production splitter reports:

```text
source rings:       128
projected pieces:   135
split source rings: 5
physical crossings: 14
```

The resulting aggregate absolute semantic-area errors were:

```text
Sinusoidal: 2.400322000000e+06 m^2
Mollweide:  4.740994906250e+06 m^2
```

Both complete worlds remained inside the same configured source-ring area-budget contract.

This is intentionally a topology stress pass rather than the displayed default world layout. It proves that the generic seam splitter is exercised by irregular pinned real-world coastlines, not only by synthetic rectangles or hand-constructed concave fixtures.

## 7. Antarctic proof detail

For the real Antarctic exterior ring, the verified WGS84 signed source area is approximately:

```text
-1.2203198759204816e+13 m^2
```

The sign reflects its explicit clockwise/right-side topology.

In the successful normal-world proof:

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

## 8. Authalic orthographic globe proof

The first pinned real-world globe diagnostic uses:

```text
camera center longitude:         +15 degrees
camera center geodetic latitude: +20 degrees
geometric curve tolerance:       5000 m
horizon root tolerance:          0.01 m
```

The source cardinality remains exactly:

```text
records:         127
rings:           128
source vertices: 5015
boundary normalizations: 11
```

The derived visible wireframe contains:

```text
visible source rings:      66
visible fragments:         74
sign-changing crossings:   26
projected vertices:        2764
maximum subdivision level: 1
```

The diagnostic checks that:

- every emitted point remains inside the visible globe disk within the stated numerical audit allowance;
- every closed source ring reports an even number of sign-changing horizon crossings;
- every partially visible fragment begins and ends on the mathematical limb within the radial audit tolerance;
- a fully visible zero-crossing ring remains a closed visible polyline;
- no hidden continuation is replaced by a straight closing chord.

The SVG artifact is wireframe coastline only. Filled visible land remains deliberately unimplemented because correct fill requires derived horizon-arc ownership from canonical interior topology.

See `GLOBE-HORIZON-TOPOLOGY.md` for the numerical reference contract and its explicit non-claims.

## 9. What these proofs do not establish

These proofs establish the current WGS84/authalic pipeline, source normalization, canonical ring semantics, polar seam topology, generic zero-winding seam splitting, high-level piecewise area verification, independent Sinusoidal and Mollweide primitive paths, and horizon-aware orthographic coastline visibility against a real whole-world land dataset.

They do **not** yet establish:

- the final historical Philbrick Sinu-Mollweide composition parameters;
- source-edge/seam-coincident degeneracies that are still explicitly fail-closed;
- complete structured exterior/hole reassociation after derived splitting for GIS/polygon-object export;
- arbitrary multi-lobe/interrupted projection topology beyond one periodic longitude cut;
- filled visible-globe polygon topology or horizon-arc ownership;
- a formal interval-arithmetic proof that the current adaptive globe classifier isolates every possible adversarial horizon root;
- political-boundary freshness or worldview semantics;
- physical relief, imagery, or high-resolution coastline ingestion;
- final production export tolerances.

Those remain separate contracts and must not be inferred from these successful gates.

## 10. Rule for future proofs

A future real-world proof must record enough information to reproduce the exact input identity and understand what contract it exercised.

AERIS must never replace a failing historical pinned proof with a newer source snapshot merely to restore a green check. A source upgrade is a new explicit compatibility case.
