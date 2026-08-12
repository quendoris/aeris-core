# Natural Earth `ne_110m_land` compatibility pin

This directory pins the exact byte identity used by the AERIS real-world integration proof.

- Natural Earth release: `v5.1.2`
- Upstream tag commit: `f1890d9f152c896d250a77557a5751a93d494776`
- Dataset-internal `VERSION.txt`: `4.1.0`
- Adapter contract: `natural-earth.ne-110m-land.shapefile.v1`

`resources.tsv` contains the expected SHA-256 and exact byte size of every resource required by the adapter.
`content.sha256` is the deterministic AERIS aggregate snapshot identity computed from those verified resources.

The ordinary CI suite does not download these files. Network acquisition belongs to the separate Source Compatibility workflow.
