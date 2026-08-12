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

## 2. Command architecture

Every user-visible mutation MUST map to a shared application command or equivalent explicit API boundary.

The same semantic operation should be reusable by:

- UI controls;
- keyboard shortcuts;
- automation;
- tests;
- future scripting or CLI surfaces.

A button is therefore an invocation surface, not the implementation of the operation.

## 3. Non-blocking computation

Heavy work MUST NOT execute on the UI event thread.

Projection rebuilds, ingestion, validation, large exports, satellite processing, topology repair, and other expensive operations run through a job system with explicit lifecycle and cancellation semantics where cancellation is safe.

The UI must remain responsive while work proceeds and must expose progress/state without inventing false precision.

## 4. Durable state separation

Project state belongs to `.aeris`.

Workspace/session state belongs to the adjacent `.aeris.session` sidecar.

Transient UI widgets must not become hidden persistence authorities. The UI may reflect durable state immediately, but it must not acknowledge a project mutation before the application layer has accepted its durability contract.

## 5. Contextual tools and extensibility

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

## 6. Panels and palettes

Panels may dock, float, stack, split, hide, and restore as workspace state.

Modal dialogs are reserved for operations that truly require a blocking decision. Ordinary configuration, inspection, layer editing, export preparation, source inspection, and style work should prefer non-blocking panels or palettes.

## 7. Theme and visual language

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

## 8. Scaling, localization, accessibility

The interface must tolerate text expansion and scaling without clipped controls or layout corruption.

Localization is a structural requirement, not a final string-replacement pass. A pseudo-locale should be used in tests before real translations exist.

Keyboard navigation, visible focus, readable contrast, and useful accessible names are part of the normal implementation contract.

## 9. Hardware and renderer independence

The UI must remain usable on the CPU reference path. Optional GPU acceleration may improve map interaction, but the application must not become unusable merely because a graphics acceleration path is unavailable.

The canvas backend is replaceable behind a stable view/model boundary. Project logic and commands must not depend on a specific GUI toolkit or graphics API.

## 10. Recovery behavior

After abrupt termination or power loss, reopening AERIS should restore the project and adjacent session state as close as possible to the last durably acknowledged working point.

Recovery should feel like continuation, not like a separate emergency workflow.

The UI must not display a misleading "recovered autosave" model for edits that were already canonical project state.

## 11. Principle

AERIS should remain operable through explicit state, explicit commands, explicit jobs, and explicit capabilities.

The interface may be sophisticated. It must not be magical.
