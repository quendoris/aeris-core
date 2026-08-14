# AERIS — Projection Seam Topology

**Status:** DRAFT IMPLEMENTATION CONTRACT  
**Implemented reference paths:** polar single-winding closure and zero-winding strip splitting

This document defines how AERIS derives finite planar polygon topology when a continuous canonical WGS84 ring meets the boundary of a selected map-projection domain.

Projection seams are derived rendering topology. They do not modify canonical source geometry, source provenance, or the semantic meaning of `wgs84-linear-v1`.

## 1. Canonical geography and projection topology are different layers

Canonical AERIS longitude is unwrapped continuously around each ring. It may therefore lie outside `[-pi, pi]` without being invalid.

For an equal-area primitive with central meridian `lambda_c`, the active uninterrupted longitude domain is

```text
[lambda_c - pi, lambda_c + pi]
```

up to equivalent copies displaced by whole turns.

A canonical ring may fit one such copy, cross one or both active boundaries, or wind globally around a pole. These cases require different derived topology. AERIS MUST NOT wrap every vertex independently and then connect the resulting planar points with arbitrary chords.

## 2. Supported topology classes

The current reference implementation distinguishes two classes.

### 2.1 Zero-longitude-winding rings

A ring with

```text
longitude_winding == 0
```

is handled by the general projection-seam splitter when it does not fit one active longitude branch.

The result is one or more zero-winding geographic pieces whose vertices all lie in the active projection domain after whole-turn longitude translation.

Each derived piece contains ordinary source-edge fragments plus derived constant-longitude seam connectors. It is then passed independently through the existing verified equal-area projector.

### 2.2 Single-winding polar rings

A ring with

```text
abs(longitude_winding) == 1
```

and explicit `RingInteriorSide` remains owned by the polar seam contract.

A supported polar ring is cyclically rebased at its unique active-seam intersection. Its planar closure follows one side of the projection boundary to the selected pole and returns along the opposite side.

The general zero-winding splitter MUST NOT silently reinterpret a polar ring as an ordinary multi-piece polygon.

Higher winding and unresolved polar topology remain fail-closed.

## 3. Whole-ring branch selection

Before splitting a zero-winding ring, AERIS checks whether one whole-turn translation places the complete unwrapped ring inside the active domain.

If so, the ring remains one piece. No `RingInteriorSide` inference is needed merely to translate longitude by an integer number of revolutions.

This avoids manufacturing seam events for a ring whose canonical branch is already geometrically continuous inside an equivalent projection domain.

## 4. Longitude-strip construction

If a zero-winding ring cannot fit one active branch, AERIS considers longitude strips

```text
L_k = lambda_c - pi + 2 pi k
R_k = L_k + 2 pi
```

for the finite set of integers `k` touched by the ring.

Every `wgs84-linear-v1` source edge is clipped against each relevant strip in its native longitude/latitude parameterization. Therefore an intersection with a seam is an exact point on the canonical coordinate-linear source edge, subject only to the reference floating-point arithmetic.

The splitter does not first project an edge and then infer where the geographic seam must have been.

## 5. Directed source chains

Within one strip, consecutive clipped source-edge fragments are joined into directed chains preserving original ring traversal.

A chain is either:

- already a closed component entirely inside the strip; or
- open, with its start and end lying on one of the two strip boundaries.

The arbitrary starting vertex of the provider ring is not topology. Therefore first and last clipped chains are cyclically merged when they are continuous through the canonical closing traversal.

## 6. Why crossing points cannot simply be paired by encounter order

A concave polygon may cross one seam four or more times. The number of connected planar components is not generally equal to the number of boundary arcs encountered in source order.

AERIS therefore does not implement the rule

```text
"take every arc between adjacent seam crossings and close it"
```

because that can connect the wrong regions.

Instead it reconstructs connected components from directed source chains and oriented boundary connectors.

A permanent synthetic conformance case crosses the active seam four times while producing exactly three connected pieces: one component on one side of the cut and two disconnected components on the other.

## 7. Oriented seam connectors

When an actual split is required, canonical `RingInteriorSide` is mandatory.

The splitter translates the intended interior side into traversal direction along each strip boundary. Open chain ends are paired with open chain starts in latitude order along that oriented boundary.

Conceptually:

```text
interior = left
    right strip boundary: increasing latitude
    left  strip boundary: decreasing latitude

interior = right
    right strip boundary: decreasing latitude
    left  strip boundary: increasing latitude
```

The resulting directed graph alternates source chains and derived seam connectors. Closed graph cycles are the geographic pieces supplied to projection.

This is a topology rule, not a visual heuristic.

## 8. Crossing ownership and counting

A physical crossing of a projection cut is incident to the two adjacent longitude strips. The implementation therefore encounters two strip incidences for one physical crossing.

The public diagnostic `seam_crossings` counts the physical event once, after validating that the incidence count is even.

This distinction matters for audit telemetry but does not alter geometry.

## 9. Geographic area partition invariant

Before any planar projection is accepted, the splitter verifies the geographic partition itself.

Let `A_source` be the signed WGS84 area of the original canonical ring and let `A_i` be the signed WGS84 area of each derived zero-winding piece, including its seam connectors.

AERIS requires

```text
sum_i A_i = A_source
```

within the accumulated numerical error contract of the WGS84 area evaluator plus bounded floating-point summation error.

Failure is `area_invariant_failed`. The implementation does not continue to rendering and hope that a later planar check hides a bad geographic split.

## 10. Piecewise equal-area projection invariant

The high-level piecewise projector composes existing verified operations rather than replacing them.

For a zero-winding ring:

```text
canonical WGS84 ring
        ↓
verified geographic seam partition
        ↓
WGS84 piece 1 ... WGS84 piece N
        ↓
existing verified single-ring equal-area projector
        ↓
planar piece 1 ... planar piece N
        ↓
signed planar area sum
```

The original ring receives one global area budget.

The measured geographic seam-partition error is reserved from that budget first. The remaining projection budget is divided among pieces so that the sum of accepted per-piece projection errors cannot exceed the remaining global allowance under the triangle inequality.

Finally AERIS independently checks

```text
abs(sum_i A_planar_i - A_source) <= global_budget
```

against the original unsplit source ring.

The final aggregate test is mandatory even though every individual piece has already passed its own projector.

## 11. Orientation and holes

Every derived piece preserves the signed orientation of its source ring.

An exterior and a hole can therefore both cross a projection seam and be split independently while retaining opposite signed semantics. Current conformance tests verify that the signed semantic sum of a split exterior and split hole is preserved through both Sinusoidal and Mollweide primitive paths.

The present SVG proof keeps all subpaths belonging to one source feature in one `fill-rule="evenodd"` path, so explicit exterior-to-piece hole reassociation is not required for that renderer.

A future structured polygon/GIS export contract MUST define explicit derived component/hole association rather than relying on SVG parity behavior.

## 12. Fail-closed cases

The current general splitter rejects rather than guesses when it encounters an unresolved topology class, including:

- non-zero longitude winding;
- an actual split with `RingInteriorSide::unspecified`;
- a source edge coincident with the active seam;
- ambiguous seam touches or duplicate crossing latitude events that do not establish unambiguous connector ownership;
- inconsistent directed-chain topology;
- a configured derived-piece limit;
- geographic area partition outside its numerical error bound.

These are explicit development boundaries. They MUST NOT be converted into silent epsilon nudges, arbitrary connector choices, or geometry deletion.

## 13. Resource bounds

`max_projection_pieces` / seam-split piece limits are safety bounds, not cartographic quality settings.

An input that would exceed the configured bound fails explicitly. AERIS must not silently drop components or merge them merely to satisfy a resource ceiling.

Subdivision depth and projected-edge segment limits remain governed separately by the verified projector.

## 14. Real-world shifted-seam proof

The pinned Natural Earth `ne_110m_land` snapshot is normally already arranged so that, with `lambda_c = 0`, its 128 source rings require no ordinary zero-winding split. Antarctica uses the separate polar winding path.

To exercise the general splitter against the same exact real source bytes, the Source Compatibility gate performs an additional full-world pass with

```text
lambda_c = +90 degrees
```

so the active seam cuts real land geometry in a different place.

The first successful shifted-seam proof produced, for both Sinusoidal and Mollweide:

```text
source rings:       128
projected pieces:   135
split source rings: 5
physical crossings: 14
```

The full semantic world area remained inside the configured aggregate projection budget in both primitive paths.

This proof establishes that the general splitter is exercised by pinned real-world geometry, not only by synthetic fixtures.

## 15. What remains unresolved

This contract does not yet claim support for every possible polygon/seam degeneracy.

Still separate work includes:

- source edges exactly coincident with a seam;
- tangent/seam-touch ownership where topology is genuinely ambiguous under finite precision;
- explicit structured reassociation of holes with derived exterior components for GIS/polygon-object export;
- projection families with multiple internal interruptions or lobe boundaries beyond one periodic longitude cut;
- the final historically verified Philbrick Sinu-Mollweide interruption/composition topology.

Those must be specified and tested independently rather than inferred from the success of the current primitive seam splitter.
