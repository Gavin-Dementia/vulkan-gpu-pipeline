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
    ├── initSceneData()         3× OBJ load (LOD0/1/2), vertex/index
    │                            buffer per LOD, 7×7×7 instance grid
    └── initCullingResources()  shared object buffer, 3× {visible-
                                 instance buffer, indirect draw buffer}
                                 (one pair per LOD — see §15), frustum
                                 uniform buffer, compute descriptor/pipeline
        └── FrameRenderer
            └── FrameGraph (DAG)
                ├── GPUCullingPass   [Compute] — frustum test + LOD fan-out
                ├── ShadowPass       [Shadow, own render pass] — depth-only,
                │                     draws all instances from the light's
                │                     view (see §22)
                ├── GeometryPass     [Graphics, depends on CullingPass +
                │                     ShadowPass] — 1 indexed-indirect draw
                │                     per LOD, samples the shadow map;
                │                     renders to the offscreen scene target,
                │                     not the swapchain directly (see §24)
                └── ImGuiPass        [UI, own render pass = the swapchain's]
                                      — dockable Viewport (samples the scene
                                      target) + debug windows (see §24)
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

**Known fragility (fixed):** The hash originally combined `position`/
`normal`/`uv` floats via plain XOR (`hash(x) ^ hash(y) ^ hash(z)`), a
textbook *bad* hash-combining strategy — XOR is commutative, so distinct
coordinate permutations could collide. It never caused an observed bug
here (Suzanne's vertex density didn't surface a collision — and
correctness was never actually at risk, since `unordered_map` always
falls back to the `VertexEqual` functor to resolve any hash collision;
a worse hash only means more collisions to resolve, not incorrect
merging). Replaced with a `boost::hash_combine`-style mix (golden-ratio
constant + bit shifts, order-dependent) so it degrades gracefully on
denser meshes instead of relying on this mesh's vertex layout happening
not to trigger a collision.

**Interview-relevant:** Worth volunteering proactively even before it's
fixed — *"I'm aware the current hash-combine is a known anti-pattern;
it didn't bite here but I'd fix it before trusting this on denser
meshes."* Identifying a latent bug nobody asked about is a stronger
signal than pretending the code is flawless — and precisely stating
*why* it was never actually unsafe (hash quality is a performance
concern for `unordered_map`, not a correctness one) is stronger still
than just calling it "a known anti-pattern."

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
 
### 13. Texture pipeline: Image/ImageView/Sampler as a parallel resource model to Buffer
 
**Decision:** Textures are implemented as a new `VulkanTexture` class
wrapping `VkImage` + `VkDeviceMemory` + `VkImageView` + `VkSampler`,
following the same staging-buffer-then-DEVICE_LOCAL-copy pattern used
for vertex/index/instance buffers (#2), but with an additional step
neither of those needed: explicit layout transitions.
 
**Why images need layout transitions and buffers don't:** A `VkImage`
has an associated layout describing what the GPU currently expects to
do with that memory (`UNDEFINED`, `TRANSFER_DST_OPTIMAL`,
`SHADER_READ_ONLY_OPTIMAL`, etc.) — this is a real hardware concept,
since GPUs often store image data in implementation-specific tiled/
compressed-for-access-pattern formats that differ between "being
written to" and "being sampled from." A buffer has no equivalent
concept; it's just linear memory. The texture upload path is therefore:
upload pixels to a staging buffer → create the image in `UNDEFINED`
layout → transition to `TRANSFER_DST_OPTIMAL` → `vkCmdCopyBufferToImage`
→ transition again to `SHADER_READ_ONLY_OPTIMAL` before the fragment
shader is allowed to sample it. Each transition is itself a
`VkImageMemoryBarrier` recorded into a one-shot command buffer — the
same submit/wait/free pattern already established for buffer-to-buffer
copies, just with an image-specific barrier instead of a
`vkCmdCopyBuffer`.
 
**Format choice:** `VK_FORMAT_R8G8B8A8_SRGB` — sRGB because texture
source images (PNGs of real photos/art) are typically authored in sRGB
color space, and sampling them as sRGB tells the GPU to convert to
linear space automatically before the fragment shader sees the value,
which avoids the (very common beginner) "everything looks washed out
or too dark" bug from doing lighting math on raw gamma-encoded values.
 
---
 
### 14. ImGui's descriptor pool requirements aren't the same as the app's
 
**Decision:** `ImGuiLayer` allocates and owns its own `VkDescriptorPool`,
entirely separate from the pools used by `VulkanDescriptor` (graphics)
and `ComputeDescriptor` (compute).
 
**Why:** ImGui's Vulkan backend manages descriptor sets internally for
font textures and any images it renders (e.g. font atlas, future custom
texture widgets), and it expects to own a pool sized for its own
descriptor type needs — not to share the application's pool, which has
no reason to know what ImGui needs internally.
 
**Bug found here:** The pool was initially sized with only
`VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER`, which produced (non-fatal,
but real) validation warnings — `vkAllocateDescriptorSets(): ... binding
0 was created with VK_DESCRIPTOR_TYPE_SAMPLER but VkDescriptorPool ...
was not created with any VkDescriptorPoolSize::type with
VK_DESCRIPTOR_TYPE_SAMPLER`. ImGui's internal layouts use `SAMPLER` and
`SAMPLED_IMAGE` as separate descriptor types in addition to (or instead
of, in certain code paths) the combined type. Fixed by declaring three
separate `VkDescriptorPoolSize` entries (`SAMPLER`, `SAMPLED_IMAGE`,
`COMBINED_IMAGE_SAMPLER`) rather than assuming one combined-type entry
would cover everything ImGui's backend might request.
 
**Pattern this reinforces:** Same lesson as #6 in the bugs table below —
a descriptor pool's size list is a literal enumeration of what it can
hand out, not something the pool infers from a single "this is roughly
what I need" entry. This is the second time in the project this exact
mistake surfaced (first in `ComputeDescriptor`, now in `ImGuiLayer`),
which says more about how easy the mistake is to make than about either
specific instance — worth treating "did I declare a `VkDescriptorPoolSize`
for *every* `VkDescriptorType` the corresponding layout uses" as a
standing checklist item whenever wiring up a new descriptor set, not
just something to debug into existence after a warning appears.
 
---

### 15. LOD pivot: from a single culling output to 3 parallel per-LOD compaction sets

**Decision:** The culling pipeline described in §7 originally had exactly
one `(VisibleInstanceBuffer, IndirectDrawBuffer)` pair, fed by one mesh.
This was extended to 3 parallel pairs — `VisibleLOD0/1/2` +
`IndirectLOD0/1/2` — with `culling.comp` bucketing each passing instance
into one of the three based on camera distance (`LOD1_DIST = 12.0`,
`LOD2_DIST = 20.0`, hardcoded in-shader at the time - made runtime-tunable
in §26) before doing the same
`atomicAdd`-based compaction into that bucket's own buffer. `ObjectBuffer`
itself stayed a single shared 343-entry array — LOD is a per-frame
*selection* over one set of instances, not a duplicated object per LOD.

**Why 3 full parallel sets instead of 1 buffer with a per-instance LOD
tag:** A single `VisibleInstanceBuffer` can only feed one
`vkCmdDrawIndexedIndirect` call, which binds exactly one vertex/index
buffer pair — but each LOD is a *different mesh* (different vertex/index
buffer, e.g. Suzanne: 507 vertices at LOD0, 165 at LOD1, 34 at LOD2).
There's no way to draw "some instances with mesh A, others with mesh B"
in a single indirect draw call, so 3 draw calls — one per LOD mesh —
were unavoidable, and each needs its own instance-count/instance-data
pair to know how many and which instances to draw with that mesh.

**Structural consequences:**
- `ComputeDescriptor` grew from 4 bindings (object / visible / indirect /
  frustum) to 8 (object / 3×visible / 3×indirect / frustum) —
  `ComputeDescriptor::create()`'s signature took `std::array<VkBuffer,3>`
  for both the visible-instance and indirect-draw buffers.
- `VulkanContext::LODMesh` groups `{VertexBuffer, IndexBuffer,
  VisibleInstanceBuffer, IndirectDrawBuffer}` per LOD, replacing the
  single top-level members those used to be.
- `FrameRenderer::drawFrame()`'s per-frame reset (§7's "must reset every
  frame" rule) now loops 3 times, and `GeometryPass` issues 3
  `vkCmdDrawIndexedIndirect` calls instead of 1 — one full bind+draw per
  LOD, every frame, regardless of how many instances actually land in
  that LOD's bucket that frame (an empty bucket still costs a draw call
  with `instanceCount = 0`).

**Known simplification carried over from this pivot:** The LOD distance
thresholds are constants baked into `culling.comp`, not derived from
each mesh's actual detail level, screen-space size, or exposed as a
tunable.

**Interview-relevant:** *"Why not just add an `lod` field to the
instance and branch in the vertex shader?"* — because indirect draw
parameters (which vertex/index buffer, how many instances) are decided
at `vkCmdDrawIndexedIndirect` record/dispatch time, not per-vertex in the
shader; the mesh itself differs per LOD, so the branch has to happen
before the draw call is issued, not inside one.

---

### 16. Mesh recentering + bounding sphere computed from actual vertex bounds

**Bug found:** `FrameRenderer.cpp` had a dead constant, `SUZANNE_OFFSET
= {2.49f, -1.25f, -4.10f}`, with a comment explaining it was Suzanne's
OBJ-space center offset — but it was never applied anywhere. Measuring
`assets/suzanne.obj`'s actual vertex bounds confirmed the comment: the
raw OBJ data is centered around `(-2.49, 1.25, 4.10)`, not the origin.
Since `GeometryPass`'s model matrix is a pure rotation about the
instance's local Y axis with no compensating translation, rotating an
off-center mesh doesn't spin it in place — it swings the mesh through a
horizontal circle of radius `sqrt(2.49² + 4.10²) ≈ 4.8` units around its
grid slot. With the instance grid spaced 3.0 units apart, that orbit
would sweep well into neighboring cells, not stay contained in the
instance's own slot. The intended fix (the offset constant) existed in
the code but was never wired into the model matrix or the mesh data.

**Fix:** Rather than patching the model matrix with the offset,
`ObjLoader::load()` now recenters every mesh at load time: after
building the deduplicated vertex list, it computes the mesh's own
bounding-box center and subtracts it from every vertex position. This
makes local `(0,0,0)` coincide with each LOD mesh's own geometric
center, so per-instance rotation is a true in-place spin regardless of
where the source OBJ's vertex data happened to be authored. All 3 LOD
variants recenter independently using their own bounds; their measured
bbox centers already agree to within ~0.12 units of each other on every
axis (expected, since they're decimated versions of the same shape), so
LOD switching doesn't introduce a visible position jump. The
now-superseded `SUZANNE_OFFSET` constant was deleted.

**Bounding sphere radius, computed instead of guessed:** `ObjLoader`
also now returns `MeshData::boundingRadius` — the max distance from the
new local origin to any vertex, i.e. the tightest origin-centered sphere
containing the whole mesh. `VulkanContext::initSceneData()` captures
LOD0's radius (the most detailed variant; LOD1/2 are decimated versions
of the same shape and are never larger) into `boundingSphereRadius_`,
which replaces the previous hardcoded `1.5f` in
`initCullingResources()`. This is a correctness fix, not just a
cleanliness one: the recentering changed the mesh's actual extents
relative to local origin, so a stale hardcoded radius could now
under- or over-approximate the true bounds depending on how the guess
compared to the recentered geometry.

**Interview-relevant:** This is a good example of a dead-code trail
pointing straight at a real bug — the unused `SUZANNE_OFFSET` constant
was the tell that someone had already diagnosed this exact problem and
started a fix that was never finished or was superseded by a different
approach, then never cleaned up. Grepping for a suspicious constant's
usage before assuming it's just leftover cruft is worth doing before
deleting it as "unused."

---

### 17. Mouse-fired projectile needs its own UBO and descriptor set

**Decision:** The first interactive feature — a single object the player
aims (via the existing keyboard-controlled camera) and fires with a
left-click, flying in a straight line at constant speed — reuses LOD2's
existing vertex/index buffers (no new asset), but gets its **own**
`UniformBuffer` and `VulkanDescriptor`, separate from the grid's.

**Why a second UBO is required, not just cleaner:** `VulkanBuffer::upload()`
(and `UniformBuffer::update()`, which calls it) writes into persistently
mapped host memory (§ "persistently map HOST_VISIBLE buffers" commit) at
command-*recording* time — but the GPU reads that memory at command-
*execution* time, after the command buffer is submitted. If the grid's
draws and the projectile's draw shared one UBO, `GeometryPass`'s lambda
would call `update()` twice per frame (once per model matrix) while
recording draw calls that reference the *same* buffer — by the time the
GPU actually executes those draws, the buffer holds whichever value was
written *last* in recording order, not whichever value was current when
each draw call was recorded. Both draws would silently end up using the
same (wrong, for one of them) model matrix. There is no per-draw
snapshot of a single mutable UBO — this is the same record-vs-execute
distinction as §11's `vkCmdDispatch` bug, applied to descriptor data
instead of a readback.

**Why this is safe to do as a second, independently-created
`VulkanDescriptor`:** Verified directly against `VulkanDescriptor::create()`
— each instance builds its own `VkDescriptorPool`/`VkDescriptorSetLayout`/
`VkDescriptorSet` from scratch, so a second instance doesn't collide with
or share state with the grid's. Vulkan's pipeline-layout-compatibility
rule (used when binding a descriptor set against `pipeline_.getLayout()`)
only requires the two layouts have *identical binding structure* (same
binding indices, descriptor types, counts, stage flags) — not the same
`VkDescriptorSetLayout` handle — so a structurally-identical second
descriptor set is valid to bind against the one shared pipeline.

**Reused, not duplicated:** The projectile's world position is carried
entirely through a new 1-entry instance buffer (binding 1), reusing the
exact same model-is-rotation + instance-is-translation split the vertex
shader already does for the grid (`worldPos = (ubo.model * position).xyz
+ inInstancePos.xyz`) — its model matrix is just `mat4(1.0f)` (no spin).
Mouse input is polled (`glfwGetMouseButton`) from `Application::mainLoop`
rather than a registered GLFW callback, because ImGui already installs
its own mouse callbacks (`install_callbacks=true` in `ImGuiLayer::init`)
— a second registered callback would replace ImGui's rather than compose
with it. Clicks are also gated on `!ImGui::GetIO().WantCaptureMouse` so
clicking the debug overlay doesn't also fire a shot.

**Deliberately out of scope for this milestone:** collision detection
against the 343-instance grid and any "scatter" reaction. The projectile
just flies in a straight line and expires after a fixed lifetime
(`Projectile::kMaxLifetime`). `Projectile::position()` is the one value
a future milestone needs to read each frame to test against the grid.

**Interview-relevant:** *"Why does a single extra object need a whole
second UBO and descriptor set instead of just calling `update()` twice
before each draw?"* — because both `update()` calls happen at record
time, but both draws read the buffer at execute time; recording order
has no effect on which model matrix "belongs" to which draw once they're
both reading from the same memory location.

---

### 18. Camera look control: QE + arrow keys replaced with mouse-look

**Decision:** `Camera::processInput()` originally used `Q`/`E` for
vertical movement and the 4 arrow keys for yaw/pitch, in addition to
`WASD` for horizontal movement — 6 keys total for look + vertical
movement. These were removed and replaced with mouse-look: yaw/pitch are
now driven by `glfwGetCursorPos()` deltas, sampled once per frame inside
`processInput()` itself (no new public method or call-site change
needed in `Application::mainLoop()`). `WASD` horizontal movement is
unchanged; vertical movement (`Q`/`E`) was removed outright rather than
rebound, since nothing else in the scene requires flying up/down and the
grid/projectile are both reachable by moving forward/back/strafe alone.

**Why this needed `GLFW_CURSOR_DISABLED`, not just reading cursor
position:** `glfwGetCursorPos()` in the default `GLFW_CURSOR_NORMAL`
mode reports a position clamped to the window's client area — the mouse
hits the screen edge and stops registering further movement in that
direction, which breaks a continuous look-around control. Setting
`glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED)` once in
`Application::init()` hides the cursor and makes GLFW report an
unbounded virtual position instead, which is what every FPS-style
mouse-look implementation relies on.

**Why this doesn't fight ImGui:** Checked directly in the vendored
`third_party/imgui/backends/imgui_impl_glfw.cpp` —
`ImGui_ImplGlfw_UpdateMouseCursor()` explicitly early-returns when
`glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED`, so
ImGui's backend never tries to re-enable the cursor out from under the
camera. Mouse *buttons* are unaffected by cursor visibility mode either
way, so the projectile's left-click fire trigger (§17) keeps working
unchanged.

**First-frame jump, avoided:** `lastMouseX_`/`lastMouseY_` start
uninitialized relative to wherever the OS cursor happened to be when the
window gained focus; without a guard, the very first frame's delta would
be a large (effectively random) jump. A `firstMouseSample_` flag makes
the first call just record the current position with no yaw/pitch
change, matching the same "suppress bogus first delta" pattern
`Application::mainLoop()` already uses for click edge-detection (§17).

**Interview-relevant:** *"Why not just clamp cursor position instead of
disabling it?"* — clamping still hits the window edge and stops
registering motion past it; disabling the cursor gives GLFW's unbounded
virtual-position mode, which is the actual mechanism (not a workaround)
every mouse-look camera depends on.

**Addendum — Ctrl toggles the cursor back on for UI adjustment:** once
the ImGui "GPU Culling Stats" and "Lighting" windows had actual controls
worth dragging/resizing (§19's sliders), `GLFW_CURSOR_DISABLED` became a
problem: GLFW's unbounded virtual position in that mode doesn't
correspond to real screen coordinates, so ImGui can't hit-test against
it — the debug windows were effectively unclickable while mouse-look was
active. Fix: `Camera::processInput()` now polls Ctrl each frame and
toggles `GLFW_CURSOR_NORMAL`/`GLFW_CURSOR_DISABLED` on the actual
transition (not every frame), and returns early — skipping yaw/pitch
entirely — while the cursor is shown, so moving the mouse to click a
slider doesn't also spin the camera. The same `firstMouseSample_` guard
already in place (see above) is re-armed on the transition back to
`DISABLED`, so releasing Ctrl doesn't reintroduce the first-frame-jump
problem this section was originally about.

**Also added: Esc to quit** (`Application::mainLoop()`, alongside the
existing `glfwWindowShouldClose` check) — unrelated to the cursor
mechanism above, just a normal quality-of-life addition once the window
had no title-bar close affordance reason to reach for during testing.

---

### 19. PBR milestone 1: Cook-Torrance lighting, no new textures

**Decision:** Replaced the fully unlit fragment shader (texture sample ×
a flat `normalize(normal)*0.3+0.7` tint) with a real Cook-Torrance BRDF
(GGX distribution, Smith geometry, Fresnel-Schlick), driven by one
directional light. Material parameters (`albedo`, `metallic`,
`roughness`) are a **push constant**, not a texture or a new descriptor
binding — texture-based materials are an explicit later milestone. This
is the first step toward the project's stated PBR direction, scoped
narrowly on purpose: verify the lighting math is correct before spending
any effort on material/texture infrastructure.

**Why `SceneData` (light + camera position) is a UBO on a shared
binding, not a push constant:** unlike material params, which differ
per draw call, light and camera data are identical for every draw in a
given frame. `VulkanDescriptor::create()` is already called twice
(`descriptor_` for the grid, `projectileDescriptor_` for the projectile)
with an identical binding layout — adding binding 2 (`UNIFORM_BUFFER`,
fragment stage) to that one shared function means every material gets
scene data through the same mechanism automatically, with no new
pipeline-layout set index and no extra `vkCmdBindDescriptorSets` call.
One buffer (`VulkanContext::sceneDataBuffer_`), updated once per frame,
referenced by both descriptor sets. Note this had to be a raw
`VulkanBuffer`, not the `UniformBuffer` class — that class is hardcoded
to `UBOData`'s type and size, not reusable for a different struct.

**Why material params are a push constant, not a 4th descriptor
binding:** the whole point of this milestone is proving the grid and the
projectile can look visually different (rough dielectric vs. shiny
metal) without any texture work. A push constant is the cheapest
mechanism for "value that changes every draw call" — `vkCmdPushConstants`
right before each draw, no descriptor-set churn, and at 32 bytes it's
nowhere near the guaranteed-minimum 128-byte Vulkan push constant
budget. The tradeoff: because the grid and the projectile share one
pipeline layout and record into the same command buffer, the push
constant must be **re-issued before each draw** — it's pipeline state
that persists until overwritten, not per-draw-call scoped the way a
descriptor set binding is.

**The sRGB-swapchain double-gamma trap:** `VulkanSwapchain.cpp`'s
`chooseFormat()` picks `VK_FORMAT_B8G8R8A8_SRGB` — confirmed by reading
the code, not assumed. This means the GPU automatically linear→sRGB
encodes on write to the swapchain image. A shader that *also* applies
`pow(color, 1.0/2.2)` at the end would double-gamma-correct and produce
a washed-out, too-bright image — a classic and easy-to-miss PBR bug.
The fragment shader does a Reinhard tonemap (`color/(color+1)`, a
different step from gamma encoding) and stops there, leaving the actual
gamma encoding to the hardware. Same category of format-awareness as
the texture format decision in §13 (`R8G8B8A8_SRGB` chosen so *sampling*
auto-converts to linear) — this is the *output* side of the same
concern.

**A previously-invisible bug, surfaced by adding real lighting:**
`triangle.vert` output `fragNormal = inNormal;` — the object-**local**-
space normal, never transformed by `ubo.model`'s rotation. Under the old
flat unlit tint this was never visibly wrong (the tint didn't depend on
a real light direction). Once lighting depends on `N·L`/`N·V`, the
grid's existing per-frame spin would have made the lit appearance visibly
swim independently of the mesh instead of rotating rigidly with it.
Fixed with `mat3(ubo.model) * inNormal` — sufficient (rather than a full
inverse-transpose normal matrix) specifically because `ubo.model` is
always a pure rotation with no scale in this codebase; that assumption
would need revisiting if non-uniform scale is ever introduced per-object.

**Verification approach:** added ImGui sliders (`Direction`/`Color`/
`Intensity` under a new "Lighting" window) that mutate
`VulkanContext`'s light state directly, so the lighting result is
interactively checkable in real time — dragging the light direction and
watching the lit side of the grid change immediately — rather than a
one-time eyeball check. Same "verify experimentally" instinct as the
`sin(time)` oscillation test for culling in §7.

**Interview-relevant:** *"Why does the projectile need to re-push its
material constants right before its draw call instead of once at the
start of the frame?"* — because push constants are pipeline state, not
per-draw-call data; the grid's `vkCmdPushConstants` call earlier in the
same command buffer already overwrote whatever was there, so the
projectile's draw would otherwise silently render with the grid's
material values.

---

### 20. Grid collision + scatter: `objectBuffer_` goes from write-once to per-frame

**Decision:** The projectile now triggers a radial "blast" scatter on
touching any grid instance — CPU-simulated (position + velocity +
framerate-independent damping, not full rigid-body physics), no
automatic return to formation, only a manual **R** key reset. This is
the feature the whole interactive-object arc (§17) was originally built
toward.

**Why this needed zero compute shader or descriptor changes:**
`culling.comp` has never had a concept of a "static" grid — every
dispatch, it reads whatever is currently in `objectBuffer_` and treats
`boundingSphere.xyz` as that instance's position, full stop, for both
the frustum/LOD test and what gets written into the visible-instance
buffers the vertex shader reads. `objectBuffer_` was simply uploaded
once at startup and never touched again — nothing about the GPU-side
pipeline assumed that. Making the grid dynamic was therefore purely a
CPU-side change: `VulkanContext::updateInstanceSimulation()` re-uploads
`objectBuffer_` every frame from simulated positions, and the existing
`VulkanBuffer::upload()` persistent-mapping optimization (the earlier
performance commit) makes that a plain `memcpy`, not a real cost.

**`cachedInstances_` stopped being scratch data.** It used to be
`.clear()`'d and `.shrink_to_fit()`'d at the end of
`initCullingResources()` once the object buffer was built from it — a
correct decision at the time (nothing else needed the grid's rest
positions after init). Now it's the permanent reference
`resetInstanceFormation()` restores from, so that clear was removed.
343×16 bytes ≈ 5.5KB kept alive for the app's lifetime — not worth a
second thought at this scale.

**Framerate-independent damping:** velocity decays via
`dampingFactor = pow(kDampingPerSecond, deltaTime)`, where
`kDampingPerSecond` is "fraction of velocity retained after one full
second" — this is the correct closed form because
`pow(k, dt1) * pow(k, dt2) == pow(k, dt1+dt2)`, so the perceived
deceleration rate doesn't depend on frame rate the way a naive
`velocity *= 0.9` per-frame multiplier would (that would decay faster at
higher framerates, since more multiplications happen per real second).

**One explosion per flight, applied as a blast radius, not a single-point
hit:** the hit test finds the *first* instance the projectile's bounding
sphere touches, then applies a radially-falling-off impulse to *every*
instance within a separate, larger blast radius — not just the one
touched — then immediately calls the new `Projectile::stop()` and
`break`s out of the search. Velocity is accumulated (`+=`), not
overwritten, so a second explosion before the first has settled
compounds rather than resets prior motion.

**`ComputeObjectData` → bare `glm::vec4`, verified byte-identical:** the
struct `initCullingResources()` originally built the upload from
(`struct ComputeObjectData { glm::vec4 boundingSphere; };`) is local to
that function and inaccessible from `updateInstanceSimulation()`.
Rather than hoist it to header scope for one caller, the new method
uploads a plain `std::vector<glm::vec4>` directly — safe because a
struct with exactly one non-static member, no base class, and no
virtual table has identical size and alignment to that member; there is
nothing for the compiler to pad around. Confirmed no `GLM_FORCE_ALIGNED`
or similar macros are defined anywhere in this project that could change
`vec4`'s layout between the two call sites.

**Known, accepted tradeoff — discrete, not swept, collision:** the hit
test only checks the projectile's position once per frame, so at a
large enough `deltaTime`/speed it could in principle tunnel through an
instance without ever registering inside the hit radius. At the
projectile's speed (30 units/s) and normal frame times this isn't
observable (sub-unit movement per frame vs. a ~1.3-unit hit radius), and
nothing else in this codebase clamps `deltaTime` either (e.g.
`Camera::processInput`) — consistent with the project's existing risk
tolerance at this instance count, not a new gap this feature introduces.

**Bundled addition — spin pause/resume (`T` key):** unrelated to the
collision system, but small enough to fold into the same pass.
`GeometryPass`'s rotation used to read `(float)glfwGetTime()` directly
each frame; pausing can't just skip that line; because the model matrix
still needs *an* angle every frame, and reusing the raw clock value
after a pause would snap to wherever the clock currently is rather than
resuming smoothly. Fixed by having `VulkanContext` own the accumulated
angle itself (`spinAngle_`), advanced by `deltaTime` only while not
paused — visually identical to the old behavior when never paused, and
resumes exactly where it left off otherwise.

**Interview-relevant:** *"Why is this simulation update called from
`Application::mainLoop()` instead of from inside `FrameRenderer`'s
`GPUCullingPass` lambda, the way the frustum buffer is updated?"* — both
placements are correct (the host write to coherent memory just needs to
happen-before the `vkQueueSubmit` that reads it, not at any particular
point in command recording), but `Application::mainLoop()` is where
`deltaTime` and the projectile's own `update(deltaTime)` call already
live, and this is world-simulation state, not rendering state — keeping
it there avoids threading a `deltaTime` parameter through
`FrameRenderer::drawFrame()`, which currently takes none.

**Addendum — collision radius separated from the render/culling
radius:** §20's original hit test reused `boundingSphereRadius_` (the
same value `culling.comp` uses for frustum/LOD tests) for collision
too, conflating two conceptually different things: a render/culling
bound must stay geometrically accurate to the mesh or objects visibly
clip/pop, while a collision bound is a gameplay-feel parameter that
teams routinely tune independently (a more forgiving hit radius than
the visible mesh is a standard pattern). Added a separate
`collisionRadius_`, initialized equal to `boundingSphereRadius_` at
startup (a sensible, mesh-derived default — not an arbitrary guess) but
free to diverge via `setCollisionRadius()`. `culling.comp` and the
`objectBuffer_` upload are untouched; only `updateInstanceSimulation()`'s
hit test now reads `collisionRadius_`. Zero behavior change with the
default value — this is groundwork for tuning hit-feel independently of
visuals, not a fix for an observed problem.

---

### 21. Mutual instance collision — fixing post-scatter clipping

**Bug reported:** after a blast, scattered instances that came to rest
near each other visibly clipped through one another ("穿模"). Root
cause: §20's simulation only ever modeled projectile-vs-instance
interaction — instances never collided with *each other*, so nothing
stopped two of them from settling at overlapping positions. This was
latent from the start, not introduced by anything since §20; it just
wasn't visible until instances actually landed close together.

**Why the margin is this tight:** the grid's rest spacing is 3.0 units;
`boundingSphereRadius_` ≈ 1.49, so two axis-adjacent resting instances
sit only `3.0 - 2×1.49 ≈ 0.02` units apart at closest — already almost
touching before anything scatters. Any small drift toward a neighbor
(from a blast, or from a second explosion pushing one instance into
another) crosses that razor-thin margin immediately.

**Fix:** a new pass in `updateInstanceSimulation()`, after velocity
integration and the blast check, before the `objectBuffer_` upload — a
positional pushout over every unique instance pair (`i < j`, so each
pair is checked once, not twice): if two instances are closer than
`minSeparation`, push them apart along the line between their centers.
Deliberately **positional, not velocity-based** — no momentum/
restitution model, no bounce — because the goal is "stop visibly
clipping," not a physically accurate collision response.

**Tuned loose on purpose, not a strict non-overlap constraint** (visual
tuning pass after the initial fix): `minSeparation = 1.5 ×
boundingSphereRadius_`, not the geometrically "just touching"
`2 × boundingSphereRadius_` — this deliberately tolerates some visual
overlap for a denser-looking scatter rather than enforcing that spheres
never touch. Each side of a pair only closes 30% of the overlap per
frame (not an even 50/50 full close), for a softer settle instead of a
hard snap. Runs every frame unconditionally (not just on the impact
frame, unlike the blast), so this partial, repeated correction is
sufficient even when 3+ instances are mutually overlapping: whatever one
frame's partial pass doesn't resolve keeps converging over subsequent
frames rather than needing an iterative in-frame solver.

**Cost, honestly accounted for:** this is `O(n²)` — `343×342/2 ≈ 58,653`
unique pairs, checked *every frame* (not once per explosion, unlike
§20's blast application). Each pair is a `glm::vec3` subtract + `length`
+ compare — trivial per-pair cost, and 58k of them is still comfortably
sub-millisecond on any modern CPU, but this is a real standing per-frame
cost now, not a one-off spike. Consistent with this project's existing
posture at this instance count (the culling shader is itself a flat
343-thread scan, no spatial partitioning) — flagged here rather than
silently accepted, since unlike the blast application, this cost is paid
every single frame regardless of whether anything is currently
scattered.

**Interview-relevant:** *"Why positional correction instead of swapping/
reflecting velocities on collision?"* — a velocity-based response needs
a restitution coefficient, mass assumptions, and angular effects to look
right; a positional pushout is the minimum viable fix for the actual
reported symptom (visual interpenetration), and running every frame
means it's self-correcting rather than needing to be perfectly right in
one pass. Worth revisiting with real impulse response if the scatter
should look more like an actual explosion with bouncing debris rather
than objects that stop dead on contact.

### 22. Shadow mapping: a third render pass, and bugs that only show up geometrically

**Why shadow mapping, not the ray-sphere alternative considered first:**
`culling.comp` already computes a bounding sphere per instance every frame
(`objectBuffer_`), which made an analytic ray-vs-sphere occlusion test in
the fragment shader an appealing zero-new-render-pass option. Rejected in
favor of classic shadow mapping because sphere-approximated occluders
would give blobby, geometrically-wrong shadow edges (visibly wrong against
Suzanne's actual silhouette — horns, ears), whereas shadow mapping shadows
the real rasterized geometry. The tradeoff accepted: a second render pass,
a second pipeline, and a new descriptor binding.

**FrameGraph needed a third pass "stage", not just a third pass.**
Before this, `FrameGraph` had exactly two: `Compute` (runs outside any
render pass, `executeCompute()`) and `Graphics` (all passes sharing the
*one* render pass `FrameRenderer::drawFrame()` already begins/ends). The
shadow pass needs its own render pass (different attachment — a sampled
depth image, not the swapchain's color+depth) and its own fixed-resolution
framebuffer, so a `Graphics`-stage pass couldn't express it — it would run
inside the wrong render pass. Added `PassStage::Shadow` and
`FrameGraph::executeShadow()`, mirroring `executeCompute()`'s existing
precedent of "a stage gets its own explicit wrapper in `drawFrame()`"
rather than trying to make one `RGPass` struct carry per-pass render-pass/
framebuffer state (a real generalization, but not needed at 2→3 passes).

**`objectBuffer_` reused directly as the shadow pass's instance buffer.**
It already holds `vec4(position.xyz, radius)` per instance, re-uploaded
every frame by `updateInstanceSimulation()` (§20) — byte-identical to
`InstanceData`'s single-`vec4` layout. The shadow pass binds it straight
as its per-instance vertex input and draws all `OBJECT_COUNT` instances
unculled (no light-frustum culling — unnecessary at 343 instances). Zero
new buffer, zero compute changes — same "the shader/buffer never had a
concept of static vs. dynamic" reasoning as §20's original write-once→
per-frame pivot.

**Bug 1 — `objectBuffer_` was missing `VK_BUFFER_USAGE_VERTEX_BUFFER_BIT`.**
It was created with only `STORAGE_BUFFER_BIT` (culling.comp's SSBO
binding). Reusing it as a vertex buffer via `vkCmdBindVertexBuffers`
without also declaring that usage bit is invalid per spec regardless of
whether a given driver happens to tolerate it silently — caught by
re-reading the buffer's creation flags before assuming the reuse was
free, not by a validation-layer message actually being observed. Fixed
by OR-ing in `VK_BUFFER_USAGE_VERTEX_BUFFER_BIT` at creation.

**Bug 2 — `glm::ortho()` uses the wrong depth convention for Vulkan, and
it's *silent* for a perspective matrix but *geometrically wrong* for an
orthographic one.** This project never defines
`GLM_FORCE_DEPTH_ZERO_TO_ONE`, so every GLM projection defaults to
OpenGL's `z_ndc ∈ [-1,1]` convention, not Vulkan's `[0,1]`.
`Camera::getProjectionMatrix()` has used plain `glm::perspective()` since
Phase 3 with no apparent ill effect, which made it easy to assume
`glm::ortho()` would be equally safe for the new light-space matrix — it
is not. For a *perspective* matrix the mismatch only shifts where
`z_ndc = 0` falls to a point very close to the near plane (worked out
numerically for this project's near/far of 0.1/200: the invalid `z_ndc<0`
region is only the eye-space sliver from distance 0.1 to ≈0.2 — invisible
at any camera distance this scene actually uses). For an *orthographic*
matrix the near→far mapping is linear, so the same wrong convention
clips away the near **half** of the light's frustum outright — roughly
whichever half of the grid sits closer to the light in a given frame
would have silently failed the primitive clip test and never made it
into the shadow map at all. Fixed by calling `glm::orthoRH_ZO(...)`
directly (GLM exposes explicit-convention variants independent of the
global force-macro) instead of either changing GLM's project-wide default
or the plain `glm::ortho()`. Lesson: "it's worked fine elsewhere in this
codebase" doesn't transfer between a perspective and an orthographic use
of the same underlying convention mismatch — the failure mode is
different in kind, not just degree.

**Shadow acne on directly-lit surfaces, worse-looking from a distance.**
Milestone 2's single-tap depth comparison showed pixel-level speckling on
the grid's top layer (the layer with the least self-occlusion, so
otherwise-uniform lit patches make any noise most visible) — classic
shadow acne, confirmed by the noise being per-pixel/grainy rather than a
solid mis-shaped region (which would have pointed at the light matrix
instead). Fixed with the standard pairing: a runtime-tunable base bias
(`SceneData.shadowParams.x`, an ImGui slider — right value depends on
shadow-map resolution and scene scale, easier to find by eye than to
compute) plus 3×3 PCF (averaging 9 depth comparisons smooths single-texel
noise into a soft result and softens shadow edges as a side effect).

**Addendum — shadow silhouette desynced from the spinning mesh.** After
the acne fix, a second, unrelated symptom surfaced: shadows on grid
instances that visibly had nothing occluding them, reported as "not sure
if this is an algorithm bug or a rendering artifact." The distinguishing
signal was that this one was *coherent* (a whole stale silhouette,
tracking with how long the app had been running) rather than *noisy*
(individual speckled texels, like the acne bug) — same broad symptom
category, different mechanism, and worth telling apart before reaching
for bias/PCF again. Root cause: `shadow.vert` never applied `ubo.model`
(the grid's continuously-accumulating spin) the way `triangle.vert`
does, so the shadow map was always cast from each mesh's un-rotated rest
pose while the visible geometry spun independently — the two would only
ever agree at `spinAngle() == 0`. Fixed by giving the shadow pass's push
constant a second `mat4 model` field (`ShadowPushConstants`, now in
`VulkanShadowPipeline.h` so both the pipeline and `FrameRenderer` share
one definition — 128 bytes total, exactly Vulkan's guaranteed minimum
push-constant size) and pushing the same per-draw rotation
`GeometryPass` already computes: `spinAngle()`'s rotation for the grid,
identity for the projectile. General lesson: any per-draw transform
applied in the main pass's vertex shader (rotation, and equally scale if
this project ever adds it) has to be applied identically in *every*
other pass that transforms the same geometry — a shadow pass is a second,
independent vertex transform of the same mesh, not a derivative of the
first one, so nothing keeps them in sync automatically.

### 23. GPU timestamp performance instrumentation: reusing an existing safe-readback pattern

Closes an open item carried since Phase 5 ("demonstrated functionally,
not yet measured numerically" — see the roadmap's old "Open / not yet
started" entry). The instrumentation itself is small; the interesting
part is that every design choice in it was already established by
earlier work rather than invented fresh.

**Query timing reuses the LOD visible-count readback's exact safe
window, not a new one.** `FrameRenderer::drawFrame()` already reads back
the previous frame's 3 LOD counts right after `vkWaitForFences` for that
frame slot (§11: this is safe specifically because the fence wait just
proved that slot's prior GPU work is fully complete). A frame slot's
`VkQueryPool` results become valid at exactly the same moment, for
exactly the same reason — so the GPU timing readback was added as a
second block right next to the LOD-count one, not a new mechanism. No
`VK_QUERY_RESULT_WAIT_BIT`, no extra stall: the wait already happened.

**One timestamp write per `FrameGraph` stage boundary, not a granular
profiler.** 4 `vkCmdWriteTimestamp` calls (frame start, compute end,
shadow end, graphics end) map exactly onto the 3 `PassStage` values
(`Compute`/`Shadow`/`Graphics`) `FrameRenderer::drawFrame()` already
wraps individually (§4, §22). Splitting `Graphics` further into
`GeometryPass` vs. `ImGuiPass` would need a 5th marker for a number
nobody asked for (ImGui's own cost) — the roadmap's stated goal was
quantifying culling/LOD/shadow cost, and 3 intervals + a total answers
that directly.

> **Update (§24):** `GeometryPass` and `ImGuiPass` *did* later end up in
> separate render passes (`PassStage::Graphics` vs. the new
> `PassStage::UI`), but that split was driven by the dockable-viewport
> work, not a timing need — the 4th timestamp write simply moved to after
> the *second* of the two passes, so `graphicsMs` still measures the same
> "everything after the shadow pass" span this section describes. Still
> no 5th marker; the reasoning above for not wanting one is unchanged.

**`TOP_OF_PIPE`-start / `BOTTOM_OF_PIPE`-end, not a stage-specific bit
per boundary.** A `BOTTOM_OF_PIPE` timestamp only fires once *everything*
submitted before it in the command buffer has finished — which is
exactly "the end of this interval" for a boundary marker, regardless of
which pipeline stages happen to be involved on either side of it. Picking
a narrower stage bit per boundary (e.g. `COMPUTE_SHADER_BIT` for the
compute/shadow boundary) would require reasoning about exactly which
stages could still be in flight at each point — `BOTTOM_OF_PIPE` sidesteps
that by construction, at the cost of nothing here since these are
already hard synchronization points (render pass boundaries, barriers).

**Timestamp support is queried once and degraded gracefully, not
assumed.** `VkPhysicalDeviceLimits::timestampComputeAndGraphics` covers
the common case; a GPU where that's false still might support timestamps
on the graphics queue specifically (`VkQueueFamilyProperties::
timestampValidBits`), so `VulkanDevice::queryTimestampSupport()` checks
both before concluding timestamps aren't available. When they aren't,
`FrameContext::queryPool` stays `VK_NULL_HANDLE` and every downstream
step (`createQueryPools()`, the writes in `drawFrame()`, the readback,
the ImGui display) checks for that and no-ops or shows "N/A" — consistent
with this project's existing posture of not crashing on unusual hardware
without adding speculative complexity for cases that can't be tested here.

---

### 24. Dockable ImGui viewport: why not Qt, and why fixed-resolution

**Decision:** The 3 debug windows used to be plain `ImGui::Begin()` calls
with no assigned position, cascading over the top-left corner of the
window and on top of the rendered grid. Reworked so the 3D scene renders
into an offscreen target and is displayed inside a dockable ImGui
"Viewport" panel, with the debug windows docked beside it instead of
overlapping it — a real editor-style layout, not just repositioned
floating windows.

**Why not just reposition the existing windows (the cheapest option):**
considered first, but it leaves the debug windows sitting on top of the
3D image regardless of where they're pinned — the 3D output still
occupies the full framebuffer underneath them. It doesn't give a real
"extra area" the way docking does.

**Why not carve out a static side region with a custom `VkViewport`
(no docking, no offscreen target):** would need the geometry pipeline's
viewport/scissor to target a sub-rectangle of the swapchain framebuffer
and `Camera`'s aspect ratio to match it — mechanically simpler than a
second render pass, but it produces a fixed, undraggable split (no true
docking/resizing/rearranging), and this codebase's pipelines already bake
viewport/scissor as static pipeline state (`VulkanPipeline.cpp`), so
"resizable" was never actually on the table without dynamic viewport
state either way. Rejected in favor of the more capable option once it
turned out not to cost meaningfully more.

**Why not Qt (the user's explicit fallback if the ImGui approach turned
out to be bloated):** researched before committing to an approach.
`QVulkanWindow` exists and can be embedded in a `QWidget` tree via
`createWindowContainer()`, but there is no existing ImGui-Vulkan-Qt
integration (`qtimgui` only supports the OpenGL widget/window backends,
not Vulkan) — building one would mean replacing GLFW's window/input
handling (mouse-look cursor capture, WASD polling, the projectile's
click-fire gating on `ImGui::GetIO().WantCaptureMouse`) and adding Qt to
a build system that currently vendors everything itself with zero
external package managers (see `docs/setup.md` §6). All of that for a
capability ImGui's docking branch already provides directly.

**Why the ImGui-docking approach turned out not to be bloated:** the
exact mechanism needed — render a Vulkan image, register it with
`ImGui_ImplVulkan_AddTexture()`, display it via `ImGui::Image()` — was
already in this codebase for the Shadow Map debug preview (§22). The
sanctioned way to add a structurally different render pass to
`FrameGraph` (a new `PassStage` + `execute*()` + explicit wrapper in
`drawFrame()`) was already documented in `docs/setup.md` §8 as the
template the Shadow stage set. The new offscreen target class
(`VulkanSceneColorTarget`) is a near-copy of `VulkanShadowMap` with color
usage bits instead of depth. Net new code ended up comparable to the
shadow-mapping feature (Phase 9) — an existing, already-shipped feature
of similar shape — not a new category of complexity. `third_party/imgui`
being a plain git submodule made moving to the `docking` branch a normal
commit-pointer change, not a fork/vendor operation.

**Why the offscreen scene target is fixed-resolution, not resized to
match the docked panel's pixel size:** `Camera::ASPECT_RATIO` is already
a hardcoded constant (`1280.0f/1024.0f`, comment: "fixed window size, not
resizable"), and no swapchain-recreation-on-resize code exists anywhere
in this codebase — `VulkanPipeline`'s viewport/scissor are static
pipeline state, baked in at creation, not `VK_DYNAMIC_STATE_VIEWPORT`.
Building live-resize plumbing for just the new viewport target, in a
codebase that has never needed it even for the swapchain itself, would
be solving a problem this project doesn't otherwise have. `ImGui::Image()`
scales the fixed-resolution texture to whatever size the panel ends up
being, so dragging/docking/resizing the panel still works visually — the
render resolution just doesn't increase with it. Flagged in
`docs/roadmap.md`'s Open items, not treated as a defect.

**Structural changes this required:**
- `FrameGraph::PassStage` gained a 4th value, `UI` — `GeometryPass`/
  `LightingPass`/`PostProcess` stay `Graphics` (now understood as "the
  offscreen scene pass"); `ImGuiPass` moved to `UI`, running in the
  swapchain's own render pass, which now hosts *only* UI.
- `VulkanContext::pipeline_` (the geometry pipeline) now targets the new
  `sceneRenderPass_`/`sceneColorTarget_.extent()` instead of the
  swapchain's `renderPass_`/extent — same shaders, same descriptor
  layout, just a different `VkRenderPass` handle and viewport, since both
  happen to already be `1280×720`. **Addendum:** for the first several
  commits this was actually `1280×1024` — a typo in
  `VulkanSceneColorTarget::HEIGHT`, unnoticed because nothing crashes or
  validates against it; see the bugs table for the aspect-mismatch
  consequence and how it was caught.
- `FrameRenderer::drawFrame()` now begins two render passes where it used
  to begin one: the offscreen scene pass (`executeGraphics()`), a color
  barrier (`COLOR_ATTACHMENT_WRITE_BIT → SHADER_READ_BIT`, mirroring the
  existing depth barrier's shape from §22), then the swapchain pass
  (`executeUI()`). The 4th GPU timestamp write moved to close over both
  passes combined, keeping `graphicsMs`'s meaning ("everything after the
  shadow pass") unchanged from §23.

**Interview-relevant:** *"Why does adding a dockable viewport touch
`FrameGraph` at all instead of just being an ImGui-side change?"* —
because the 3D scene has to stop rendering directly to the swapchain and
start rendering to a sampled offscreen image instead; that's a second
render pass with different attachments, which is exactly the kind of
structurally-different pass this codebase already has a sanctioned
extension mechanism for (`PassStage` + `execute*()`), not a UI-only
change.

### 25. Texture-based PBR materials: a `Material` class, and a NaN bug the placeholder assets made inevitable

**Decision:** PBR milestone 1 (§19) proved the lighting math with flat
push-constant material params; this milestone adds real textures behind
them — a `Material` class (`include/vulkan/texture/Material.h`) bundling
albedo/normal/metallic-roughness/AO, `VulkanDescriptor` growing from 4 to
7 bindings, and `triangle.frag` sampling all four instead of just albedo.

**The blocking problem: Suzanne has zero real UV data.** `triangle.frag`
already sampled a texture for albedo and `Vertex::uv` was already wired
through the whole pipeline (§13's earlier isolated validation), but
`assets/suzanne.obj` (and both LOD variants) have no `vt` lines at all —
confirmed the same way §13's original gap was confirmed, by grepping the
source file rather than assuming. Asked the user how to unblock this;
directed to search GitHub rather than switch the demo mesh or hand-roll
triplanar projection. Found `opengl-tutorials/ogl`'s `suzanne.obj` — the
same base Blender monkey mesh, real `vt`/`vn` data, from the widely-used
opengl-tutorial.org series. **Provenance, stated plainly rather than
papered over:** no explicit LICENSE file exists in that source repo.
Suzanne itself is Blender's own bundled default primitive, and
unattributed re-exports of it are ubiquitous across the real-time
graphics tutorial ecosystem (LearnOpenGL, Sascha Willems' Vulkan
samples, this exact repo) — a strong convention, but not a documented
grant. Put to the user directly; the resolution chosen was clear
attribution rather than leaning on that convention silently — both
`assets/suzanne_pbr.obj`'s own header comment and `README.md`'s "Asset
Credits" section now name the exact source URL.

**Scoping decision: only LOD0 gets the new mesh.** Generating matching
UV-preserving LOD1/LOD2 decimations needs a 3D tool (Blender) not
available in this environment. LOD1/LOD2 keep sampling a constant
`uv = (0,0)` texel — the same flat-tinted look they already had, not a
regression, just an explicitly named gap (same treatment as every other
"good enough now" simplification this project tracks, e.g. the flat
ambient term standing in for real IBL).

**`VulkanTexture` gained a `VkFormat` parameter instead of a second
class.** Albedo is color data (`VK_FORMAT_R8G8B8A8_SRGB` — sampling
auto-converts sRGB→linear, same reasoning as §13's original format
choice); normal/metallic-roughness/AO are not color data and must be
read back as raw bytes (`VK_FORMAT_R8G8B8A8_UNORM`) or the values come
out wrong. One parameterized class, not a near-duplicate "non-color
texture" class, since the only real difference is which enum value gets
passed to `vkCreateImage`/`vkCreateImageView`.

**Metallic-roughness/AO are textures multiplying existing push-constant
factors, not textures replacing them.** `triangle.frag`'s
`finalAlbedo = material.albedo.rgb * texColor` pattern already
established "push constant is a per-draw factor, texture is the spatial
detail" — extended the same way to metallic/roughness (glTF's channel
convention: G=roughness, B=metallic, so any future swap to a real
authored/downloaded texture set drops in with no repack) and AO
(multiplies the ambient term only — the direct `Lo` term already has its
own separate occlusion signal, the shadow map, so AO touching it too
would double up two different occlusion sources for no reason; same
"don't touch the shadowed direct term" discipline §22's `calcShadow()`
already established).

**Normal mapping: derivative-based tangent frame, not a vertex
attribute — deliberately, and it exposed a real bug.** Adding a `tangent`
field to `Vertex` would mean recomputing it for all 3 LOD meshes and
touching `ObjLoader`; the alternative (reconstructing a per-pixel TBN
basis from `dFdx`/`dFdy` on the fragment's world position and UV —
Schuler's "normal mapping without precomputed tangents" technique) needs
zero vertex-format changes. Implemented it, and hit a NaN bug immediately
on first run that turned the *entire* scene solid black, not just the
newly-textured mesh:

```glsl
float invmax = inversesqrt(max(dot(T, T), dot(B, B)));
```

`T`/`B` (the reconstructed tangent/bitangent) are built from `dFdx(uv)`/
`dFdy(uv)`. For any mesh with **no real UV variation across a pixel** —
which is exactly what LOD1/LOD2 have, since `ObjLoader` falls back to a
constant `uv = (0,0)` when an OBJ has no texcoord data (confirmed in
`ObjLoader.cpp`: `index.texcoord_index >= 0` gates reading real UVs, an
`else` branch sets the constant) — `dFdx(uv)` and `dFdy(uv)` are exactly
`(0,0)` at every pixel, so `T = B = vec3(0)`, `dot(T,T) = dot(B,B) = 0`,
and `inversesqrt(0)` is `+Inf`. `vec3(0) * Inf` is `NaN` per IEEE 754 —
not zero — and that NaN propagates through the TBN matrix, the perturbed
normal, `NdotL`/`NdotV`, and finally `outColor` itself.

**Why this made the *whole scene* black, not just LOD1/LOD2:** the
default camera position (`(0,0,25)`, per `Camera.h`) is far enough from
the 7×7×7 grid (spacing 3.0, so the grid's near face is only at `z≈9`)
that no instance is ever within `LOD1_DIST = 12.0` at startup — the
default view shows *only* LOD1/LOD2. The newly-textured LOD0 mesh was
never actually on screen during the first test; every visible instance
was hitting the degenerate-UV-gradient path. This is a good example of
why "run it and look" beats reasoning about a shader change in the
abstract — the bug was invisible from reading the code (the math looks
correct for any mesh with real UV variation) and only showed up by
actually launching the app.

**Fix:** guard the degenerate case and fall back to the unperturbed
geometric normal — exactly the correct behavior for a mesh with no real
UV data to perturb against, and it costs one branch:

```glsl
float maxLenSq = max(dot(T, T), dot(B, B));
if (maxLenSq < EPS)
    return N;
```

**Interview-relevant:** *"Why does a normal-mapping shader change break
meshes that were never touched?"* — because the shader is shared by every
draw using this pipeline regardless of mesh, and a `0 * Inf = NaN` in
GLSL isn't a graceful "no effect," it's a poison value that propagates
through every subsequent operation touching it, including in a
draw call whose mesh was never the one being tested.

---

### 26. LOD distance thresholds: piggyback on the existing FrustumData upload, not a new buffer

**Decision:** `culling.comp`'s `LOD1_DIST`/`LOD2_DIST` (§15) were `const
float` shader constants - closing that "Not yet done" item meant making
them runtime-tunable. Added a 4th field, `lodDistances` (`vec2` worth of
data in a `vec4` slot for alignment), to the existing `FrustumPlanes`/
`FrustumData` struct that already gets uploaded to the GPU every frame
in `GPUCullingPass`, rather than a new UBO or a new compute descriptor
binding.

**Why extend an existing buffer instead of adding a new one:** the
frustum data is already re-uploaded every frame (camera moves every
frame, so it has to be), and the LOD thresholds only need to change when
a user drags a slider - piggybacking costs one `vec4` of upload bandwidth
that was going to happen anyway, versus a whole new `VkBuffer` +
`VkDescriptorSetLayoutBinding` + pool size entry + `vkUpdateDescriptorSets`
call for two floats. `ComputeDescriptor` and `culling.comp`'s binding
count stay unchanged (8 bindings) - only the `FrustumData` block's byte
size grows, from 112 to 128 bytes, and `frustumSize = sizeof(FrustumPlanes)`
in `initCullingResources()` already computes that size dynamically, so
the buffer and the descriptor's bound range both pick up the change with
no hardcoded byte count to update.

**Why the setters enforce `lod2Distance_ >= lod1Distance_` instead of
trusting the ImGui call site:** `culling.comp`'s bucketing is an
if/else-if chain (`camDist < LOD1 → LOD0`, `camDist < LOD2 → LOD1`, else
→ LOD2), which silently produces a confusing result if the thresholds
cross - an instance between the (now-inverted) LOD2 and LOD1 values would
still pass the first check and get bucketed into LOD0. Rather than
relying on the ImGui slider code to never let that happen,
`VulkanContext::setLod1Distance()`/`setLod2Distance()` clamp the
invariant themselves, so any future caller (not just the one ImGui window
today) can't produce the broken state.

**Verification:** dragged `LOD1 Distance` in the "GPU Culling Stats"
window up past 25 (the default camera's distance from the grid) and
watched every instance reclassify to LOD0 live, with the visible mesh
detail changing across the whole grid in the same frame - the same
"verify experimentally, don't just trust the math" instinct as the
`sin(time)` oscillation test in §7.

---

### 27. Left-click-to-fire intermittently not registering right after releasing Ctrl

**Reported symptom:** after holding Ctrl to interact with a debug window
and releasing it, the projectile's left-click trigger wouldn't always
fire on the very next click - it usually took a large mouse-look swing
before clicking started working again. User's own diagnosis, stated up
front: suspected a conflict between the UI's hit-test region and the
cursor's own region - which turned out to be exactly right.

**Root cause:** `Camera::processInput()`'s Ctrl handling (§18 addendum)
switches `GLFW_CURSOR` between `NORMAL` (visible, for clicking ImGui) and
`DISABLED` (hidden, unbounded virtual position, for mouse-look) -
correct on its own. The missed half: `imgui_impl_glfw.cpp` does **not**
ignore mouse position while `GLFW_CURSOR_DISABLED` is active (confirmed
in its own changelog: *"2023-07-18: Inputs: Revert ignoring mouse data on
GLFW_CURSOR_DISABLED as it can be used differently. User may set
ImGuiConfigFlags_NoMouse if desired."*) - it keeps feeding the unbounded
virtual position into `io.MousePos` as if it were a real screen
coordinate, and ImGui hit-tests window rects against it every frame
regardless.

The instant Ctrl is released, GLFW's virtual position doesn't jump - it
continues from wherever it was the moment before release (this is
deliberate, see `firstMouseSample_` in §18, added specifically to avoid
a *different* jump artifact). Before Phase 11, that residual position
landing inside one of the 3 small floating debug windows was unlikely -
most of the screen wasn't an ImGui window at all. Since Phase 11's
dockable viewport, the **entire client area** is docked ImGui windows
(Viewport + the 3 debug tabs), so the residual position is almost always
still "inside some window," `WantCaptureMouse` stays stuck `true`, and
`Application.cpp`'s `!ImGui::GetIO().WantCaptureMouse` click-fire gate
(unchanged since §17) blocks the shot. Moving the mouse to look around
eventually drags the virtual position far enough to fall outside every
window's rect, at which point `WantCaptureMouse` finally clears and
clicking works again - matching "occasionally only after a big view
swing" exactly.

**Fix:** exactly what ImGui's own changelog recommends - toggle
`ImGuiConfigFlags_NoMouse` in `Application::mainLoop()`, synced to
`Camera::cursorVisible()` every single frame (not just on the Ctrl
transition, so it self-corrects regardless of frame ordering):

```cpp
if (context->camera().cursorVisible())
    io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
else
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
```

While the cursor is disabled (look mode), ImGui now ignores mouse input
entirely - `WantCaptureMouse` is always `false`, and the stale/unbounded
position can never be misread as "hovering a window," because ImGui
isn't looking at it at all. While Ctrl is held (cursor visible), the flag
clears and the debug windows work exactly as before.

**Why this is the right layer to fix it at, not the click-gate
condition:** a narrower fix (e.g. `!cursorVisible() || !WantCaptureMouse`
only at the fire-gate) would have patched just the reported symptom, but
left the same stale-position mechanism free to cause other, unreported
mouse-capture confusion during look mode (phantom hover highlighting,
a docked panel's drag/resize technically still being "live" from
ImGui's perspective while the user can't even see their cursor).
Disabling ImGui's mouse processing outright for the whole duration
cursor is captured is the actually-correct statement of intent: the user
cannot interact with any widget while the cursor is invisible and
captured, so ImGui shouldn't believe it can capture anything either.

**Interview-relevant:** *"Why does a UI-interaction bug only show up
after adding a dockable viewport, when the underlying cursor-mode code
was already there?"* - because the *rate* of a stale-position false
positive is a direct function of how much of the screen is covered by
ImGui windows. At 3 small floating windows the odds were low enough to
go unnoticed; at "the entire client area," they round up to "almost
every time."

### 28. Light-frustum culling for the shadow pass, and re-verifying the camera path's plane extraction first

`ShadowPass` (§22) drew all `OBJECT_COUNT` instances unculled from
`objectBuffer_` directly — accepted at the time as "fine at this
instance count." Adding real culling meant first checking whether the
existing camera-frustum plane extraction (`FrustumPlanes::
extractFromMatrix`, used by `culling.comp` every frame) was even
correct to begin with, since a bug there would just get copy-pasted
into the new light path.

**Camera path re-verified as correct.** `extractFromMatrix` uses the
standard Gribb-Hartmann trick — `glm::transpose(viewProj)` then
combining rows (`m[3]±m[i]`) — which assumes GLM's default OpenGL-style
`z_ndc ∈ [-1,1]` convention. `Camera::getProjectionMatrix()` uses plain
`glm::perspective()` with no `GLM_FORCE_DEPTH_ZERO_TO_ONE`, so it *is*
in that convention — the near/far plane formulas (`m[3]+m[2]` /
`m[3]-m[2]`) match. No bug here.

**Reusing it verbatim for the light would have been wrong.** §22 already
established that `VulkanContext::lightViewProj()` deliberately uses
`glm::orthoRH_ZO()` — Vulkan's native `z_ndc ∈ [0,1]` — specifically
*because* the `[-1,1]` convention geometrically breaks an orthographic
matrix (clips away the near half). That same fact breaks Gribb-Hartmann
plane extraction one level up: the near-plane formula for `[0,1]` is
`m[2]` alone, not `m[3]+m[2]` (far/left/right/top/bottom are convention-
independent, so those three lines are unaffected). Using the `[-1,1]`
near-plane formula on the light's `[0,1]` matrix wouldn't crash or
validation-error — it would silently produce a near plane so permissive
it never culls anything near the light, the same "silent, not loud"
failure shape §22's original ortho bug had. Fixed by giving
`extractFromMatrix` a `zeroToOne` parameter (default `false`, so the
camera call site is untouched) that switches only that one line.

**Implementation reuses the existing `culling.comp` dispatch rather than
adding a second one.** The shader already loops over all 343 objects
once per frame and branches into 3 camera-visible LOD buckets; it now
also runs an independent light-frustum sphere test per object
(unconditional — not gated on camera visibility, since a shadow caster
can be off-screen) and compacts survivors into a 4th
`{ShadowVisible, ShadowIndirect}` output pair, same atomic-counter
compaction pattern as the LOD buckets (§7). `ComputeDescriptor` grew
from 8 to 11 bindings (bindings 8-10: shadow-visible buffer, shadow
indirect-draw buffer, light-frustum UBO — the light frustum reuses the
`FrustumPlanes` struct/std140 layout for its buffer even though only
`planes[6]` is meaningful, avoiding a second GLSL struct definition).
`ShadowPass` switched from a direct `vkCmdDrawIndexed(..., OBJECT_COUNT,
...)` reading `objectBuffer_` straight, to a `vkCmdDrawIndexedIndirect`
reading the new compacted buffer/command — the exact shape
`GeometryPass`'s per-LOD draws already use.

**No new barrier needed.** The existing compute→graphics
`VkMemoryBarrier` in `FrameRenderer::drawFrame()` (§5) is a blanket
`SHADER_WRITE → VERTEX_ATTRIBUTE_READ | INDIRECT_COMMAND_READ`, not
scoped to specific buffers — it already covers the two new buffers for
free, since they're written by the same compute dispatch it was already
ordering against the shadow pass's reads.

### 29. `lightViewProj()`'s scene radius: fixed constant → derived from live scatter state

`kSceneRadius` (`VulkanContext::lightViewProj()`) had been a hardcoded
`24.0f` since shadow mapping was first implemented (§22) — sized to
cover the 7×7×7 grid's rest-formation half-diagonal (~15.6 units) plus
margin. The grid collision + scatter system (§20/§21) can permanently
push instances outward via a projectile blast impulse — there's no
automatic return to rest, only a manual **R**-key
`resetInstanceFormation()`. A blast large enough to push instances past
the fixed 24-unit box would silently clip them: they'd fall outside the
ortho box's depth range in the shadow pass's fragment tests, *and*,
since §28 added light-frustum culling using this same box, fail the
light-frustum cull and never even get submitted to the shadow pass's
draw call in the first place. Both known gaps, both closed by the same
fix.

**Fix: compute the radius from live state, every frame, inline inside
`lightViewProj()`.** Kept as a self-contained `const` function reading
existing members (`instanceCurrentPositions_`, `boundingSphereRadius_`)
— no new state, same "single source of truth, no side channels" shape
`Camera::getProjectionMatrix()` already uses:

```cpp
constexpr float kMinSceneRadius = 24.0f;   // the old constant, now a floor

float maxDist = 0.0f;
for (const auto& p : instanceCurrentPositions_)
    maxDist = std::max(maxDist, glm::length(p));

float sceneRadius = std::max(kMinSceneRadius, maxDist + boundingSphereRadius_);
```

`boundingSphereRadius_` margins the *mesh extent* of the outermost
instance, not just its center point — same reasoning `objectBuffer_`'s
per-instance bounding sphere already uses everywhere else. The floor
means this is a true no-op in the rest-formation case: before any blast,
`maxDist + boundingSphereRadius_` computes to the same value
`kSceneRadius` used to be a constant at, so nothing visually changes
until a blast actually happens.

**Three design decisions made explicitly, not by default:**

- **Recomputed fresh every frame, not smoothed or quantized.** A
  per-frame-varying ortho box size changes the fixed 2048×2048 shadow
  map's world-space-per-texel ratio, a known cause of shimmering shadow
  edges ("shadow swimming") even on otherwise-static geometry. Accepted
  as an explicit tradeoff for staying a simple, stateless function of
  current positions — this project's existing precedent for "small
  cosmetic aliasing artifact, not worth the complexity to fully solve"
  is the shadow acne fix in §22 (bias + PCF, not a perfect solution
  either). If it ever becomes visually bothersome, the fix would be
  quantizing `sceneRadius` to a coarse step (e.g. round up to the
  nearest few units) rather than a full temporal-stabilization scheme —
  low effort, meaningfully reduces the frequency of resize-driven
  shimmer without adding state.
- **Grid instances only — the projectile is excluded.** Its own flight
  path can carry it far outside the grid's footprint; including it would
  make `sceneRadius` (and therefore shadow-map resolution density for
  everything else) swing on a single short-lived, low-visual-priority
  object. The tradeoff: the projectile's own shadow can get clipped
  while it's far from the grid. Deliberate, not an oversight — see the
  "Remaining gap" note in "Open items" below.
- **Cost is a non-issue.** One O(343) scan per frame is the same order
  of magnitude as the O(n²) mutual-collision pass
  `updateInstanceSimulation()` already runs every frame (§21) — adding
  it to a function already called once per frame from `GPUCullingPass`
  and `GeometryPass` (§28) isn't a measurable regression.

**No downstream wiring needed.** The light-frustum culling added in §28
already calls `lightViewProj()` fresh every frame
(`FrustumPlanes::extractFromMatrix(context->lightViewProj(), ...,
/*zeroToOne=*/true)`) — the dynamically-sized frustum flows through to
that culling test automatically, the same way it already flows through
to the shadow pass's depth test and the `triangle.frag` shadow sample.
One function change, three consumers updated for free — a direct payoff
of `lightViewProj()` having stayed a pure, single-sourced function
instead of accumulating separate per-consumer copies.

### 30. Mutual instance collision: from pure positional pushout to a velocity-impulse + positional-correction hybrid

§21's mutual-collision fix was deliberately positional-only: two
overlapping instances get shoved apart along the line between their
centers, no velocity read or written. That section's own
"Interview-relevant" note already named the tradeoff — "worth revisiting
with real impulse response if the scatter should look more like an
actual explosion with bouncing debris rather than objects that stop dead
on contact." This section is that revisit.

**Why not just add the impulse and drop the positional correction
entirely.** A velocity impulse only has something to act on when a pair
is *approaching* — the physically meaningful quantity is
`dot(relativeVelocity, contactNormal)`, and an impulse is only computed
when that's negative. A pair that's already overlapping with near-zero
relative velocity (the common end state here: damping asymptotically
kills velocity every frame, §20) triggers no impulse at all, because
there's no approach to counteract. Pure velocity resolution alone would
leave such a pair visibly interpenetrated *indefinitely* — not a
transient glitch, a permanent one, since nothing else in the simulation
would ever separate them. This is why every real-time physics engine
pairs impulse resolution with some form of positional correction rather
than using one exclusively; this project does the same, just with a much
lighter positional term now that the impulse handles the dynamic case.

**Implementation** — both steps live inside the same `if (dist <
minSeparation)` branch, in the existing `O(n²)` unique-pair loop
(`updateInstanceSimulation()`, `src/vulkan/VulkanContext.cpp`), no new
pass:

1. **Velocity impulse**, equal-mass (every grid instance shares
   `boundingSphereRadius_`, so mass is implicitly 1:1 — no per-instance
   mass tracking needed), restitution-scaled, along the contact normal
   `n`:
   ```cpp
   glm::vec3 relVel = instanceVelocities_[j] - instanceVelocities_[i];
   float velAlongNormal = glm::dot(relVel, n);
   if (velAlongNormal < 0.0f)
   {
       glm::vec3 impulse = -(1.0f + restitution_) * velAlongNormal * 0.5f * n;
       instanceVelocities_[i] -= impulse;
       instanceVelocities_[j] += impulse;
   }
   ```
   This is the standard textbook equal-mass, restitution-scaled impulse
   formula. `restitution_ = 0` reduces to instances stopping dead along
   the normal (matching the old pushout's visual "stop on contact," just
   arrived at through momentum instead of position); `restitution_ = 1`
   is a fully elastic bounce.
2. **Positional correction**, kept but scaled down from 30%-of-overlap-
   per-side to 10%: now purely a safety net for the resting-overlap case
   the impulse can't touch, not the primary separation mechanism, so it
   no longer needs to do most of the work alone.

**`restitution_` is runtime-tunable, not hardcoded** — `VulkanContext::
restitution()`/`setRestitution()` (default `0.3f`, clamped to `[0,1]`),
exposed via a new "Collision" section in the "GPU Culling Stats" ImGui
window (`FrameRenderer.cpp`). Same reasoning already applied to
`shadowBias_`/`lod1ScreenSize_`/`lod2ScreenSize_`: the right-feeling value
depends on scene scale and blast strength in ways easier to dial in by
eye than to derive, and this project already has the "expose it, don't
guess it" pattern established for exactly that situation.

**Cost:** unchanged asymptotically — still the same `O(n²)` ≈ 58,653-pair
scan §21 already accounted for, with a few extra vector ops per pair that
actually overlaps. Not a new standing cost category, just slightly more
work inside a branch that was already there.

**Interview-relevant:** *"Why not drop the positional correction now
that there's a real impulse?"* — because impulse resolution is
velocity-based by construction, and velocity-based correction cannot
separate two bodies that have already interpenetrated and stopped moving
relative to each other; it can only prevent *future* interpenetration
from an approaching pair. Position and velocity are different quantities
solving different halves of the same problem (respectively: "are you
currently overlapping" and "are you about to be") — a complete resolver
needs both, which is exactly why the impulse+correction pairing is the
standard shape for this kind of constraint in real-time physics
generally, not a one-off decision specific to this project.

### 31. Hierarchical (coarse + fine) GPU culling

Closes the item `docs/roadmap.md` had carried since Phase 5/6 under "Open
/ not yet started": *"Multi-pass / hierarchical culling — current design
is a flat 343-thread scan; fine at this scale, would need a coarser first
pass at much higher instance counts."* `culling.comp` (the fine pass) now
runs behind a new coarse pass, `cullingCoarse.comp`, that tests
cluster-level bounding volumes first and lets the fine pass skip its
per-object plane tests for any cluster already proven fully outside a
frustum.

**Clustering.** The 343-instance grid (`GRID_SIZE=7`) is grouped into
`CLUSTER_DIM=2`-cell clusters, `CLUSTERS_PER_AXIS =
ceil(GRID_SIZE/CLUSTER_DIM) = 4`, `CLUSTER_COUNT = 64`. Membership is
static and index-derived, not spatial/proximity-based: instance `idx`
(generated as `idx = x*49 + y*7 + z` in `initSceneData()`'s nested loop)
belongs to cluster `(x/2)*16 + (y/2)*4 + (z/2)`. This formula has to be
kept in sync in exactly two places —
`VulkanContext::clusterIndexForInstance()` (CPU) and `culling.comp`'s
GLSL recovery of `(x,y,z)` from `idx` — the same "no shared C++/GLSL
header exists, constants are manually mirrored" discipline every other
shader constant in this codebase already relies on (LOD distances,
`ShadowPushConstants`' layout, etc).

**Why CPU-computed cluster bounds, re-uploaded every frame, instead of a
GPU reduction pass.** `objectBuffer_` already established the pattern
this reuses: `updateInstanceSimulation()` recomputes CPU-side state every
frame (positions, velocities) and re-uploads the SSBO, because the
scatter/collision simulation itself is CPU-side (§20/§21/§30). Cluster
bounding spheres are a cheap O(2×343) aggregation (accumulate a mean per
cluster, then a max reach per cluster) added to the same function, right
next to the existing `objectBuffer_.upload(...)` — negligible next to the
O(n²) ≈ 58,653-pair mutual-collision scan that function already runs
every frame. A GPU reduction pass would have meant a third dispatch and a
second compute→compute barrier for a workload this small; not worth it
at this scale, and the CPU already touches every instance's position this
frame regardless.

**Correctness — the coarse gate is provably lossless, not just
empirically fine.** Each cluster's bounding sphere is built so it
*strictly contains* every member's sphere: `radius = max over
members(dist(clusterCenter, memberCenter) + memberRadius)`. A frustum
plane test is 1-Lipschitz (`f(x) = dot(n,x)+d`, `n` unit length), so
containment gives `|memberCenter - clusterCenter| ≤ clusterRadius -
memberRadius`. If the cluster fails a plane
(`f(clusterCenter) < -clusterRadius`), then for any member:
```
f(memberCenter) ≤ f(clusterCenter) + |memberCenter - clusterCenter|
                 < -clusterRadius + (clusterRadius - memberRadius)
                 = -memberRadius
```
— i.e. the member fails that same plane too. So gating the fine
per-object test behind the coarse cluster result can only skip objects
that would have been culled anyway; it can never produce a different
final visible set than the original flat scan. This is the actual
interview-relevant claim for this feature (*"does the coarse pass ever
change the result?"* — no, by construction), not just "it looked right
when I tested it."

**Two independent frustums, gated independently.** Like the existing
fine pass, the coarse pass tests every cluster against *both* the
camera's frustum and the light's orthographic frustum (§28), writing two
separate flag buffers (`ClusterVisibleCamera`/`ClusterVisibleLight`,
bindings 12/13). The fine pass's two gates are applied independently —
`if (!clusterCamOk) return;` only short-circuits the camera/LOD path,
while the light-frustum block is `if (clusterLightOk && sphereInside...)`
— because an object can be light-visible/camera-invisible or vice versa;
combining them into one early return would silently drop valid shadow
casters that are off-camera, the same class of bug §28 already had to
reason carefully about when it first added the independent light-frustum
test.

**The new compute→compute barrier.** `GPUCullingPass` now records two
dispatches — coarse (`(1,1,1)`, `CLUSTER_COUNT==local_size_x==64`, one
workgroup covers every cluster exactly) then fine
(`(OBJECT_COUNT+63)/64` as before) — with a hand-written
`VkMemoryBarrier` (`SHADER_WRITE→SHADER_READ`,
`COMPUTE_SHADER→COMPUTE_SHADER`) between them, since the fine pass reads
the flag buffers the coarse pass just wrote. This is the first
compute→compute barrier in the codebase (every prior barrier in
`drawFrame()` is compute→graphics or graphics→graphics), but the same
blanket, hand-written-at-the-point-of-need shape as all the others —
`FrameGraph` has no automatic per-pass barrier insertion anywhere.
Without this barrier: a race, not a validation error — the fine pass
could read stale or undefined flag values, producing silent
nondeterministic over- or under-culling that would be very easy to miss
on a fast GPU where the race usually resolves "correctly" by luck.

**Descriptor set: one, shared by both pipelines, not two.** Coarse and
fine both read the same camera/light frustum UBOs (bindings 7/10) and
touch the same cluster-flag buffers (12/13, one writes what the other
reads) — unlike `descriptor_`/`projectileDescriptor_`, which are two
*separate* sets because the grid and the projectile need two genuinely
different UBO values bound at the same time in one frame, there's no
such conflict here. `ComputeDescriptor` grew 11→14 bindings;
`VulkanComputePipeline::create()` grew a `shaderPath` parameter
(defaulted to the original fine shader, so the one pre-existing call site
kept compiling unchanged) so a second `VulkanComputePipeline` instance
(`computePipelineCoarse_`) could point at `cullingCoarse.comp.spv` while
reusing the identical descriptor set layout.

**Honest performance note.** At 343 instances / 6 fine workgroups, this
does not move the needle — the coarse pass itself costs one more tiny
dispatch plus a barrier, and the fine pass's per-object plane test it
sometimes skips was already cheap. The value here is closing the
roadmap's explicitly named architectural gap and demonstrating the
two-stage GPU-driven culling pattern correctly at a scale small enough to
verify by eye, consistent with how this project already treats several
other features (placeholder PBR textures, a single LOD threshold pair
shared across every mesh regardless of its detail level - §32) as "prove
the mechanism works, be honest that it's not tuned/scaled for production
numbers."

**Accepted limitation.** Cluster membership is static/index-based, not
re-clustered by proximity. A projectile blast that scatters instances far
from their original grid neighbors (§20) can make a cluster's bounding
sphere balloon toward covering most of the scene, since a cluster's
members are no longer physically close together — correctness still
holds (the containment proof above doesn't depend on spatial locality),
but the coarse pass's *rejection efficacy* degrades the more scattered
the grid gets. Not fixed here — an accepted tradeoff, same "documented
gap, not a silent one" bar as the LOD threshold pair not accounting for
per-mesh detail level (Phase 12/14, §32) or LOD1/LOD2's missing UV data
(Phase 8).

**Verification.** With the camera at a fixed position, "Total visible"
counts and the rendered image are unchanged from before this change
(confirms the gate is lossless in practice, matching the proof above).
Moving the camera until an entire edge cluster leaves the frustum drops
the new "Clusters visible (camera)" ImGui stat by 1 and "Total visible"
by that cluster's member count in the same frame — the same "make the
culling behavior visible and interactively verifiable" bar established
in Phase 5's oscillation test and reused in Phase 6/9/12, just applied
one level up at cluster granularity.

### 32. LOD thresholds: from flat world-space distance to screen-space projected size

§26 (Phase 12) made `culling.comp`'s `LOD1_DIST`/`LOD2_DIST` runtime-
tunable data instead of `const float` shader constants, but left their
*meaning* unchanged — a flat world-space distance, compared against raw
`camDist = length(center - cameraPos)`. That section's own "Not yet
done" note already named the gap: *"not derived from mesh detail or
screen-space size."* This section closes the screen-space half of that.

**Why a flat distance threshold isn't actually the right quantity.** The
thing that should decide whether an object needs its highest-detail mesh
is how large it *appears* — its projected size in pixels — not its raw
distance from the camera. Two scenes with the same distance threshold
but different vertical FOV (a wider FOV makes everything look smaller at
the same distance) or different output resolution (more pixels means the
same apparent size covers more of them) would classify the same physical
scene differently under the old metric, purely because the threshold
never accounted for either factor. A screen-space threshold is invariant
to both, which is exactly why real GPU-driven LOD systems (Nanite-style
cluster LOD, id Tech's virtual texturing/geometry systems, and most
production engines' distance-based LOD in general) use a projected-size
or "screen-space error" metric rather than raw distance.

**The math — small-angle approximation, not an exact projection.** A
bounding sphere of radius `r` at distance `d` from the camera, viewed
through a perspective projection with vertical FOV `fovY`, subtends a
half-angle of `atan(r/d)`. For the small angles this project's objects
actually subtend (grid spacing 3.0, `boundingSphereRadius_` well under
that, viewed from typical camera distances well outside contact range),
`atan(r/d) ≈ r/d` is an accurate enough approximation, and converting
that angular size to pixels against a viewport of height `H` gives:

```
screenSize (px) ≈ r * (H / (2*tan(fovY/2))) / d
                 = r * screenScale / d
```

`screenScale = H / (2*tan(fovY/2))` is a per-frame constant (H and fovY
don't change at runtime in this project — no live-resize, see "Dockable
viewport" in architecture.md) computed fresh each frame in
`GPUCullingPass` anyway, matching the existing "just recompute it, don't
special-case caching a value that's cheap to derive" discipline this
codebase already applies to the frustum planes themselves. `H` is
`VulkanSceneColorTarget::HEIGHT` (720) — the *offscreen scene render
target's* height, not the swapchain's or the ImGui panel's displayed
size, since that's what's actually rasterized; `fovY` is
`Camera::FOV_DEGREES`, made `public` for this (previously private) so
`FrameRenderer.cpp` can read the same single source of truth
`Camera::getProjectionMatrix()` already uses, rather than a second,
possibly-drifting copy of the FOV constant.

**No new buffer, no new binding.** `FrustumPlanes::lodDistances` was
renamed to `lodParams` and repurposed in place: `x`/`y` are now the
LOD1/LOD2 screen-size thresholds (pixels) instead of distances, and the
previously-unused `z` component now carries `screenScale`. `w` stays
unused. This keeps the struct at exactly 128 bytes (the existing
`static_assert`), so no descriptor/binding changes were needed anywhere
— `culling.comp` already had this data plumbed to it every frame, only
its *meaning* and the comparison shader-side changed.

**The inverted invariant.** `VulkanContext::lod1Distance()`/
`lod2Distance()` (and their setters, which enforced `lod2Distance_ >=
lod1Distance_`) became `lod1ScreenSize()`/`lod2ScreenSize()`, now
enforcing the *opposite* ordering: `lod1ScreenSize_ >= lod2ScreenSize_`.
This isn't an arbitrary flip — screen size is inversely related to
distance (closer = bigger on screen), so the LOD0/LOD1 boundary (which
happens at a *closer* distance than the LOD1/LOD2 boundary) now happens
at a *larger* screen-size threshold than the LOD1/LOD2 boundary. Getting
this backwards would have silently inverted the LOD selection (distant
objects rendering at full detail, near ones at the lowest), a bug that
would only show up as "why does everything look wrong now" rather than a
crash or validation error — worth calling out explicitly since it's the
easiest mistake to make porting a distance-based invariant to a
size-based one without re-deriving which direction it should go.

**Interview-relevant:** *"Why not just divide the old distance
thresholds by some FOV/resolution-dependent constant instead of
reworking the shader math?"* — because the *shape* of the relationship
is what's wrong, not just the scale. Distance and screen size aren't
linearly related (`screenSize ∝ 1/distance`), so no single constant
factor converts one threshold scheme into the other correctly across the
full range of distances objects can be at; the comparison itself has to
move from `camDist < threshold` to `screenSize > threshold`, not just
have its right-hand side rescaled.

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
| `stbi_load` returned `nullptr`, "unknown image type" | The specific PNG file used for testing had some non-standard encoding stb_image couldn't parse — confirmed by checking `stbi_failure_reason()` rather than guessing | Always surface the library's own error reason before assuming the bug is in your code; a generic "load failed" could mean a dozen different things, the library usually already knows which one |
| Texture loaded successfully but Suzanne rendered with no visible texture (looked like a flat color) | `suzanne.obj`'s vertex data had no texcoord entries at all — confirmed by instrumenting the loader to count vertices with `uv == (0,0)`, which came back 507/507 | Don't assume a downloaded test asset has every vertex attribute you need; verify with a count, not a glance. Isolated the texture *pipeline* from the *asset* problem by switching to a hand-written cube OBJ with explicit `vt` coordinates — this confirmed the Vulkan-side code (descriptor, shader, layout transitions) was correct *before* spending more time on the unrelated asset issue |
| New/modified files in `assets/` weren't reflected in the running executable despite a successful rebuild | CMake's `add_custom_command(... COMMAND copy_directory ...)` does not reliably re-copy a file whose name is unchanged but whose *content* changed — incremental build tracking treated the POST_BUILD step as already satisfied | Had to manually delete or `Copy-Item -Force` the stale file in the output directory to force a refresh. This is a real caveat of `copy_directory`-based asset pipelines, not a one-off fluke — worth remembering before re-debugging "my code change isn't taking effect" when the actual change was to a *data* file, not a source file |
| Shadow pass's `vkCmdBindVertexBuffers` reused `objectBuffer_` without the right usage flag | Buffer was created with only `STORAGE_BUFFER_BIT` (culling.comp's SSBO), never `VERTEX_BUFFER_BIT` — reusing a buffer for a new purpose doesn't retroactively grant it that purpose's usage flag | Caught by re-checking the buffer's creation call, not by an observed validation message — worth doing that check *before* reusing a buffer cross-purpose, since some drivers won't complain even though it's invalid per spec (§22) |
| Shadow map showed roughly half the grid missing/wrong depending on light angle | `glm::ortho()` defaults to OpenGL's `z_ndc ∈ [-1,1]` (this project never defines `GLM_FORCE_DEPTH_ZERO_TO_ONE`); for an orthographic matrix that linearly clips away the near half of the frustum under Vulkan's `[0,1]` requirement | `Camera`'s existing `glm::perspective()` usage had the same convention mismatch but hid it (only a near-plane sliver is affected for perspective); switched to `glm::orthoRH_ZO()` for the light matrix specifically (§22) — the perspective/orthographic distinction changes whether this bug is invisible or scene-breaking |
| Shadows appeared on grid instances with nothing actually occluding them, worse the longer the app had been running | `shadow.vert` transformed vertices with only the raw mesh-local `inPosition` — it never applied `ubo.model` (the grid's continuously-accumulating spin rotation) the way `triangle.vert` does, so the shadow map was permanently cast from each mesh's un-rotated *rest pose* while the visible geometry kept spinning independently every frame | Reported as "not sure if it's an algorithm bug or an aliasing artifact"; the deciding clue was that it was pixel-*coherent* (a stale silhouette-shaped mismatch, confirmed by asking whether it tracked with spin over time), not pixel-*noisy* like the earlier acne bug — same failure category ("shadow doesn't match the caster") but a different mechanism, worth distinguishing before reaching for a bias/PCF fix again. Fixed by giving `ShadowPushConstants` a second `mat4 model` field and pushing the same per-draw rotation (grid: `spinAngle()`; projectile: identity) that `GeometryPass` already computes — 128 bytes total, exactly Vulkan's guaranteed minimum push-constant size |
| `app.exe` printed `[Texture] stbi error: can't fopen` then aborted (`Debug Error! abort() has been called`) when launched directly from `build/bin/Debug/` | `CMAKE_RUNTIME_OUTPUT_DIRECTORY` is `build/bin`, so the `assets/`/`shaders/compiled/` POST_BUILD copies land in `build/bin/`, one level *above* where MSVC's multi-config generator actually places the executable (`build/bin/Debug/app.exe`, already called out in every doc's build section) — relative paths like `"assets/test_texture.png"` don't resolve from that directory. The texture failure alone is survivable (`stbi` logs and returns); the actual `abort()` is `ShaderLoader` throwing `std::runtime_error` for the equally-missing `shaders/compiled/*.spv` with nothing in `main()` to catch it, so it reaches `std::terminate()` | Always run the executable with `build/bin` as the working directory (e.g. `build/bin/Debug/app.exe` launched *from* `build/bin`), not from inside the `Debug/` subfolder it actually lives in — a distinction every doc's build section already states for a different reason (locating the binary) but doesn't spell out for *running* it |
| After adding derivative-based normal mapping, the *entire* scene rendered solid black — including LOD1/LOD2 meshes the change never touched | `dFdx(uv)`/`dFdy(uv)` are exactly `(0,0)` for any mesh with no real UV variation (LOD1/LOD2's `ObjLoader`-fallback constant `uv=(0,0)`), collapsing the reconstructed tangent/bitangent to the zero vector; `inversesqrt(0)` is `+Inf`, and `vec3(0) * Inf` is `NaN` per IEEE 754, which then poisons every subsequent value derived from it, in every draw sharing the pipeline | Verified by actually launching the app rather than re-reading the shader math (which looks correct for any mesh with real UV variation and gives no hint of the failure). The default camera position never brings any instance within LOD0 range, so the very first test run only ever exercised the degenerate-UV path — worth remembering that "the change I made should only affect X" is a claim to verify against what's actually on screen, not assume from which mesh the change targeted (§25) |
| A newly added `assets/suzanne_pbr.obj` (Phase 8 milestone 2) never showed up in `git status` after `curl`-ing it into place - investigating turned out to be much bigger than one missing file | `.gitignore`'s "Compiled objects" section had a bare `*.obj` rule intended for MSVC compiler object files. It also matches Wavefront OBJ mesh assets, and `assets/*.obj` was never exempted - `git ls-files assets/` showed only the `.mtl` files and `test_texture.png` were ever actually tracked. **`suzanne.obj`, `suzanne_lod1.obj`, `suzanne_lod2.obj`, and `textured_cube.obj` had never been committed at all**, on any commit up to this one - every one of this project's demo meshes, the whole time. A fresh `git clone` would build successfully (submodules + CMake don't need the assets) but `ObjLoader::load()` would throw at startup on the first missing file. `build/` (already ignored above the `*.obj` line) already covers every actual compiler `.obj` in this project, making the rule pure redundant risk, not a needed one | Caught by noticing the new file was absent from `git status --short` right before staging, not by an error message - a silently-ignored file produces no warning, and the working tree builds and runs fine regardless of git-tracking status since the files are still physically on disk. Only a fresh clone would have surfaced this. Removed the `*.obj` line entirely and committed the meshes; `*.o`/`*.lo`/`*.slo` (real object-file extensions with no asset-format collision here) stayed ignored |
| The offscreen scene target's aspect ratio silently didn't match the camera's projection matrix, for the first several commits of the dockable-viewport feature (§24) | `VulkanSceneColorTarget::HEIGHT` was `1024`, not `720` - a typo made while writing the class, never `1280×720` as every design note (including this document's own §24 writeup) already claimed. `Camera::ASPECT_RATIO` is a separate hardcoded `1280.0f/720.0f` constant in a different file, with nothing anywhere cross-checking the two against each other or against the GLFW window's actual creation size (`glfwCreateWindow(1280, 720, ...)` in `Application.cpp`) | Found doing a routine "recheck the docs against the code" pass, not by looking for this specific bug - grepped every doc's stated `1280×1024` resolution and cross-referenced it against `Camera::ASPECT_RATIO` and the actual `glfwCreateWindow` call, which is where the mismatch became obvious. A useful reminder that a doc describing a wrong value *consistently* reads as confirmation, not a red flag, unless it's actually checked against the code and not just against itself. Fixed the constant to `720`; nothing else needed to change since `sceneColorTarget_.extent()` is already read dynamically everywhere it's used |
| Left-click-to-fire intermittently didn't register right after releasing Ctrl - reliably fixed by a big mouse-look swing, reported by the user with the correct suspicion ("UI region vs. cursor region conflict") | `imgui_impl_glfw.cpp` keeps feeding GLFW's unbounded `GLFW_CURSOR_DISABLED` virtual position into `io.MousePos` (confirmed in its own changelog - it deliberately does not ignore mouse data in that mode) even though that position isn't real screen coordinates; `WantCaptureMouse` hit-tests it against ImGui window rects anyway. Since Phase 11 made the entire client area docked ImGui windows, the residual position from right before Ctrl release is almost always still "inside some window," keeping `WantCaptureMouse` stuck `true` until enough mouse-look movement drags it back outside every window's rect (§27) | User-reported, with the root cause already correctly guessed before investigation started. Fixed per ImGui's own recommended remedy: toggle `ImGuiConfigFlags_NoMouse` in sync with `Camera::cursorVisible()` every frame, so ImGui ignores mouse input outright while the cursor is captured, rather than patching only the one call site (`Application.cpp`'s click-fire gate) that happened to surface the symptom |

---

## Open items / known simplifications

- **Texture-based PBR materials are implemented** (§25) — a `Material`
  class, albedo/normal/metallic-roughness/AO all sampled in
  `triangle.frag`. Remaining gaps, both deliberate: only LOD0 has real UV
  data (LOD1/LOD2 sample a constant texel, same as before this
  milestone); the texture maps themselves are small self-generated
  placeholders (flat normal, a metallic/roughness gradient, flat AO), not
  authored/downloaded PBR photo sets. Material params are still shared by
  all 343 grid instances (the projectile gets its own distinct push-
  constant values, but still one flat set) — true per-instance material
  variation is future work.
- **No IBL/environment lighting** — the ambient term is a flat
  `0.03 * albedo` constant, not derived from any environment map. Faces
  fully turned away from the single directional light are nearly black
  except for this flat term.
- **Shadow mapping is implemented** (§22) for the single directional
  light — depth-only `ShadowPass`, 3×3 PCF, tunable bias, light-frustum
  culling as of §28 (GPU-culled indirect draw, same shared
  `culling.comp` dispatch as the camera path), and a scatter-aware scene
  radius as of §29 (`lightViewProj()`'s frustum size now grows with a
  projectile blast instead of being a fixed constant). Remaining gap:
  the scene-radius computation only considers grid instances, not the
  projectile's own position (§29) — a deliberate, accepted omission, not
  an oversight.
- **LOD is implemented** (§15) as 3 buckets, runtime-tunable since §26
  (data, not a shader constant) and screen-space-projected-size-based
  since §32 (default `lod1ScreenSize_ = 120px`, `lod2ScreenSize_ = 60px`,
  not a flat world-space distance) but still not derived from per-mesh
  detail level - one threshold pair shared across all LOD transitions
  regardless of how detailed each specific mesh actually is. A bucket
  with 0 instances this frame still costs a full
  `vkCmdDrawIndexedIndirect` call — fine at 3 LOD levels, would need
  revisiting (e.g. skip empty buckets, or a 4th "culled entirely" bucket
  merge) if the LOD count grows.
- **Single compute dispatch covers all 343 instances** with no
  multi-pass culling hierarchy (e.g. coarse cell-based culling before
  per-object testing). Acceptable at this instance count; would need
  revisiting at much higher instance counts where the linear scan itself
  becomes the bottleneck.
- **The dockable Viewport target is fixed-resolution** (§24) — resizing
  the ImGui panel scales the existing 1280×720 image rather than
  re-rendering at a new resolution; would need swapchain-style resize
  handling this project has never needed anywhere else.
- **Texture sampling is implemented and validated** (#13), and reunited
  with the primary demo mesh for LOD0 as of Phase 8 milestone 2 (§25) —
  `assets/suzanne_pbr.obj` has real `vt` data, unlike the original
  `suzanne.obj`. LOD1/LOD2 (`suzanne_lod1.obj`/`suzanne_lod2.obj`) still
  have none and sample a constant `(0,0)` texel — see §25's scoping
  decision for why, and `docs/roadmap.md`'s Open items for the tracked gap.

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

  