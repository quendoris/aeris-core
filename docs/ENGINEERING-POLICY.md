# AERIS Engineering Policy

**Status:** Project policy

AERIS is a small project by scope compared with an operating system or processor stack, but it is not allowed to use a lower engineering standard because of that.

The implementation should be boringly reliable, inspectable, portable, and deterministic.

## 1. Correctness before convenience

AERIS must prefer explicit failure over silent corruption, implicit substitution, guessed semantics, or best-effort behavior that changes the meaning of a project.

Examples:

- missing source snapshot → report it; do not silently fetch a newer one;
- unknown required project-format feature → refuse writable open;
- corrupt hash → report corruption; do not replace data automatically;
- invalid geometry → reject or repair through an explicit, recorded normalization step;
- unsupported export capability → report it; do not quietly downgrade quality.

## 2. Continuous durability

There is no periodic autosave model.

Once AERIS acknowledges a project mutation to the user, that mutation must be durably committed to the canonical `.aeris` file.

The application must not maintain a hidden interval during which accepted project edits exist only in volatile memory.

Project data and session/workspace state have separate durability domains:

- `example.aeris` — canonical durable project state;
- `example.aeris.session` — adjacent durable workspace/session state used to restore the exact working context after restart or power loss.

The session sidecar may contain viewport, selection, active tool, open panels, in-progress UI state, and other non-project information. Deleting it must not damage the project itself.

The sidecar is deliberately adjacent to the project rather than buried in a deep application-specific directory.

## 3. Save is not a user action

AERIS does not expose a traditional manual Save operation for ordinary editing.

- `Ctrl+S` means **Export**.
- If a valid previous export target/profile exists, `Ctrl+S` repeats that export atomically.
- If no export target/profile exists yet, `Ctrl+S` opens the export configuration.
- `Ctrl+Shift+S` means **Export As / Configure Export**.

Closing the application should not ask whether the user wants to save project edits that have already been acknowledged and committed.

## 4. Dependency policy

Dependencies are accepted for value, not convenience.

Before adding a runtime dependency, AERIS should evaluate:

- whether the required functionality is large or narrow;
- license compatibility;
- release and security maintenance quality;
- version stability;
- backward compatibility;
- supported operating systems and architectures;
- minimum hardware requirements;
- transitive dependency cost;
- deterministic behavior;
- binary distribution complexity;
- whether the dependency creates a network requirement;
- whether the project would become unable to open its own historical files without that dependency.

### Narrow dependency rule

If AERIS needs only a small, well-defined capability from a library and that library creates material compatibility, deployment, hardware, or versioning problems, a minimal internal implementation is preferred.

The size of that internal implementation is not the deciding factor. A larger amount of straightforward, owned, tested code can be preferable to a smaller amount of integration code wrapped around unstable external behavior.

This is **not** a blanket "not invented here" rule. Reimplementation is justified only when ownership materially improves reliability, portability, compatibility, auditability, or long-term maintenance.

Internal replacements must meet the same quality bar as any other core component:

- specification or documented behavior;
- unit tests;
- property/invariant tests where applicable;
- fuzz tests for parsers and binary boundaries where useful;
- malformed-input tests;
- deterministic outputs;
- compatibility fixtures.

## 5. Hardware floor

AERIS must have a correct CPU-only reference path for core project editing, vector rendering, projection, and ordinary export.

GPU, SIMD, multithreading, or platform-specific acceleration may improve performance but must not silently become correctness requirements.

The project should avoid accidental hardware escalation caused by dependencies.

Heavy optional workloads, such as large satellite mosaics, may expose accelerated backends, but the application must clearly distinguish optional acceleration from minimum compatibility.

## 6. Bounded-memory processing

AERIS must not impose arbitrary output ceilings merely to simplify implementation.

Large raster exports should use tiled or striped rendering and streaming encoders so peak memory usage is bounded independently of final image dimensions where practical.

The same principle applies to large source datasets: stream, tile, index, or page data instead of assuming it all fits in memory.

## 7. Stable file semantics

Application version and file-format version are independent.

A new application release is not a reason to change the `.aeris` on-disk contract.

Once a stable project-format version is published:

- existing field semantics are immutable;
- existing stable files remain test fixtures forever;
- current AERIS releases must retain readers for all stable AERIS project formats;
- compatibility is tested, not assumed;
- a format-generation change is a last resort, not routine release management.

Implementation-native serializers are prohibited in the stable project format, including language object pickles, ABI-dependent struct dumps, framework object streams, or equivalent version-coupled encodings.

## 8. Determinism and reproducibility

Given identical canonical input data, source versions, project state, projection parameters, style definition, and export profile, AERIS should produce equivalent output within explicitly documented deterministic tolerances.

Where exact byte-for-byte reproducibility is not possible because of metadata timestamps, compression libraries, fonts, or platform rasterization, the nondeterministic fields must be identified rather than ignored.

## 9. Parsers treat input as hostile

Downloaded geography, imported project files, GeoJSON, GeoPackage, raster metadata, fonts, SVG assets, and all other external input are untrusted.

Parsers must enforce:

- bounded lengths and counts;
- numeric range checks;
- recursion/depth limits where applicable;
- geometry complexity limits or streaming paths;
- no unsafe object deserialization;
- no execution of embedded code;
- no path traversal during extraction/import;
- explicit handling of NaN/Infinity and invalid coordinates;
- fail-closed behavior for malformed required metadata.

## 10. Tests are part of the product

At minimum, the project should grow the following gates alongside implementation:

- unit tests;
- projection invariant tests;
- area-preservation tests;
- round-trip tests where inverse transforms exist;
- geometry/topology tests;
- parser fuzzing;
- corruption tests;
- power-loss / forced-process-termination tests;
- stable historical project fixtures;
- golden render tests;
- export validation;
- license/provenance validation.

A feature is not complete when it merely works on a normal input. It is complete when failure modes are understood and tested.

## 11. Power-loss contract

AERIS should be tested under repeated abrupt termination during random mutation sequences.

A representative destructive test should:

1. open a project;
2. perform a mutation;
3. terminate the process at a randomized point;
4. reopen the project;
5. verify database integrity;
6. verify project invariants;
7. verify the last acknowledged transaction is present and any unacknowledged transaction is absent;
8. restore the adjacent session state as far as durably committed;
9. repeat many times.

"It survived when tested manually" is not a durability guarantee.

## 12. Network independence

Opening and editing an already self-contained or fully cached project must not require network access.

Network access belongs to explicit ingestion/update operations and to resources that the project knowingly references externally.

A frozen/portable project must be reproducible offline.

## 13. No invisible downgrade

AERIS must never silently reduce:

- export resolution;
- color depth;
- vector fidelity;
- source precision;
- projection accuracy;
- font quality;
- image quality;
- provenance completeness.

If a requested mode cannot be produced, the user receives an explicit failure or explicit choice.

## 14. Code ownership and clarity

Core code should prefer explicit data flow, small interfaces, typed contracts, and isolated side effects.

Avoid hidden global state, monkey patching, runtime mutation of unrelated components, reflection-driven persistence, or framework magic that makes the project contract difficult to audit.

The goal is not minimum line count. The goal is minimum accidental complexity.
