# AERIS Attribution Policy

**Status:** Project policy

AERIS separates legal attribution from historical acknowledgement and from gratitude.

The purpose of this policy is to keep credit factual, proportional, and resistant to marketing visibility. A modern website, successful company, or popular product does not become the origin of an idea merely because it is easier to discover than the older work behind it.

## 1. Evidence first

Every non-trivial historical attribution should be supported by a source that is reasonably close to the underlying work or by reputable technical literature.

When authorship, date, or priority is uncertain, AERIS must say so. Uncertainty is preferable to a confident but unsupported origin story.

AERIS must not:

- infer the inventor of a technique from the company currently selling a product that uses it;
- transfer credit from an individual researcher to a later vendor for convenience;
- describe a commercial reference as an intellectual origin without evidence;
- manufacture gratitude to satisfy branding expectations.

## 2. Attribution classes

### Class H — Historical or intellectual contribution

Use when a person or group created, discovered, formalized, or materially advanced an idea on which AERIS relies.

Treatment:

- visible acknowledgement;
- name the actual contribution;
- preserve historical context where useful;
- cite the best available source;
- distinguish the original contribution from later implementations.

This is the highest acknowledgement class.

### Class O — Intentionally open contribution

Use for research, datasets, software, specifications, or other work deliberately made available for broad reuse without requiring a commercial transaction.

Treatment:

- satisfy all license requirements;
- acknowledge substantial enabling work beyond the legal minimum when appropriate;
- identify the specific contribution rather than offering generic praise.

Open availability alone does not automatically make a dependency historically important. The acknowledgement should remain proportional to actual contribution.

### Class C — Commercial dependency or reference

Use when AERIS uses, interoperates with, compares against, or was led to an idea through a commercial product or service that was created and licensed as a commercial offering.

Treatment:

- satisfy the exact legal attribution requirements;
- record factual provenance where relevant;
- avoid implying endorsement, partnership, or intellectual priority;
- do not add ceremonial gratitude merely because the product is polished, prominent, or commercially successful.

Commercial success is not treated negatively. A company that created a good product and successfully sold it completed the exchange it intended. That fact alone does not create an additional historical debt.

## 3. Failed or unfinished research can deserve major credit

Commercial success is not a criterion for historical importance.

A research effort that failed to produce a successful product may still deserve strong acknowledgement if it explored a genuinely original path, rejected established assumptions, published useful results, or made later work possible.

AERIS evaluates contribution by what was attempted, discovered, demonstrated, or released — not by market outcome.

## 4. Legal attribution is mandatory and separate

If a license requires a notice, credit line, link, share-alike condition, or other attribution, AERIS must comply regardless of the acknowledgement class above.

Mandatory notices belong in the appropriate legal/provenance path even when the dependency receives no special entry in `ACKNOWLEDGEMENTS.md`.

Conversely, AERIS may acknowledge an historical contributor whose work is old, public-domain, or otherwise requires no legal attribution.

## 5. Provenance in generated projects and exports

Where a source contributes data or assets to a project, AERIS should retain enough provenance to answer:

- What source was used?
- Which version or snapshot was used?
- When was it retrieved?
- What license governed it?
- What exact content was used, where hashing is practical?
- Is an attribution required in generated output?

The renderer and exporters should derive required notices from provenance rather than from hard-coded marketing text.

## 6. Current cartographic lineage

The current primary historical acknowledgement for the Sinu-Mollweide direction is **Allen K. Philbrick**, to whom cartographic literature attributes the 1953 Sinu-Mollweide construction.

**J. Paul Goode** and **Karl Brandan Mollweide** are recorded in the projection lineage for their foundational contributions.

The Future Mapping Company is recorded separately as the modern commercial visual reference through which this project encountered the projection family.

See [`../ACKNOWLEDGEMENTS.md`](../ACKNOWLEDGEMENTS.md).
