# AERIS — UI Architecture Contract

**Status:** DRAFT — implementation contract

The AERIS interface is a view and command surface over the application model. It is not the source of truth and must not contain hidden business logic required for project correctness.

## 1. Workbench model

AERIS uses one primary workbench rather than a collection of unrelated modal windows.

The intended interaction model is:

- one central map canvas/work surface;
- contextual tool strip on the left;
- compact top-level category menus that open non-blocking palettes or panels;
- movable, dockable, stackable, splittable panels where that improves work;
- explicit workspaces may preserve panel arrangements without changing project semantics;
- controls appear because the current task needs them, not because every possible function must remain visible at once.

The interface should remain modern, calm, dense when useful, and free of decorative complexity that competes with the map.

## 2. Globe, projection, and unfolding views

The central work surface MUST be able to present the same canonical geographic model in more than one representation without duplicating project data.

At minimum AERIS is intended to support:

- a rotatable globe view of the canonical WGS84/authalic geometry;
- a flat projection view using the selected equal-area projection contract;
- an optional comparison/transition view used to explain how the spherical surface becomes the planar map;
- seam/cut overlays that refer to the same projection regions in both globe and planar views.

The globe is not a decorative preview. It is a first-class inspection surface for projection orientation, cut placement, source geometry, disputed boundaries, and other spatial state that is easier to understand on the sphere.

### 2.1 Camera rotation is not projection rotation

Free globe dragging changes only the viewing camera/session state.

It MUST NOT silently mutate the mathematical orientation, central meridian, seam plan, or other project projection parameters.

If the user wants the current globe orientation to become part of the projection, that change MUST be explicit through a command such as `Use current orientation for projection` or an equivalent operation.

This separation prevents ordinary inspection from changing reproducible map output.

### 2.2 Unfolding

An `Unfold` interaction may animate the transition from the spherical representation to the selected planar projection.

The animation is explanatory UI. Intermediate animation frames are not normative cartographic states and are not used as geometry for export or area verification.

The two normative endpoints are:

1. the canonical spherical/authalic representation before planar projection;
2. the final geometry produced by the selected projection contract, seam ownership rules, and adaptive subdivision requirements.

AERIS MUST compute the final planar geometry directly from the projection pipeline rather than deriving it from the visual animation.

Until the interpolation/animation contract is implemented, the UI should expose `Unfold` as unavailable rather than provide a visually plausible but mathematically undefined transition.

### 2.3 Cut-plan inspection

When an interrupted or region-based projection is active, the globe view should be able to show the exact seam/cut plan on the spherical surface before unfolding.

The same seam identifiers and region ownership used by the projection engine should drive this overlay. The UI must not maintain a visually similar but independent copy of seam geometry.

This makes it possible to inspect what will be separated, rotate the globe for context, and then unfold the same defined topology into the planar representation.

## 3. Command architecture

Every user-visible mutation MUST map to a shared application command or equivalent explicit API boundary.

The same semantic operation should be reusable by:

- UI controls;
- keyboard shortcuts;
- automation;
- tests;
- future scripting or CLI surfaces.

A button is therefore an invocation surface, not the implementation of the operation.

## 4. Non-blocking computation

Heavy work MUST NOT execute on the UI event thread.

Projection rebuilds, ingestion, validation, large exports, satellite processing, topology repair, and other expensive operations run through a job system with explicit lifecycle and cancellation semantics where cancellation is safe.

The UI must remain responsive while work proceeds and must expose progress/state without inventing false precision.

### 4.1 Preview is not verified output

Interactive movement may require a cheaper representation than a fully verified scene. AERIS MUST distinguish those states explicitly.

The first implemented globe viewer uses the following contract:

```text
mouse drag
    ↓
horizon-aware wireframe PREVIEW
    ↓
mouse release
    ↓
background verified filled-globe computation
    ↓
VERIFIED scene
```

A preview is allowed to omit expensive derived fill geometry when that omission is visible and semantic correctness is not implied. It MUST NOT display a coarse fill as though it had passed the verified fill contract.

Flat Sinusoidal and Mollweide views in the first viewer use the verified piecewise equal-area path rather than an unlabelled preview substitute.

### 4.2 Stale job ownership

Background scene work is generation-owned.

When a newer camera/view request supersedes an older computation:

- the old job receives a cancellation token where practical;
- a monotonically newer generation becomes authoritative;
- a late result from the stale generation MUST NOT replace the current scene;
- only the owning/UI thread accepts or rejects a completed scene for presentation.

This lifecycle is part of the viewer conformance gate, not merely an implementation convention.

## 5. Durable state separation

Project state belongs to `.aeris`.

Workspace/session state belongs to the adjacent `.aeris.session` sidecar.

Transient UI widgets must not become hidden persistence authorities. The UI may reflect durable state immediately, but it must not acknowledge a project mutation before the application layer has accepted its durability contract.

## 6. Contextual tools and extensibility

Tools should be registered through explicit descriptors/manifests rather than hard-wired across unrelated widgets.

A tool declaration should be able to describe at least:

- stable tool identifier;
- human-readable localized name;
- applicable selection/context;
- command bindings;
- optional panel contribution;
- shortcut contribution;
- capability requirements.

This supports modular growth without turning the workbench into a monolith.

## 7. Panels and palettes

Panels may dock, float, stack, split, hide, and restore as workspace state.

Modal dialogs are reserved for operations that truly require a blocking decision. Ordinary configuration, inspection, layer editing, export preparation, source inspection, and style work should prefer non-blocking panels or palettes.

## 8. Theme and visual language

The visual system uses semantic theme tokens rather than widget-local hard-coded colors.

The default dark theme should use dark non-black surfaces so hierarchy remains visible. Status colors are semantic and must retain meaning across themes.

The visual hierarchy should distinguish at least:

- normal/neutral;
- active/selected;
- working/running;
- success/verified;
- warning;
- error;
- disputed/divergent state where the data model requires it;
- disabled/unavailable.

Color must not be the sole carrier of critical meaning.

The first interactive viewer already exposes textual `PREVIEW`, `VERIFYING`, and `VERIFIED` states in addition to color.

## 9. Scaling, localization, accessibility

The interface must tolerate text expansion and scaling without clipped controls or layout corruption.

Localization is a structural requirement, not a final string-replacement pass. A pseudo-locale should be used in tests before real translations exist.

Keyboard navigation, visible focus, readable contrast, and useful accessible names are part of the normal implementation contract.

Long provenance values such as content hashes must remain accessible without forcing destructive layout expansion. A compact fingerprint may be shown in a normal inspector while the full value remains available through a selectable/detail surface.

## 10. Hardware, toolkit, and renderer independence

The UI must remain usable on the CPU reference path. Optional GPU acceleration may improve map interaction, but the application must not become unusable merely because a graphics acceleration path is unavailable.

The canvas backend is replaceable behind a stable view/model boundary. Project logic and commands must not depend on a specific GUI toolkit or graphics API.

The globe and planar views may use different optimized rendering backends, but both MUST consume the same application/project model and the same canonical geometry semantics.

### 10.1 First viewer implementation

The first runnable workbench is an optional **Qt 6 Widgets** frontend:

```text
aeris_core                 Qt-free C++17 reference core
      ↑
scene builder              toolkit-independent scene computation
      ↑
Qt Widgets viewer          optional frontend adapter
```

`AERIS_BUILD_VIEWER` is disabled by default. Building/testing the reference core must not search for or require Qt.

The initial canvas uses the CPU/QPainter path deliberately. This gives AERIS a correct baseline viewer that works without a GPU-specific rendering contract. Future GPU acceleration may replace or augment the drawing backend but must not become the authority for canonical geometry, source verification, projection correctness, or verified scene semantics.

## 11. Recovery behavior

After abrupt termination or power loss, reopening AERIS should restore the project and adjacent session state as close as possible to the last durably acknowledged working point.

Recovery should feel like continuation, not like a separate emergency workflow.

The UI must not display a misleading "recovered autosave" model for edits that were already canonical project state.

## 12. First viewer conformance milestone

Before the initial interactive viewer is treated as locally runnable, its dedicated gate must prove more than process startup.

The first milestone requires exact pinned source bytes and verifies:

- `VerifiedSnapshot` plus the production Natural Earth adapter before the UI is created;
- real verified Globe, Sinusoidal, and Mollweide scene construction;
- stale verified job cancellation/ignore semantics;
- synchronous explicit preview at an intermediate camera;
- final verified scene ownership by the newest camera request;
- offscreen Qt startup;
- rendering the complete `QMainWindow` workbench to an image for visual inspection.

This is not a claim of final UI design. It is the minimum evidence required before human visual review becomes a useful part of the development loop.

## 13. Principle

AERIS should remain operable through explicit state, explicit commands, explicit jobs, and explicit capabilities.

The interface may be sophisticated. It must not be magical.
