# AERIS — Storage Foundation

**Status:** DRAFT IMPLEMENTATION CHECKPOINT  
**Project format:** pre-1.0, no long-term compatibility promise yet

This document records the first implemented persistence foundation beneath the broader `AERIS-PROJECT-FORMAT.md` design. It describes what the current code proves and, equally importantly, what it does not yet prove.

## 1. Layer boundary

SQLite persistence is implemented by the optional native target:

```text
AERIS::storage
```

The cartographic/reference target:

```text
AERIS::core
```

has no transitive SQLite dependency. Qt is not involved in project-format semantics. A headless mathematical build can therefore remain free of both SQLite and the GUI toolkit.

## 2. Implemented draft containers

The first vertical slice implements:

```text
world.aeris
world.aeris.session
```

Both are SQLite 3 databases, but they have separate provisional application identifiers and separate schemas.

The canonical project currently stores only its foundation metadata:

- project UUID;
- draft format major/minor;
- monotonically increasing project revision;
- canonical UTC creation/modification timestamps;
- producer and producer version;
- projection identifier;
- worldview identifier;
- frozen marker.

The sidecar currently stores:

- the canonical project UUID it belongs to;
- its own revision and modification timestamp;
- optional view mode, camera longitude/latitude, and zoom.

The wider source/resource/feature/layer/style/extension schema remains a later layer. This checkpoint must not be mistaken for the stable AERIS Project Format 1.0 schema.

## 3. File acceptance precedes writable configuration

An existing database is treated as untrusted input.

AERIS opens the candidate and verifies, before applying writable durability configuration:

1. SQLite `quick_check`;
2. expected application identifier;
3. supported draft schema generation;
4. required singleton metadata and SQLite column types;
5. canonical UUID and metadata bounds;
6. canonical Gregorian UTC timestamps;
7. for a session sidecar, exact equality with the UUID of the already validated `ProjectStore`.

A project database presented to the session reader, or a session database presented to the project reader, is rejected before AERIS changes its journal configuration.

`SessionStore::open_or_create` accepts a validated `ProjectStore`, rather than an independently supplied path plus UUID. This prevents callers from manufacturing two disagreeing copies of project identity.

## 4. Acknowledged mutation transaction

The current reference writable connection uses:

```text
foreign_keys = ON
journal_mode = DELETE
synchronous = EXTRA
temp_store = MEMORY
```

An acknowledged project or session mutation executes inside a short `BEGIN IMMEDIATE` transaction. The in-memory revision/state is advanced only after SQLite reports a successful commit.

There is no periodic autosave window in this contract.

## 5. No-overwrite project publication

Creating a new `.aeris` does not open the final pathname with `CREATE` after a separate existence check.

Instead, AERIS:

1. creates a uniquely named sibling staging directory;
2. builds the complete SQLite project in that directory;
3. commits it and runs `quick_check`;
4. closes the staged database;
5. publishes it to the requested final pathname through a no-overwrite hard-link operation;
6. opens and validates the published pathname through the ordinary project reader;
7. removes the staging link/directory.

If the final pathname already exists, publication fails without replacing it. If the filesystem cannot provide the required hard-link publication primitive, the current implementation fails explicitly with `filesystem_failure`; it does not silently fall back to an overwrite-prone sequence.

## 6. Session publication and concurrent winner semantics

A new adjacent sidecar is constructed and validated in sibling staging before publication by the same no-overwrite principle.

If another creator wins the publication race first, the losing caller discards its staging database and opens the winner through the normal identity/UUID validation path. It never merges two sidecars and never overwrites the winner.

A stale sidecar belonging to a different project UUID is rejected and left untouched.

Deleting `.aeris.session` remains semantically harmless to `.aeris`.

## 7. Executed conformance tests

The dedicated Storage CI exercises the storage target independently on Ubuntu, Windows, and macOS, plus a Linux ASan+UBSan job.

The implemented tests cover:

- create, mutation, close, and reopen;
- revision persistence;
- application-ID rejection;
- impossible Gregorian date rejection, including leap-year boundary cases;
- adjacent session creation without invented view state;
- session view mutation and restoration;
- stale UUID-bound sidecar rejection without byte mutation;
- project/session cross-reader rejection without byte mutation;
- cleanup of successful and failed staging state;
- abrupt process exit immediately after acknowledged project and session commits;
- simultaneous publication attempts for one project pathname;
- simultaneous publication attempts for one session pathname.

The abrupt-exit test deliberately calls `std::_Exit(0)` after the commits. The verification process then reopens both files and requires the acknowledged revisions/state. This proves that the current contract does not depend on C++ destructors or `sqlite3_close()` during graceful shutdown.

## 8. Explicit non-claims

This checkpoint does **not** yet prove:

- survival of a physical machine power cut at every instruction boundary;
- parent-directory durability across every filesystem/storage-stack combination;
- correctness on filesystems that do not support the current hard-link publication primitive;
- stable pre-1.0 backward compatibility;
- full source/resource/feature/layer/style persistence;
- frozen/portable resource embedding;
- migration behavior between stable format generations;
- read-only project operation;
- corruption recovery beyond fail-closed validation;
- hostile-input resource/decompression limits for schema layers that do not exist yet;
- viewer Create/Open/Recent-project UX.

Those are separate contracts and must be tested before the corresponding capability is claimed.

## 9. Next storage layer

Once this foundation is merged, the next persistence work should build on it rather than widen this PR indefinitely:

```text
verified source snapshot/provenance
        ↓
aeris_source + aeris_resource
        ↓
canonical feature/layer data
        ↓
style / extension / export-profile state
        ↓
viewer project lifecycle
```

The canonical geographic model remains upstream of projection/render caches. Storage must not turn derived planar or globe-view geometry into geographic truth.
