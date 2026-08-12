# AERIS — Source Adapter Contract

**Status:** DRAFT IMPLEMENTATION CONTRACT

AERIS treats geographic content as replaceable evidence, not as part of the projection engine.

## 1. Stable system versus changing world

The following layers evolve at very different rates:

- political borders and dispute policy may change from year to year or faster;
- populated places and infrastructure may change continuously;
- coastlines and hydrography usually change more slowly;
- relief and bathymetry are comparatively slow-changing;
- satellite imagery can change daily;
- the mathematical representation, canonical geometry semantics, project format, rendering contracts, and equal-area invariant should not change merely because a source snapshot changes.

AERIS therefore forbids source-specific behavior from leaking into projection, rendering, or project-model code.

## 2. Adapter boundary

Every source enters through an adapter with a stable `adapter_id`, an explicit capability set, and a temporal class.

The adapter is responsible for:

1. locating or receiving one exact source snapshot;
2. validating source-specific structure;
3. interpreting source edge semantics;
4. normalizing geometry into the canonical AERIS geometry contract;
5. emitting stable feature identifiers where the source permits them;
6. reporting complete provenance;
7. refusing unsupported worldview/policy requests rather than guessing.

No downstream subsystem may branch on a provider name such as `Natural Earth`, `OSM`, or `geoBoundaries`.

Downstream code consumes only canonical AERIS features plus provenance.

## 3. Dataset identity, snapshot, and worldview are different things

AERIS models these separately.

```text
provider  = Natural Earth
dataset   = ne_110m_land
snapshot  = v5.1.2
worldview = ""
```

and, for a political source that supports alternate policies:

```text
provider  = ...
dataset   = admin0
snapshot  = 2027-04-15
worldview = neutral-disputed
```

A newer snapshot is not a new adapter. A different worldview is not silently treated as a newer truth.

## 4. Provenance is mandatory

A successful adapter result must include at least:

```text
provider
dataset
snapshot
source_uri
license_id
content_sha256
retrieved_at_utc
worldview (when applicable)
```

Missing provenance invalidates the successful result.

AERIS project files may later store additional source-specific metadata, but these fields are the minimum common contract.

## 5. Capabilities

Adapters advertise what they can actually supply. Initial capability classes include:

- land;
- ADM0 / political boundaries;
- ADM1;
- disputed boundaries;
- physical relief;
- hydrography;
- imagery.

The capability system is intentionally orthogonal to provider identity. A future project may compose land from one provider, current borders from another, hydrography from a third, and imagery from a fourth while using the same renderer and projection.

## 6. Temporal class

Every adapter describes the expected change rate of its content as one of:

- `timeless_or_structural`;
- `slow_change`;
- `periodic`;
- `fast_change`.

This classification does not trigger automatic updates. It informs the project/UI about freshness expectations.

AERIS never replaces a saved snapshot merely because a fresher one exists.

## 7. No silent normalization

If a source defines curves differently from `wgs84-linear-v1`, the adapter must either:

- preserve an explicitly supported canonical edge model; or
- convert the source curve to `wgs84-linear-v1` with a documented normalization error bound.

If it cannot do this within the requested quality contract, ingestion fails.

## 8. Fail closed

An adapter must fail instead of guessing when:

- the requested snapshot cannot be obtained exactly;
- the requested worldview is unsupported;
- source geometry is malformed or ambiguous;
- edge semantics are unknown;
- required provenance cannot be established;
- normalization cannot satisfy its bound.

## 9. First adapters

Natural Earth is intended as the first cartographic baseline adapter because its public-domain vector data is compact and useful for low- and medium-resolution world maps.

It is not treated as the authoritative live political state of the world.

Future adapters may include fresher political sources, OpenStreetMap-derived content, geoBoundaries, relief/bathymetry sources, and satellite imagery. These adapters must remain independently removable and replaceable.

## 10. Principle

AERIS owns the contracts.

Sources provide snapshots of the world.

Replacing a snapshot must not require rewriting the map engine.
