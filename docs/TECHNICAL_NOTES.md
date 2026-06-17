# Technical Notes — Vulkan GPU-Driven Pipeline

A running log of design decisions, problems encountered, and the reasoning
behind each choice. Written to survive the gap between "I built this" and
"I can explain why I built it this way" — the gap that matters in an
interview.

---

## Why this document exists

Most of the boilerplate Vulkan code here was AI-assisted. That's fine —
what isn't fine is being unable to explain a design decision because it
was never really *made*, just accepted. This document is the record of
the decisions that *were* made: what problem each one solved, what
alternative was rejected, and what broke along the way.

---

## Architecture overview

```
Application
└── VulkanContext
    ├── initCore()              instance → device → swapchain → depth →
    │                            renderpass → pipeline → framebuffer
    ├── initSceneData()         OBJ load, vertex/index buffer, instance grid
    └── initCullingResources()  object buffer, visible-instance buffer,
                                 indirect draw buffer, compute descriptor/pipeline
        └── FrameRenderer
            └── FrameGraph (DAG)
                ├── GPUCullingPass   [Compute]
                └── GeometryPass     [Graphics, depends on CullingPass]
```

The split into `initCore` / `initSceneData` / `initCullingResources` was a
deliberate refactor once `init()` became a god function — see
[Refactor: splitting init()](#refactor-splitting-init) below.

---

## Decision log

### 1. FrameGraph as a DAG, not a fixed pipeline

**Decision:** Render passes are nodes in a dependency graph (`reads` list
per pass), resolved into an execution order via Kahn's algorithm at
`build()` time, not hardcoded as a sequence of function calls.

**Why:** This was inherited from prior experience building a CPU task
scheduler (`ConcurrentEngine_Light`) using the same DAG + topological sort
pattern. The insight carries over directly: a renderer's passes have real
dependencies (lighting needs geometry's depth buffer; post-process needs
lighting's output), and encoding those dependencies explicitly — rather
than relying on call order being correct by convention — makes invalid
orderings a `build()`-time error (`FrameGraph has cycle!`) instead of a
runtime bug discovered three weeks later.

**Direct payoff:** When GPU culling was added in Week 3, it slotted in as
a new node (`GPUCullingPass`) with `GeometryPass` declaring it as a
dependency. No changes to `FrameRenderer::drawFrame()` were needed beyond
the Compute/Graphics stage split (see #4). The graph absorbed a new,
structurally different kind of pass without architectural surgery.

**Interview-relevant:** *"Why DAG instead of a fixed pass list?"* — because
pass order is a correctness property, not a convention, and a topological
sort makes violations impossible rather than merely discouraged.

---

### 2. Staging buffer for all GPU-resident data

**Decision:** Every buffer that the GPU reads repeatedly (vertex, index,
instance) is uploaded via a `HOST_VISIBLE` staging buffer, then copied
once into a `DEVICE_LOCAL` buffer via `vkCmdCopyBuffer`, after which the
staging buffer is destroyed.

**Why:** `HOST_VISIBLE` memory is reachable by the CPU but lives in system
RAM from the GPU's perspective — every read during rendering pays a
bandwidth penalty. `DEVICE_LOCAL` memory lives in VRAM, an order of
magnitude faster to read. Since vertex/index/instance data is read once
per draw call per frame (potentially 60+ times/second), the one-time cost
of staging is recovered immediately. Uniform data (the MVP matrix) is the
deliberate exception — see #3.

**Pattern reused for:** `VertexBuffer`, `IndexBuffer`, `InstanceBuffer` —
all three are near-identical wrappers around `VulkanBuffer::create()` +
a one-shot `vkCmdCopyBuffer` transfer, differing only in the
`VkBufferUsageFlags` passed in (`VERTEX_BUFFER_BIT`, `INDEX_BUFFER_BIT`,
etc.).

**Interview-relevant:** *"Why not just use one HOST_VISIBLE buffer for
everything?"* — because the GPU would pay the slow-path memory cost on
every vertex fetch, every frame. One slow upload now buys many fast reads
later.

---

### 3. Uniform Buffer stays HOST_VISIBLE — no staging

**Decision:** Unlike vertex/index data, the MVP uniform buffer is created
directly as `HOST_VISIBLE | HOST_COHERENT` and written every frame with
`vkMapMemory` / `memcpy` / `vkUnmapMemory` — no staging buffer, no
`DEVICE_LOCAL` copy.

**Why:** The staging pattern only pays off when upload is rare and reads
are frequent. The MVP matrix is the opposite: it's rewritten *every
frame* (camera moves, model rotates), so staging would mean a
staging-buffer write + a `vkCmdCopyBuffer` + a GPU read, every single
frame — strictly worse than just writing `HOST_VISIBLE` memory directly
and letting the GPU read it once per frame regardless.

**Interview-relevant:** This is the answer to *"when would you skip the
staging pattern?"* — when write frequency approaches read frequency,
staging's amortization advantage disappears.

---

### 4. Compute and Graphics as separate FrameGraph stages

**Decision:** `RGPass` gained a `PassStage` field (`Compute` / `Graphics`).
`FrameGraph::execute()` was split into `executeCompute()` and
`executeGraphics()`. `FrameRenderer::drawFrame()` calls `executeCompute()`
*before* `vkCmdBeginRenderPass`, and `executeGraphics()` *inside* it.

**Why:** Compute dispatches are not bound to a `VkRenderPass` — they're a
distinct pipeline bind point (`VK_PIPELINE_BIND_POINT_COMPUTE`) with no
attachment concept. The original FrameGraph only had one `execute()`
called inside the render pass scope, which is structurally wrong for
compute work. Rather than special-case the culling pass outside the
graph entirely (which would have meant the DAG's dependency tracking no
longer covered it), the graph itself was extended to understand
heterogeneous pass types.

**What this bought:** `GeometryPass` can declare `{ cullingPass }` as a
read dependency, and the topological sort guarantees culling runs first
— a guarantee enforced by the scheduler, not by remembering to call
things in the right order in `drawFrame()`.

**Interview-relevant:** *"How do you keep a render graph from becoming a
graphics-only concept once you add compute work?"* — answered directly by
this change.

---

### 5. Explicit memory barrier between compute write and graphics read

**Decision:** A `VkMemoryBarrier` with `srcAccessMask = SHADER_WRITE_BIT`
and `dstAccessMask = VERTEX_ATTRIBUTE_READ_BIT | INDIRECT_COMMAND_READ_BIT`
sits between `executeCompute()` and `vkCmdBeginRenderPass`, with stage
masks `COMPUTE_SHADER_BIT → VERTEX_INPUT_BIT | DRAW_INDIRECT_BIT`.

**Why:** Without it, nothing guarantees the GPU finishes writing
`visibleInstanceBuffer` and `indirectDrawBuffer` before the graphics
pipeline starts reading them — Vulkan does not implicitly order
operations across pipeline stages just because they were recorded in
sequence in the command buffer. This is the textbook race condition the
Vulkan synchronization model exists to prevent.

**Interview-relevant:** *"What happens if you skip this barrier?"* —
undefined/stale data: the graphics pipeline might read a partially
written or last-frame's `instanceCount`, producing flickering or
incorrect culling that's nondeterministic and very hard to repro.

---

### 6. Sphere-Frustum test over AABB-Frustum

**Decision:** Culling uses bounding-sphere vs. 6-plane frustum
intersection (`dot(plane.xyz, center) + plane.w < -radius` → cull),
not axis-aligned bounding box (AABB) intersection.

**Why:** Sphere tests are cheaper (one dot product + compare per plane,
6 total) and rotation-invariant — an AABB has to either be recomputed
when the object rotates or be conservatively oversized to cover all
rotations, while a sphere's bounding volume doesn't change under
rotation. For Suzanne instances that spin in place, this sidesteps the
AABB recompute problem entirely. The tradeoff is a less tight bound for
non-spherical meshes (more false positives = objects that *aren't*
actually visible but pass the test) — acceptable here, would need
revisiting for elongated geometry.

**Frustum plane extraction:** Uses the Gribb-Hartmann method — the 6
planes are extracted directly from rows of the transposed `proj * view`
matrix (`m[3]±m[0]`, `m[3]±m[1]`, `m[3]±m[2]`), normalized by plane
normal length. This avoids manually deriving each plane's normal and
point from camera parameters.

**Interview-relevant:** *"Why sphere over AABB?"* — cost and
rotation-invariance; *"how do you get the frustum planes?"* — extracted
from the combined view-projection matrix, not recomputed from camera
vectors.

---

### 7. Compaction via atomic counter, not a fixed-size visibility array

**Decision (architecture pivot, Week 3):** The original culling design
wrote a per-object `visible[idx] = 1 or 0` into a flat array, leaving the
*decision* of what to draw still on the CPU (read the array back, decide
draw count). This was replaced with a compaction scheme: each compute
thread that passes the frustum test calls `atomicAdd(indirectDraw.
instanceCount, 1)` to claim a unique write slot, then writes its instance
data into `visibleInstanceBuffer[writeIndex]`. The CPU never reads
visibility data back — it only resets `instanceCount = 0` before dispatch
and calls `vkCmdDrawIndexedIndirect`.

**Why this is the actual point of "GPU-driven":** A 0/1 array still
requires *something* (the CPU, in the naive version) to decide how many
to draw and where they are. Compaction moves that decision fully onto the
GPU: the indirect draw buffer's `instanceCount` field is written by the
GPU, read by the GPU (during `vkCmdDrawIndexedIndirect`), with the CPU
never in the loop for "how many" or "which ones." This is the
architectural difference between *CPU-side frustum culling* (CPU computes
visibility, CPU issues a draw call with the resulting count) and
*GPU-driven rendering* (GPU computes visibility, GPU decides the draw
parameters, CPU just submits the indirect command).

**Synchronization subtlety:** `instanceCount` must be reset to 0 *every
frame* before dispatch — `atomicAdd` only increments, so a missing reset
means each frame's count compounds onto the previous frame's, eventually
overflowing or drawing far more instances than actually pass culling.

**Interview-relevant:** This is the single most likely deep-dive question
for this project: *"What makes this GPU-driven rather than just GPU-
accelerated culling?"* — the answer is the atomic compaction + indirect
draw combination, not the compute shader alone. A compute shader that
just flags visibility and hands the result back to the CPU for a normal
`vkCmdDrawIndexed` call is GPU-*assisted*, not GPU-*driven*.

---

### 8. Index buffer with vertex deduplication

**Decision:** `ObjLoader` deduplicates vertices via an
`unordered_map<Vertex, uint32_t>` keyed by a custom hash + equality
functor, rather than emitting tinyobjloader's per-face vertex stream
directly.

**Result:** 2904 face-vertex entries collapsed to 507 unique vertices for
Suzanne (~83% reduction) — expected for a closed mesh where most vertices
are shared across many adjacent faces.

**Known fragility:** The hash combines `position`/`normal` floats via XOR
(`hash(x) ^ hash(y) ^ hash(z)`), which is a textbook *bad* hash-combining
strategy — XOR is commutative, so distinct coordinate permutations can
collide. It happened not to cause an observed bug here (Suzanne's vertex
density didn't surface a collision), but this should be replaced with a
proper combining function (e.g. `boost::hash_combine`-style bit rotation)
before reusing this loader for denser meshes.

**Interview-relevant:** Worth volunteering proactively — *"I'm aware
the current hash-combine is a known anti-pattern; it didn't bite here but
I'd fix it before trusting this on denser meshes."* Identifying a latent
bug nobody asked about is a stronger signal than pretending the code is
flawless.

---

### 9. Instancing: per-vertex vs. per-instance vertex input rate

**Decision:** Two vertex input bindings: binding 0 (`Vertex` — position +
normal, `VK_VERTEX_INPUT_RATE_VERTEX`) and binding 1 (`InstanceData` —
world position, `VK_VERTEX_INPUT_RATE_INSTANCE`). The vertex shader reads
both and adds the instance offset to the per-vertex position after the
model matrix's rotation is applied.

**Why instancing over duplicating the mesh 343 times:** One copy of
Suzanne's vertex/index data is bound once; only a 343-entry
position array varies per draw. Memory cost is `507 vertices + 343
positions`, not `507 × 343 vertices`.

**Design consequence:** Once instancing was in place, the *visible*
instance buffer fed by GPU compaction (#7) reuses this exact same
mechanism — `visibleInstanceBuffer` is bound at binding 1 exactly like
the original static instance buffer was, just populated by the compute
shader instead of CPU-uploaded once at startup. Instancing and GPU-driven
culling share the same vertex-input plumbing; culling only changes *who
writes* the instance buffer and *how many* instances the indirect draw
claims.

---

### 10. Camera view matrix shared between culling and rendering

**Decision:** `Camera::getViewMatrix()` is called once per frame and used
identically by both the culling compute pass (to build the frustum) and
the geometry pass (to transform vertices). There is exactly one source of
truth for "where the camera is this frame."

**Why this matters more than it looks:** If culling and rendering ever
used *different* view matrices (e.g. culling computed from last frame's
camera, rendering from this frame's), the result would be visually wrong
in a way that's easy to not notice until the camera moves fast — objects
culled based on stale frustum data while the renderer draws based on
current data. This was a deliberate guard against a one-frame-of-lag bug
class, not an accident of code reuse.

---

### 11. GPU readback timing: record-time vs. execute-time

**Bug:** After wiring up the ImGui debug overlay to display
`instanceCount` from the indirect draw buffer, the displayed value was
stuck at `0` regardless of camera position or instance count. A direct
diagnostic — calling `vkQueueWaitIdle()` immediately after
`vkCmdDispatch()` inside the culling pass lambda, then reading the
buffer — also returned `0`, which initially looked like confirmation
that the compute shader wasn't culling anything.

**Root cause:** `vkCmdDispatch()` does not execute anything — it
*records* a dispatch instruction into the command buffer. The actual
GPU execution only happens after that command buffer is submitted via
`vkQueueSubmit()`. The diagnostic code was calling `vkQueueWaitIdle()`
*during command buffer recording*, before the buffer had been submitted
at all — there was nothing for the GPU to be idle *from* yet. The read
that followed wasn't picking up a "culling failed" result; it was
reading the buffer's reset value, because no compute work had run.

This meant every layer of the actual culling logic (frustum extraction,
sphere-plane test, atomic compaction, descriptor bindings) was correct
from the start — confirmed by re-deriving and re-checking each one
independently before finding the actual cause. The bug was never in the
GPU-side logic; it was in *when* the CPU was allowed to look at the
result.

**Fix:** Readback of `instanceCount` was moved out of the pass-recording
lambda entirely. It now happens once per frame, immediately after
`vkWaitForFences()` succeeds at the top of `drawFrame()` — the one point
in the frame loop where the *previous* frame's GPU work is guaranteed
complete. The value is cached on `VulkanContext` and the ImGui pass
reads the cached value rather than calling `getVisibleCount()` itself.

**Consequence accepted deliberately:** The displayed count is always
one frame behind the actual culling result for *that* frame (it shows
what was visible last frame, not this frame). This is invisible at
60fps and is the standard tradeoff for any GPU→CPU debug readback —
the alternative (synchronizing every frame to read the current frame's
exact result) would reintroduce a real GPU stall purely for a debug
number, which is a worse trade than a one-frame-old display value.

**Interview-relevant:** This is a strong example of distinguishing
*recording* a command buffer from *executing* it — a foundational Vulkan
concept that's easy to state abstractly but easy to violate in practice
the first time a "just read it back to check" instinct (reasonable in
higher-level APIs) gets applied to a command-buffer-based API. Worth
volunteering directly if asked "what was the hardest bug in this
project" — it's not flashy, but it demonstrates the difference between
debugging by checking assumptions one at a time (which is what actually
found it) versus debugging by re-reading code looking for a typo (which
wouldn't have, since there wasn't one).

---

### 12. Refactor: splitting `init()` {#refactor-splitting-init}

**Problem:** `VulkanContext::init()` grew into ~150 lines covering
Vulkan core object creation, mesh loading, instance grid generation, and
GPU culling resource setup — a god function that made any single change
require scanning the whole thing for the relevant 10 lines.

**Decision:** Split into `initCore()` / `initSceneData()` /
`initCullingResources()`, each independently responsible for one phase.
Two new private members (`meshIndexCount_`, `cachedInstances_`) carry
data across the split that used to be local variables shared implicitly
within one function body.

**Also found during the refactor:** `visibilityBuffer_` — a buffer
created early in the original GPU culling design (before the compaction
pivot in #7) — was still being allocated and destroyed but never
referenced by `ComputeDescriptor::create()`. Removed as dead weight.
This is the kind of orphaned resource that accumulates naturally when a
design changes mid-implementation and not every trace of the old design
gets cleaned up — worth scanning for after any architecture pivot, not
just at the end of a project.

**Interview-relevant:** *"How do you know when to refactor vs. keep
moving?"* — the trigger here was concrete: changes were taking longer
because finding the relevant lines in a god function took longer than
writing the change itself. The refactor was scoped narrowly (pure
function-body relocation, zero behavior change) specifically so it
wouldn't become its own multi-day side quest.

---

## Bugs encountered (and what they taught)

| Bug | Root cause | Lesson |
|---|---|---|
| Validation error: descriptor not in pipeline layout | `VkPipelineLayoutCreateInfo.setLayoutCount` left at 0 | Vulkan's validation layers catch binding mismatches at pipeline creation, not draw time — read the VUID message, it names the exact field |
| `vertexInputInfo` vs `vertexInput` typo | Copy-paste introduced a second, undeclared variable name | Vulkan's verbose struct-literal style makes this class of typo easy; the compiler caught it, validation layers wouldn't have |
| Suzanne rendered as a flat silhouette | `Vertex::position` was `vec2`, OBJ Z-coordinate silently discarded | Worth checking: does the data type even have room for the data you're loading? |
| Suzanne invisible after OBJ load | OBJ's vertex coordinates were in world space (X: -3.86..-1.13, Y: 0.27..2.24, Z: 3.25..4.96), nowhere near NDC `[-1,1]` | NDC and "the numbers in the file" are unrelated until a transform connects them — this is *why* MVP matrices exist, not an optional nicety |
| `code page 950` warnings, builds otherwise fine | Chinese-language comments in source files, MSVC's default codepage on a Traditional Chinese Windows locale | Non-ASCII comments are a portability landmine; switched to English comments project-wide |
| `'renderPass_': undeclared identifier` | Header declared the member as one name, `.cpp` used a different name for the same field | When a `.cpp`/`.h` pair is edited separately (e.g. by different AI-assisted passes), member names can drift — always grep for the field name across both files after a rename |
| Descriptor pool allocation silently insufficient | `VkDescriptorPoolCreateInfo.pPoolSizes` only listed `STORAGE_BUFFER`, but the layout also declared a `UNIFORM_BUFFER` binding | Pool sizes are a *budget declaration per descriptor type*, not implicitly inferred from the layout — every distinct `VkDescriptorType` in the layout needs its own `VkDescriptorPoolSize` entry |
| Compute culling result looked static at first (`visibility[0] = 1` forever) | Plausible but unverified — the shader could have been silently doing nothing | Forced an actual experiment: oscillate the test object's position with `sin(time)` and confirm `visibility[0]` flips `1 → 0 → 1` in sync. Passing a test that *could* fail is meaningfully different from a test that can only ever pass. |
| Monkey heads "exploded" into disconnected triangles after enabling instancing | Leftover `vkCmdDraw` call (non-indexed) not updated to `vkCmdDrawIndexed` when index buffer was introduced | Search-and-replace across a codebase is not the same as confirming every call site was actually touched |

---

## Open items / known simplifications

- **Vertex hash-combine (#8)** uses XOR, a known weak combining strategy.
  Works for Suzanne's vertex density; flagged for replacement before
  trusting on denser meshes.
- **Bounding sphere radius is a hardcoded constant** (`1.5f`) rather than
  computed from the actual mesh bounds. Fine for a single mesh type at
  uniform scale; would need to be computed per-mesh for a scene with
  varied geometry.
- **No LOD (level of detail) system yet.** Culling currently only answers
  "draw or don't" — the natural next step is a distance-based LOD
  selection, reusing the same compute-pass infrastructure (the object
  buffer already has per-instance position data; LOD just needs a
  distance-to-camera bucket and a per-LOD index buffer).
- **Single compute dispatch covers all 343 instances** with no
  multi-pass culling hierarchy (e.g. coarse cell-based culling before
  per-object testing). Acceptable at this instance count; would need
  revisiting at much higher instance counts where the linear scan itself
  becomes the bottleneck.
- **No texture sampling yet** — fragment shading is normal-based only.
  Planned but deprioritized relative to finishing the GPU-driven culling
  core.

---

## Glossary (for quick interview recall)

- **Staging buffer:** `HOST_VISIBLE` buffer used as a one-time CPU→GPU
  transfer relay; destroyed after copying into a `DEVICE_LOCAL` buffer.
- **SSBO (Shader Storage Buffer Object):** A buffer a compute (or
  fragment/vertex) shader can read *and write*, unlike a uniform buffer
  which is read-only from the shader's side.
- **Indirect draw:** A draw call whose parameters (instance count, index
  count, etc.) are read from a GPU buffer at execution time, rather than
  passed as immediate CPU-side arguments — the precondition for letting
  the GPU decide "how many to draw."
- **Compaction:** Using an atomic counter to pack only the elements that
  pass a filter into a contiguous output array, with each passing thread
  claiming a unique output slot via `atomicAdd`.
- **Kahn's algorithm:** A topological sort using indegree tracking and a
  queue of zero-indegree nodes — used here to resolve FrameGraph pass
  execution order from declared dependencies.
- **Gribb-Hartmann method:** Extracting the 6 frustum planes directly
  from the rows of a combined view-projection matrix, rather than
  deriving them geometrically from camera vectors.

  