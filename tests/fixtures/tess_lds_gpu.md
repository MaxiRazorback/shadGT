# NVIDIA tessellation LDS fixture

Build `shadps4_gcn_test` and `shadgt_tess_lds_gpu` in a Debug configuration. Run
`shadps4_gcn_test --gtest_filter=TessLdsShader.*` with `RYGT_TEST_SPV_DIR` set to a
writable directory. This emits `tess_lds.spv` using the actual TCS backend. Validate it
with `spirv-val --target-env vulkan1.3`.

Compile `tess_lds_gpu.vert` and `tess_lds_gpu.tese` to SPIR-V 1.5 with
`glslangValidator -V --target-env vulkan1.2`. Run the standalone executable with the
vertex, emitted TCS and evaluation module paths, in that order. It selects an NVIDIA
Vulkan 1.2 device and uses its own instance, buffers, command pool and queue.

The test runs two three-patch draws with distinct push constants, output descriptors
and private buffer-device-address scratch. It compares two checked patch values from
each draw in both separate writer/barrier/reader execution and a shared-barrier run.
The last patch reads an allocated, initialized sentinel; only the first two are checked.
It does not launch or modify a game or validate every real guest shader.
