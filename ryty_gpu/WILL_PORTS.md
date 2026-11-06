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

## Additional integrated groups

- `PERF-245`, `PERF-251`: serialized recorder-idle ownership and queue/decode
  yielding. Register-delta tests retain the target's coalesced-run contract.
- `PERF-247`, `FIX-267`: tessellation LDS writer/reader emulation and deferred
  writer barriers, with partial sample masks excluded from the optimization.
- `FIX-248`, `FIX-252`, `FIX-253`, `FIX-254`: FMask specialization, wave64 lowering,
  floating-point atomic classification, pinned vertex information, asymmetric
  empty-buffer matching and AMD translation behavior.
- `FIX-255`, `FIX-263`, `FIX-264`, `FIX-265`, `FIX-270`: tiny and sixteen-sample
  macrotiles, stable buffer rebinding, and region replacement. Reject failed
  replacements without discarding authoritative GPU bytes or claiming a clear
  that was never written.
- `PERF-250`, `PERF-256`: generation-qualified target and sampler reuse,
  canonical Vulkan samplers, and exact-value descriptor/push-constant command
  reuse. Worker snapshots still receive their full bindings.
- `PERF-259`: guarded compiler CPU placement, source worker priorities and
  recorder-core ownership. External CPU selections are respected; unsupported
  ownership combinations leave placement alone. The heavy driver-build limit
  remains in force.
- `FIX-268`, `FIX-271`, `FIX-277`: lifetime-guarded close-time cache saving and
  non-destructive compatible shader-store selection. Old stores, archives,
  pack stamps and incompatible retranslation marks are retained. An unreadable
  archive falls back to a writable directory store.
- `PERF-269`, `PERF-260`: bounded metadata readers, optional SPIR-V prefetch,
  and compute recipes warmed under the shared heavy-build gate. Recipes use
  explicit translation-policy compatibility, not executable, driver or SCM
  identity. Existing incompatible files are skipped and preserved.
- `FIX-272`, `PERF-262`, `FIX-275`, `PERF-274`: own queued callable values,
  record native command buffers on the scheduler ring, keep replay state on its
  owning thread, and start a list during serialized recorder idle. The ring is
  excluded when recorder workers, Intel direct recording, raw checkpoints or
  unsupported timing paths require direct command handles. Explicit
  `RYGT_NATIVE_RING=0` remains available in Release; periodic A/B diagnostics
  are Debug-only.
- `PERF-273`: early submission when the preceding tick is complete, bounded
  read-only storage mirrors, and display-awake requests on the present thread.
- `FIX-276`: reclaim GPU-completed upload allocations before an allocation
  retry; never wait for or free the current unsubmitted tick.

These groups include source defaults where their lifetime and API contracts can
be retained. GT7-only addresses, game patch offsets and branding require a GTS
adaptation; they are not evidence of equivalent behavior in GT Sport.

## Remaining work

The all-author inventory is not complete. Packed-specialization adaptation,
on-screen build pacing/catch-up, precise page writeback/fault-ahead ownership,
and the shader-object dependency chain remain under integration and review.
Graphics pipeline libraries and Vulkan shader objects are separate features.
Do not describe the existing library path as shader-object support.

One integrated Debug checkpoint has built. The latest ring/prewarm assembly
requires a fresh build and GPU validation. Twenty CPU cases covering upload
recovery, store selection/readers, callable ownership and exit-save lifetime
passed; the compute recipe and ring suites are additional checks. The real
wave64 lowering tool passed 1,536 cases. The emitted tessellation shader passed
SPIR-V validation and a standalone NVIDIA two-draw writer/reader fixture with
both separate and shared barriers. That fixture does not prove NativeRenderer
batch lifetime or game output. Full Release, latest Debug, import/symbol checks,
NVIDIA regression tests and two-race menu/Brand Central checks remain required.

No runtime speedup, stutter reduction or 16.6 ms target is established by these
component tests. Nothing in this port has been installed or merged.
