# AERIS — Real-World Conformance Proofs

**Status:** IMPLEMENTED DEVELOPMENT GATE  
**First proof dataset:** Natural Earth `ne_110m_land`, release snapshot `v5.1.2`

This document records reproducible real-world integration proofs executed by AERIS against exact third-party source bytes.

These proofs supplement, but do not replace, synthetic unit/property/conformance tests. Their purpose is to expose assumptions that ideal fixtures do not contain: real multipart geometry, antimeridian structure, polar winding, source floating-point tails, provider conventions, and long irregular coastlines.

## 1. Separation from ordinary CI

The ordinary AERIS CI remains network-independent.

Real upstream compatibility is checked by the separate `Source Compatibility` workflow. That workflow obtains exact pinned bytes, verifies their identities, passes them through the production acquisition/adapter/geometry/projection path, and emits a diagnostic artifact.

Failure of a live mirror or network path therefore does not redefine mathematical correctness of the core. Conversely, a change in source decoding, canonical geometry, or projection topology wakes the real-world gate so those assumptions are exercised again.

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

## 3. Pipeline proved

The successful real-world path is:

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
adaptive finite planar geometry
        ↓
final per-ring area-budget verification
        ↓
Sinusoidal / Mollweide diagnostic SVG
```

No GDAL/OGR, PROJ geometry transformation, or external polygon engine participates in this proof path.

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

## 5. Successful world metrics

The first complete successful proof contains:

```text
features:        127
rings:           128
source vertices: 5015
winding rings:   1
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

The real-world gate validates every ring against its own configured absolute/relative area budget and also checks the aggregate semantic area difference against the sum of those budgets.

The diagnostic proof currently uses:

```text
relative per-ring area tolerance: 1e-7
absolute per-ring area floor:     10000 m^2
```

These are integration-proof settings, not a declaration that future maximum-quality export is limited to this tolerance.

## 6. Antarctic proof detail

For the real Antarctic exterior ring, the verified WGS84 signed source area is approximately:

```text
-1.2203198759204816e+13 m^2
```

The sign reflects its explicit clockwise/right-side topology.

In the successful proof:

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

## 7. What this proof does not establish

This proof establishes the current WGS84/authalic pipeline, source normalization, canonical ring semantics, polar seam topology, and the independent Sinusoidal and Mollweide primitive paths against a real whole-world land dataset.

It does **not** yet establish:

- the final historical Philbrick Sinu-Mollweide composition parameters;
- general multi-crossing seam splitting for arbitrary zero-winding rings;
- complete polygon/hole reassociation after a general projection seam split;
- political-boundary freshness or worldview semantics;
- physical relief, imagery, or high-resolution coastline ingestion;
- final production export tolerances.

Those remain separate contracts and must not be inferred from this successful gate.

## 8. Rule for future proofs

A future real-world proof must record enough information to reproduce the exact input identity and understand what contract it exercised.

AERIS must never replace a failing historical pinned proof with a newer source snapshot merely to restore a green check. A source upgrade is a new explicit compatibility case.
