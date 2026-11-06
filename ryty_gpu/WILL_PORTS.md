# Will Scarlett source port (in progress)

Target base: `ryty-gpu` at `20b1965a00de9f1b5fdca9b3f861c7a7f6faadee`.
Upstream snapshot: `rygt-team/shadgt7` main at
`3b27dbfee4cd63eaf173826c5447c8cd715a183b`.

The full inventory covers all 186 commits authored by
WillScarlettOhara (`leilumultipass@gmail.com`) reachable from that snapshot,
including 77 commit messages mentioning PERF. Dependencies and later fixes are
reviewed together. This is a draft port, not a claim of runtime validation or a
completed port of every inventory entry.

## Native resource group

| Source | Ported behavior |
| --- | --- |
| `32288f94`, `4bbac02c` | Optional in-place page statistics; address-indexed pending region writes. Existing imported-page coverage supersedes the small range-import memo. |
| `a6dac7da`, `5ad550f4` | Keep recently used, CPU-loaded idle textures within the 2 GiB LRU budget; exclude GPU/foreign writes, target-backed textures and crowded addresses. |
| `25e4c8b4` | Import allocation-relative 256 MiB host-memory pieces on use, up to two pieces for a crossing request. Preserve target coverage, negative-cache generation and deferred cleanup. |
| `c555339f`, `615f732e` | Revalidate disjoint buffer/texture memo ranges through change logs; unmaps and hard invalidation still reject them before dereferencing old backing. |
| `1fce9209`, `a5b40722`, `23282819`, `0f86e14f` | Write small pictures back once per frame; flush overlapping pending pictures before later reads/writes. Preserve immediate GTS photo/thumbnail writebacks above the upstream 64 KiB limit. |
| `f97bc35f` | Batch image downloads, tile in device-local scratch and copy to guest memory. Keep target's authoritative image-to-region copy ordering and reusable standalone scratch. |
| `ed6d5c06`, `42003b21` | Coalesce proven independent compute runs; same-texel mip/layer keys retain unknown-write, descriptor-alias and prior-read safeguards. A functional recording epoch replaces diagnostic counters for Release dependency checks. |

Will's switches/defaults are retained: `RYGT_D1_VIEW_MB=256`,
`RYGT_NATIVE_KEEP_TEXTURES_MB=2048`, buffer/texture revalidation enabled,
`RYGT_NATIVE_WRITEBACK_EACH` unset, writeback batching enabled and same-texel
optimization enabled. Program-specific dispatch shortcuts require the exact
upstream program identity and proofs; other dispatches keep their barriers.

For the explicitly requested GDS experiment,
`RYGT_SPLIT_GDS_WRITE_FAULTS=1` enables the existing PERF-015 path at startup.
Unset or `0` keeps it disabled. This trial does not change the separate PERF-014
drain/readback safeguards. Previous vertex/sparks corruption remains a required
runtime comparison, not a resolved defect.

## Validation so far

- Real clang-cl front-end checks of the isolated native renderer, image registry
  and direct-memory imports, using the worktree's headers before dependency paths.
- All nine upstream same-texel GoogleTest cases pass.
- Added GPU regressions for two batched destinations, nonzero offsets, guard bytes,
  linear/tiled layouts and mip roundtrips; execution is pending the integrated build.
- Full Debug/Release builds, isolated GPU checks, gameplay and transition checks
  are still required. No installed game or shader cache has changed.
