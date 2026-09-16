# AERIS — Surface Classification Contract

Status: **DRAFT — NOT YET A STABLE FORMAT CONTRACT**

This document defines the semantic boundary between a numerical elevation field and the physical/material class of the surface being presented.

It exists because a height value is not a material label. In particular, the rule `elevation >= 0 => land` is not a valid general AERIS classification rule.

## 1. Scope

This contract covers semantic classification used by physical terrain presentation and other consumers that need to distinguish what occupies a geographic surface point.

The initial class set is intentionally small:

- `unknown`;
- `water`;
- `land`;
- `grounded_ice`;
- `floating_ice_shelf`.

A later generation may refine water, vegetation, seasonal ice, urban surface, wetlands, glaciers, reefs or other material categories. Those future distinctions must not be inferred into generation 1 IDs retroactively.

This contract does **not** redefine the numerical elevation codec, projection mathematics, canonical polygon semantics or frontend color palette.

## 2. Core invariant

For every geographic sample used by terrain presentation, AERIS treats these as independent channels:

```text
numerical elevation / bathymetry
surface classification
presentation style
```

A numerical value answers a height/depth question under its declared vertical reference.

A classification value answers a semantic material/surface question.

A presentation style decides how those two inputs are visualized.

No one channel is allowed to silently substitute for another.

## 3. Required semantics

### 3.1 `unknown`

AERIS does not have enough verified semantic information to classify the point.

`unknown` is a real fail-closed state, not an alias for land or water.

A renderer may use a neutral fallback presentation, but it must not manufacture a semantic class from the sign of elevation merely to avoid an unknown region.

### 3.2 `water`

The point is classified as a persistent water surface for the purpose of this generation.

The numerical elevation channel remains independent. A positive numerical value does not automatically invalidate `water`, and a negative value is not by itself sufficient to prove `water`.

This distinction matters for inland water, vertical-datum differences, source artifacts and future non-sea-level water models.

### 3.3 `land`

The point is classified as ordinary non-ice land surface at the semantic resolution of the active classification source.

A renderer may apply a land hypsometric style using numerical elevation, but the class must come from semantic classification rather than `elevation >= 0`.

### 3.4 `grounded_ice`

The visible surface is persistent ice that is grounded on land/bedrock for the semantics of the active verified source.

Numerical elevation may represent ice-surface elevation or another explicitly identified elevation variant. Switching between ETOPO Surface and Bedrock does not silently change the semantic class.

### 3.5 `floating_ice_shelf`

The visible surface is persistent floating ice shelf according to the active verified semantic source.

It is neither ordinary land nor ordinary open water for presentation semantics, even though it occupies a marine region and its numerical elevation can be positive.

This class exists specifically so a physically meaningful positive ice-surface elevation cannot be rendered as green/ochre ordinary land merely because of its sign.

## 4. Source independence

The class set is a canonical semantic vocabulary, not a Natural Earth, ETOPO, NOAA or frontend-specific enum.

A classification may be supplied by:

- durable canonical vector sources;
- a durable classified raster/grid resource;
- a deterministic derivation from multiple durable canonical project channels;
- another future verified canonical source that can satisfy the same semantics.

The source/provider/version/provenance must remain explicit. A frontend must not embed undocumented coordinate patches or latitude heuristics and then present them as canonical classification.

## 5. Durability and reproducibility

A classification used for reproducible project presentation MUST be either:

1. durably materialized inside `.aeris`; or
2. deterministically derivable from other durable canonical `.aeris` state with no dependency on a machine-local acquisition path.

If multiple source channels are required to derive the class, the project must preserve enough provenance to explain those inputs.

An acquisition cache, temporary download, unpack directory or original source path is never the semantic classification itself.

After successful materialization, removing the acquisition source must not change classification on reopen.

## 6. Geographic sampling

Classification is evaluated at a canonical geographic position, independent of frontend projection.

The intended conceptual path is:

```text
device/surface point
    -> verified inverse surface mapping
    -> WGS84 geographic point
    -> surface classification sample
    -> numerical elevation sample
    -> presentation style
```

Globe, Sinu-Mollweide, Mollweide, Sinusoidal and future supported projections must therefore classify the same WGS84 point consistently.

A projection must not become a classification source.

## 7. Resolution and boundaries

A classification source has finite spatial resolution and topology. AERIS does not claim sub-source precision at coastlines, grounding lines or ice-shelf fronts.

Implementations MUST preserve which verified source/version produced the semantic boundary.

Where independently verified semantic sources overlap inconsistently, the implementation must follow an explicit deterministic reconciliation contract or return `unknown`. Silent source-order accidents are not a valid reconciliation rule.

## 8. Relationship to numerical elevation

The elevation channel keeps its own existing responsibilities:

- numerical sample value;
- horizontal grid/georeferencing;
- vertical reference;
- no-data handling;
- source provenance;
- resolution/LOD.

Classification does not rewrite elevation samples.

Examples:

- `water` + `-4200 m` may be styled as deep bathymetry;
- `land` + `+350 m` may be styled as low terrestrial relief;
- `grounded_ice` + `+2400 m` may use an ice/relief style;
- `floating_ice_shelf` + `+45 m` remains ice shelf, not ordinary land;
- `unknown` + `+100 m` remains semantically unknown.

These examples illustrate channel independence; exact colors are frontend policy.

## 9. Variant independence

A numerical dataset variant must not secretly redefine semantic material classes.

For example, choosing a numerical "surface" elevation versus a "bedrock" elevation may change the height represented under an ice sheet. It does not by itself decide whether the corresponding geographic point is grounded ice, floating ice shelf, land or water.

If a semantic source intentionally changes with a numerical variant, that coupling must be explicit, versioned and reproducible.

## 10. Presentation requirements

Frontends may choose different palettes, hillshade, contrast or accessibility styles, but they must consume the semantic class consistently.

At minimum:

- `water` must not enter an ordinary-land color ramp solely because numerical elevation is non-negative;
- `floating_ice_shelf` must not enter ordinary-land styling solely because numerical elevation is non-negative;
- `grounded_ice` must remain distinguishable from ordinary land when the active style claims to represent surface material;
- `unknown` must not be silently promoted to a known class by sign-based heuristics.

A purely numerical visualization mode may intentionally ignore classes, provided it is clearly a numerical field visualization rather than a claim about physical surface material.

## 11. Caching

Surface classification may be cached or rasterized for presentation performance.

Such a cache is rebuildable frontend/runtime state unless a separate durable resource contract explicitly says otherwise.

Cache identity must include every semantic input needed to prevent stale classification from crossing:

- project changes;
- source/version changes;
- classification-generation changes;
- viewport/projection state where the cached representation is projection-dependent.

A cached presentation mask must never become the hidden source of canonical truth.

## 12. Failure behavior

Classification failure must fail closed.

Examples include:

- missing durable source;
- invalid or corrupt classified resource;
- ambiguous source reconciliation;
- stale async result for another project;
- unsupported class generation.

The system may continue with a neutral/unknown presentation where product policy allows it, but it must not fabricate `land` or `water` from numerical sign as an error-recovery shortcut.

## 13. Initial implementation strategy

This contract does not mandate one source dataset, but the first implementation should prefer already-verifiable durable semantic geometry over coordinate-specific fixes.

A practical staged implementation may be:

1. derive ordinary `land` versus non-land from an existing verified durable physical-land source;
2. add verified persistent ice/ice-shelf semantic geometry;
3. resolve grounded ice and floating ice shelf explicitly;
4. rasterize/cache the resulting classes only as presentation acceleration;
5. keep ETOPO as the independent numerical elevation/bathymetry channel.

The staged implementation must not describe stage 1's non-land fallback as a complete five-class model.

## 14. Acceptance requirements

Before this contract can be considered implemented, acceptance must prove all of the following:

1. No production surface-material decision defines land as `elevation >= 0`.
2. Classification survives close/reopen after acquisition sources are removed.
3. Globe and every supported planar projection classify equivalent WGS84 points consistently.
4. ETOPO numerical detail/overview LOD remains independent of semantic class loading.
5. Switching numerical Surface/Bedrock variants does not silently relabel material classes.
6. Stale async classification/cache work cannot repaint another project or viewport generation.
7. At least one deterministic fixture contains positive numerical elevation in a non-land semantic class and proves it does not receive ordinary-land styling.
8. At least one deterministic grounded-ice sample and one floating-ice-shelf sample exercise distinct classes.
9. The reported real Antarctic anomaly is checked at an identified coordinate against the exact real source set before being declared fixed.
10. The real-data acceptance records provider/version/provenance rather than relying on a screenshot-only judgment.

## 15. Reported Antarctic anomaly

A manual Desktop run with real ETOPO Surface data exposed a conspicuous green/ochre patch near the Antarctic coast.

That observation is valid bug evidence, but the screenshot alone does not prove the patch is an ice shelf, a coastline mismatch, a georeferencing error, a stale tile, or another defect.

Therefore:

- do not hard-code a correction at the apparent screenshot location;
- do not switch globally to Bedrock merely to hide the symptom;
- do not label the exact patch as an ice shelf until its geographic coordinate is recovered and checked;
- retain a coordinate-level real-data regression once the cause is identified.

## 16. Out of scope for generation 1

This draft does not yet define:

- seasonal sea ice;
- snow cover;
- vegetation/biome classes;
- urban/artificial surfaces;
- wetland/reef classes;
- dynamic water levels;
- a stable binary classified-grid codec;
- a final conflict-resolution policy for arbitrary third-party classification sources;
- exact frontend colors.

Those can evolve without weakening the central invariant: numerical elevation and semantic surface class are separate verified channels.