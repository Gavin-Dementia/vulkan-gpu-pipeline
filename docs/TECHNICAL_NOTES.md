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
                ├── GPUCullingPass   [Compute] — coarse per-cluster
                │                     frustum test, then fine per-object
                │                     frustum test + screen-space-size LOD
                │                     fan-out (see §31, §32)
                ├── ShadowPass       [Shadow, own render pass] — depth-only,
                │                     draws all instances from the light's
                │                     view (see §22)
                ├── GeometryPass     [Graphics, depends on CullingPass +
                │                     ShadowPass] — skybox draw (see §33)
                │                     + 1 indexed-indirect draw per LOD,
                │                     samples the shadow map; renders to
                │                     the offscreen scene target, not the
                │                     swapchain directly (see §24)
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
tolerance at this instance count, not a new gap this feature introduces
*at the time this was written*. **§37 revisits this**: once §36 gave the
frame loop its first real mid-loop stall source (`vkDeviceWaitIdle` on a
viewport resize), the "large `deltaTime`" case stopped being purely
theoretical. **§41 closes it**: the hit test is now a swept segment
test, not a point check.

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
`docs/roadmap.md`'s Open items, not treated as a defect. **Later
revisited and closed — see §36**, once this project's pipelines/render
targets had enough established "recompute fresh, don't cache" precedent
(frustum planes, `lightViewProj()`) that adding live-resize plumbing
stopped being "solving a problem this project doesn't otherwise have."

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

### 33. Image-Based Lighting, Milestone 1: cubemap infrastructure + procedural sky

`docs/roadmap.md`'s "Open / not yet started" list named this gap
explicitly: *"IBL / environment lighting — current ambient term is a flat
`0.03 * albedo` constant."* Full IBL (diffuse irradiance convolution +
specular prefilter/BRDF LUT) was requested but explicitly staged, not
built in one pass. **This section covers Milestone 1 only: cubemap
infrastructure, a procedurally baked sky, and a visible skybox — the
`0.03 * finalAlbedo * ao` ambient term in `triangle.frag` is not touched
by this milestone at all.** Milestones 2/3 will read this same cubemap
to actually replace it.

**Why procedural, not a loaded HDR file.** Phase 8 sourced a real
external asset (`suzanne_pbr.obj`) when a functional gap (no UV data)
made a placeholder inadequate, with explicit attribution. Here the
environment's *content* doesn't matter for proving the cubemap/
convolution mechanism works — a procedural gradient sky is a legitimate,
self-contained source (no licensing question, no network dependency, no
`stbi_loadf`/HDR-file-format code path needed yet) that exercises exactly
the same infrastructure a photographed HDRI would. `envCapture.frag`
computes a `mix(horizon, zenith)` gradient plus a bright sun disk aligned
with `-lightDirection_` — the same directional light the rest of the
scene already uses, not an independent, driftable second light source.

**Why `inverse(viewProj)` for direction reconstruction, not hand-derived
basis vectors.** Both the bake capture and the live skybox reconstruct a
per-pixel world-space ray direction the same way:
`worldPos = invViewProj * vec4(ndc, 1, 1); worldPos /= worldPos.w; dir =
normalize(worldPos.xyz - eyePos)`. This is the exact algebraic inverse of
the transform that placed the fullscreen triangle in clip space, so it's
self-consistent by construction — there's no separate set of
right/up/forward basis vectors that could drift out of sync with the
projection matrix's own convention (this project's `proj[1][1] *= -1`
Vulkan Y-flip, applied identically here to the 6 capture-face
projections via the same convention `Camera::getProjectionMatrix()`
uses). A hand-derived per-face basis (`dir = forward + ndc.x*right +
ndc.y*up`) was the original design; it was replaced specifically because
a sign error in 6 manually-transcribed basis triples is a real,
easy-to-make, hard-to-spot-by-reading-code risk (it would show up
subtly, as a mirrored or rotated sky, not a crash) — `inverse(viewProj)`
eliminates that entire risk category rather than requiring it to be
gotten right by careful derivation.

**The 6-face capture table.** Six real `glm::lookAt` view matrices, one
per cube face, using the standard capture-direction table (identical to
LearnOpenGL's IBL tutorial and Sascha Willems' Vulkan
`generateCubemaps()` — a proven, widely-shipped reference, not derived
from scratch):
```cpp
static const glm::vec3 kCaptureForward[6] = {
    { 1, 0, 0}, {-1, 0, 0}, { 0, 1, 0}, { 0,-1, 0}, { 0, 0, 1}, { 0, 0,-1}
};
static const glm::vec3 kCaptureUp[6] = {
    { 0,-1, 0}, { 0,-1, 0}, { 0, 0, 1}, { 0, 0,-1}, { 0,-1, 0}, { 0,-1, 0}
};
// face order matches Vulkan's cube array-layer convention: +X,-X,+Y,-Y,+Z,-Z
```
paired with a 90°-FOV projection carrying the same Y-flip
`Camera::getProjectionMatrix()` applies
(`captureProj[1][1] *= -1`), so a bounding-sphere-radius-style mistake
(reusing the *wrong* convention for a *different* matrix — the exact bug
class §22 already hit once for the light's orthographic matrix) doesn't
repeat here.

**Bake once, HDR raw; tonemap only in the live draw.**
`environmentCubemap_` is `VK_FORMAT_R16G16B16A16_SFLOAT` (HDR-capable —
the sun highlight is deliberately >1.0, values M2/M3's irradiance/
prefiltered cubemaps will also need this range for). `envCapture.frag`
writes raw, untonemapped color; `skybox.frag` applies the exact same
Reinhard tonemap (`color/(color+1)`) `triangle.frag`'s last line already
uses, so the skybox and the lit grid share one consistent dynamic-range
mapping despite being computed in two different shaders.

**Baking mechanics.** `VulkanContext::initEnvironment()` (called from
`init()` right after `initCore()`, before `initSceneData()`/
`initCullingResources()`) creates the persistent `environmentCubemap_`
(a new `VulkanCubemap` class — this project's first cube image; one
`VK_IMAGE_VIEW_TYPE_CUBE` view for sampling, 6 `VK_IMAGE_VIEW_TYPE_2D`
single-layer views for render-pass attachments, since Vulkan attaches 2D
views per subresource, not cube views), then does a **6-draw, one-shot
bake** using locally-scoped (created, used, destroyed within this one
function — not `VulkanContext` members, since they're never needed
again) render pass/framebuffer/pipeline. Reuses the exact one-shot
command buffer pattern `VulkanTexture::transitionLayout()`/
`copyBufferToImage()` already established for setup-time GPU work:
`ONE_TIME_SUBMIT_BIT`, one `vkQueueSubmit`+`vkQueueWaitIdle`, no fence.
One `VkImageMemoryBarrier` covering all 6 array layers
(`oldLayout==newLayout==SHADER_READ_ONLY_OPTIMAL`) orders the memory
access explicitly before the cube view is first sampled — the per-face
render passes' `finalLayout` already transitions each of the 6
subresources correctly (Vulkan tracks image layout per-subresource, not
per-view), but this barrier makes the write→read ordering explicit, the
same `oldLayout==newLayout`, "ordering only" shape `FrameRenderer.cpp`'s
`shadowBarrier`/`sceneBarrier` already use.

**Shared `fullscreenTriangle.vert`.** Both the bake pipeline
(`VulkanEnvCapturePipeline`) and the live skybox pipeline
(`VulkanSkyboxPipeline`) point at the same compiled `.vert` — a
deliberate reuse, since a `gl_VertexIndex`-driven fullscreen triangle has
nothing pipeline-specific about it, unlike this codebase's usual
one-`.vert`/`.frag`-pair-per-pipeline convention.

**The skybox draw itself needs no new `FrameGraph` pass.** It's recorded
inside `GeometryPass`'s existing lambda (`FrameRenderer.cpp`), first,
before the grid's pipeline bind — the same "just another draw call in
this lambda" precedent the projectile draw already established.
`depthTestEnable`/`depthWriteEnable` both `VK_FALSE`: the skybox always
draws (fullscreen triangle, nothing to discard) and never touches the
depth buffer, so the grid's own `LESS` test afterward runs against the
still-pristine 1.0-cleared depth buffer exactly as before this
milestone — no z-value trickery (e.g. authoring the triangle at exactly
the far plane and relying on a `LESS_OR_EQUAL` test) needed.

**Interview-relevant:** *"Why reconstruct the ray direction via
`inverse(viewProj)` instead of passing basis vectors as a push
constant?"* — single source of truth and correctness-by-construction.
Basis vectors would be a second, independently-computed representation
of the same camera/face orientation the projection matrix already
encodes; keeping both in sync is pure bookkeeping risk with no
compensating benefit (the inverse-matrix approach costs one
`glm::inverse()` per draw, negligible next to a full frame's work). This
mirrors a discipline already established elsewhere in this codebase —
`Camera::getViewMatrix()`/`getProjectionMatrix()` being the one shared
source for both the culling pass and the geometry pass (§10) — applied
to a new problem (ray-direction reconstruction) rather than a new
principle invented for this feature.

**Not done in this milestone (open for M2/M3):** the ambient term in
`triangle.frag` is unchanged (closed for the diffuse half by M2, §34,
right below); the environment is baked once at startup from whatever
`lightDirection_` is at that moment, so a live change to the light
direction via the "Lighting" ImGui window does not move the sun in the
skybox (re-baking on demand, e.g. a "Rebake Environment" button, is a
cheap follow-up but out of scope here — still true as of M2, which
inherits the same limitation for the irradiance term); `VulkanCubemap`
is single-mip only, which M3's specular prefilter will need to extend.

### 34. Image-Based Lighting, Milestone 2: diffuse irradiance convolution

Closes the diffuse half of the roadmap's still-open IBL gap. §33 built
the cubemap infrastructure and a procedural-sky skybox but explicitly
left `triangle.frag`'s ambient term untouched. This milestone bakes a
second, small cubemap — `irradianceCubemap_`, the cosine-weighted
hemisphere-convolved diffuse irradiance of `environmentCubemap_` — and
uses it to replace the diffuse half of that ambient term. **Specular
IBL (prefiltered environment + BRDF LUT split-sum) is explicitly not
part of this milestone** — that's M3, still open.

**The convolution formula — a trusted reference derivation, not
hand-derived.** Same discipline §33 already established for the 6-face
capture table: `irradianceConvolve.frag` is the standard cosine-weighted
Riemann-sum diffuse-irradiance integral (LearnOpenGL's IBL diffuse
irradiance article, itself the widely-cited reference derivation used
across real-time IBL implementations), not something re-derived from
scratch here. For a direction `N`, it builds an arbitrary tangent basis
around `N`, double-loops `phi ∈ [0,2π)` / `theta ∈ [0,π/2)` at
`sampleDelta=0.025`, and accumulates
`texture(environmentMap, sampleVec).rgb * cos(theta) * sin(theta)`
before normalizing by `π / nrSamples`. The `cos(theta)` factor is the
Lambertian cosine weighting (grazing-angle incident light contributes
less to diffuse reflection); the extra `sin(theta)` factor is the
solid-angle Jacobian for spherical coordinates — without it, samples
near the pole (`theta≈0`) would be over-weighted relative to samples
near the horizon, since a fixed `(dphi, dtheta)` step covers a smaller
solid angle near the pole than near the equator.

**Why `initEnvironment()`'s call site had to move into `initCore()` —
not a cosmetic refactor.** §33's `initEnvironment()` ran entirely after
`initCore()` returned, because nothing inside `initCore()` depended on
anything it produced. That's no longer true: `pipeline_.create()`
(inside `initCore()`) now needs `irradianceDescriptor_.layout()` as a
second descriptor-set-layout argument (see below), and
`irradianceDescriptor_` can't exist before the entire bake — environment
capture → barrier → irradiance convolution → barrier — completes. Since
`initEnvironment()`'s actual prerequisites (`sceneRenderPass_`/
`sceneColorTarget_`, needed only for the persistent `skyboxPipeline_`
built at the very end of the function) are already satisfied right after
`sceneFramebuffer_.create()` — earlier in `initCore()` than
`pipeline_.create()` — the fix is to call `initEnvironment()` from
*inside* `initCore()`, at that point, rather than as a separate
post-`initCore()` step. A one-line-looking call-site move, but one with
a real dependency reason behind it, not a tidiness pass.

**One command buffer, two bakes.** The single `VkImageMemoryBarrier` that
used to sit at the very end of `initEnvironment()` (M1) moved to right
after the 6 environment-capture draws instead. A `vkCmdPipelineBarrier`
recorded mid-command-buffer creates an execution+memory dependency
ordering everything before it against everything after it *within that
command buffer's own submission order* — exactly what lets the 6
irradiance-convolution draws, later in the *same* command buffer, safely
sample `environmentCubemap_` with no second `vkQueueSubmit`/
`vkQueueWaitIdle` round-trip. A second, identically-shaped barrier for
`irradianceCubemap_`'s 6 layers still goes right before
`vkEndCommandBuffer`, matching M1's "explicit write→read ordering"
precedent (not strictly load-bearing here, since `irradianceDescriptor_`
is only built host-side after `vkQueueWaitIdle` returns — kept anyway for
consistency). Both bakes also share one `VulkanRenderPass` object
(`createColorOnly(R16G16B16A16_SFLOAT)`) — a `VkRenderPass` encodes
attachment format/structure only, not extent, so the same object
legitimately backs both the 512² environment bake's framebuffer and the
32² irradiance bake's framebuffer.

**`CubeSamplerDescriptor` — renamed from `SkyboxDescriptor`.** Its shape
(one `COMBINED_IMAGE_SAMPLER` binding, `create(device, cubeView,
cubeSampler)`) was already completely generic in M1; this milestone
reuses the exact same instance (`skyboxDescriptor_`) as the irradiance
bake's input binding to `environmentCubemap_` (same layout, same bound
resource, no reason to allocate a second set for the same thing) and
adds one new instance (`irradianceDescriptor_`, bound to
`irradianceCubemap_`) as the main graphics pipeline's new set 1. The
"Skybox"-specific name stopped fitting once it wasn't skybox-exclusive —
fixed now rather than letting the mismatch compound with a third use
site later.

**Two-set pipeline layout — this codebase's first.** `VulkanPipeline`
was hardcoded to exactly one descriptor set since Phase 3. It now takes
two: set 0 (`descriptor_`/`projectileDescriptor_`'s shared layout shape —
per-object material data) and set 1 (`irradianceDescriptor_` — ambient
lighting data, identical for every material since it's scene-wide, not
per-object). `VulkanDescriptor` itself needed zero changes — growing it
to hold IBL data would have meant `descriptor_.create()` (which runs
early in `initCore()`, before `sceneRenderPass_`/environment baking are
ready) depending on something not yet available, the same ordering
problem `initEnvironment()`'s call-site move above had to solve, avoided
entirely by keeping IBL data in its own set instead. Bound once per frame
in `FrameRenderer.cpp`'s `GeometryPass` (`firstSet=1`, before the
existing `firstSet=0` bind) and left bound for the rest of the pass —
per Vulkan's descriptor-set binding-persistence rule, binding only set 0
later (for the grid→projectile switch) doesn't unbind an
already-bound, layout-compatible set 1.

**Push constant reuse, not a third struct.** `irradianceConvolve.frag`
needs exactly `invViewProj` + `cameraPos` (origin) — identical to what
the live skybox already uses, and identical to how the env-capture bake
already sets its own `cameraPos = vec4(0,0,0,0)`. `SkyCapturePushConstants`
only diverges from `SkyboxPushConstants` because the procedural sky needs
*extra* data (`sunDirAndCos`); this shader needs nothing extra, so by that
same "diverge only when data needs differ" pattern, `VulkanIrradiancePipeline`
reuses `SkyboxPushConstants` directly rather than introducing a
byte-identical third struct.

**Interview-relevant:** *"Why is this a one-time startup bake and not
computed per-frame or on demand?"* — diffuse irradiance from a static
environment is itself static; recomputing it every frame would mean
paying the full convolution cost (see below) 60+ times a second for a
result that never changes unless the environment itself changes. A
future "live re-bake on light-direction change" feature (still out of
scope — same limitation §33 already named for the skybox) would trigger
this same one-shot bake path on demand, not turn it into a per-frame cost.

**Honest cost note, with a real number.** At `sampleDelta=0.025`: the
`phi` loop runs `⌈2π/0.025⌉ = 252` iterations, the `theta` loop
`⌈(π/2)/0.025⌉ = 63` iterations — **15,876 environment-map samples per
output texel**. `irradianceCubemap_` is 6 faces × 32×32 = 6,144 texels,
so the whole bake is **≈97.5 million texture samples**, a real,
nontrivial cost compared to M1's near-instant procedural bake (which did
*zero* texture reads — a pure analytic function). Still a one-time
startup cost, not a per-frame one, and expected to complete in low tens
of milliseconds on any GPU capable of running this renderer at all — but
this wasn't empirically profiled as part of this milestone, and would be
worth measuring directly if `initEnvironment()`'s total startup time ever
becomes user-visible.

**Not done in this milestone:** specular IBL (M3 — prefiltered
environment mip chain + BRDF LUT split-sum, closed right below in §35)
was still unimplemented at the time this section was written; the
no-live-rebake-on-light-change limitation (§33) applies to the
irradiance term too, not just the skybox, and — per §35 — to the
prefiltered/BRDF-LUT terms as well.

### 35. Image-Based Lighting, Milestone 3: specular prefilter + BRDF LUT

The final IBL milestone. §33 built cubemap infrastructure + a
procedural-sky skybox; §34 added diffuse irradiance convolution but
explicitly left the specular half of the ambient term uncomputed. This
milestone closes it with Karis's split-sum approximation (2013, "Real
Shading in Unreal Engine 4") — two new baked assets: `prefilteredCubemap_`
(a mip-chain cubemap, each mip storing the environment convolved with a
GGX-importance-sampled specular lobe for that mip's roughness) and
`brdfLut_` (a 2D texture, not a cubemap, storing the precomputed second
half of the split-sum integral, indexed by `(NdotV, roughness)`). This
closes the roadmap's IBL gap entirely — nothing IBL-specific stays open
after this milestone except the pre-existing, unrelated no-live-rebake
limitation (§33).

**The split-sum approximation, briefly.** The full specular IBL integral
(pre-integrating the environment against the specular BRDF, per pixel,
per frame) is infeasible in real time. Karis's insight splits it into two
independently-precomputable pieces multiplied together at shading time:
(1) a *prefiltered* environment — the radiance you'd see reflected off a
surface of a given roughness, baked once into a mip chain (sharper mips
for smoother surfaces, blurrier mips for rougher ones); (2) a BRDF
integration LUT — a per-(NdotV,roughness) `(scale, bias)` pair,
independent of any environment content, baked once and reused forever.
`triangle.frag` combines them at shading time:
`specularIBL = prefilteredColor * (F * envBRDF.x + envBRDF.y)`. Neither
piece alone is the answer; the product approximates the true integral
closely enough to be the standard real-time technique, not an ad hoc
shortcut invented here — same "reuse a trusted formula" discipline §33/
§34 already established for the capture table and the cosine-weighted
convolution.

**The `minLod`/`maxLod` sampler fix — a real, silent bug caught before
it shipped.** `VulkanCubemap`'s sampler never set `minLod`/`maxLod`;
`VkSamplerCreateInfo{}` zero-initializes both to `0.0`, which per spec
clamps every sampled LOD into `[0,0]`. Harmless at `mipLevels=1` (M1/M2's
`environmentCubemap_`/`irradianceCubemap_`), but would have silently
forced every `textureLod()` sample of the new 5-mip
`prefilteredCubemap_` back to mip 0 regardless of the roughness-derived
level `triangle.frag` requests — no validation-layer complaint, just a
renderer that looks like it has zero roughness variation in its
reflections. Fixed by `minLod=0.0f, maxLod=mipLevels-1` in
`VulkanCubemap::create()`. Worth naming as its own decision: `mipmapMode`
was already `VK_SAMPLER_MIPMAP_MODE_LINEAR` from M1, silently inert at 1
mip, and only became load-bearing (enabling trilinear roughness
blending between adjacent mips) once this milestone gave it something to
interpolate.

**`VulkanCubemap` gains mip-chain support, kept backward-compatible.**
`create()` grew a defaulted `mipLevels = 1` parameter rather than a
required one, so `environmentCubemap_`/`irradianceCubemap_`'s existing
call sites needed zero changes. `faceView(int i)` became
`faceView(uint32_t face, uint32_t mip = 0)` for the same reason — the
defaulted second parameter keeps every existing single-argument call
site compiling. `faceViews_` internally became `[mip][face]`-indexed
(`std::vector<std::array<VkImageView,6>>`).

**`k_IBL` vs `k_direct` — a footgun called out explicitly, not left for
a future reader to trip over.** Karis's paper uses two *different*
remappings of the Schlick-GGX geometry term's `k` parameter: `k_direct =
(roughness+1)²/8` (tuned for analytic direct lights — already in
`triangle.frag`'s `geometrySchlickGGX`, used only by the direct `Lo`
term, untouched by this milestone) and `k_IBL = roughness²/2` (for the
Monte-Carlo-integrated ambient case). `shaders/brdfLUT.frag` gets its own
local `GeometrySchlickGGX_IBL`/`GeometrySmith_IBL` using `k_IBL`,
duplicated rather than shared (no cross-shader include mechanism in this
build) with a comment explicitly contrasting it against the direct-light
version. Using the wrong `k` here would have been a real, silent
correctness bug — plausible-looking but measurably wrong (too dark at
grazing angles) values, not a crash or validation error.

**Five static-viewport pipelines, not one dynamic-viewport pipeline.**
`grep -r VK_DYNAMIC_STATE` across this codebase's own source returns zero
hits — every pipeline class here bakes a static `VkViewport` at creation
time. Rather than introduce the first use of dynamic viewport state for
this one bake, `VulkanPrefilterPipeline` gets 5 short-lived instances,
one per mip level (each viewport-sized to that mip's face size:
128/64/32/16/8), created, used, and destroyed within `initEnvironment()`
— the same "locally-scoped bake resource" discipline M1/M2 already
established, just five of them instead of one.

**`IBLDescriptor` replaces `irradianceDescriptor_` as the main
pipeline's set 1 — 3 bindings, not 1.** `CubeSamplerDescriptor` (M2's
rename of `SkyboxDescriptor`) is deliberately fixed at exactly 1 binding
— making it variable-arity for this one 3-binding caller would have
complicated every existing call site (skybox draw, irradiance-bake
input, and now also the prefilter-bake input — all still 1-binding uses)
for no shared benefit. `IBLDescriptor` is a new class instead, bundling
irradiance + prefiltered + BRDF LUT into one set — `irradianceDescriptor_`
is fully removed, not kept alongside it.

**The roughness-aware Fresnel upgrade — fulfilling what §34 already
predicted, not scope creep.** M2's ambient term used plain
`fresnelSchlick(NdotV, F0)` as a deliberate staged simplification; §34's
own text named *"the roughness-aware Fresnel refinement affecting both
terms"* as still-open work. This milestone adds `fresnelSchlickRoughness`
(the standard `F0 + (max(vec3(1-roughness),F0)-F0) * pow(...)` variant)
and uses it for `kS_ambient`, reused by both the diffuse and the new
specular ambient term — a documented, intentional change to M2's
existing diffuse term's exact numeric look, not a regression.

**Interview-relevant:** *"Why does the BRDF LUT need no descriptor set
or push constant at all, when every other bake in this codebase needs at
least a push constant?"* — because `IntegrateBRDF(NdotV, roughness)` is
a pure function of its two inputs, both of which the fullscreen
triangle's own UV already encodes directly (`uv.x = NdotV, uv.y =
roughness`) — no direction reconstruction, no environment sampling, no
per-frame external state at all. Every *other* bake needs at least
`invViewProj`/`cameraPos` because it's evaluating a function of *world
direction*, which has to be reconstructed from a 2D fullscreen triangle;
the BRDF LUT sidesteps that reconstruction entirely by being indexed in
a space (`NdotV`, `roughness`) that's already flat 2D.

**Honest cost note — flagged more assertively than §34's hedge.**
Prefilter: 5 mips × 6 faces × `SAMPLE_COUNT=1024`, face sizes
128/64/32/16/8 → **≈134.1M texture samples** (mip 0 alone: 128²×6×1024 ≈
100.7M). BRDF LUT: 512×512×1024 ≈ 268M loop iterations — but *zero*
texture fetches, pure ALU (NDF/geometry-term math only). Combined with
§34's already-baked 97.5M irradiance samples, this milestone roughly
quadruples the texture-sampling work alone and adds a comparable-
magnitude ALU-bound pass on top of that — plausibly **low hundreds of
milliseconds** of total startup bake time (all four bakes: environment,
irradiance, prefilter, BRDF LUT — one combined command buffer, one
`vkQueueWaitIdle`). Unlike §34's "not profiled, probably fine" framing,
this crosses into "worth actually measuring" territory; it has not been
empirically profiled as part of this milestone either, but should be
treated as a known, real startup cost rather than assumed negligible.

**Remaining limitations, honestly stated.** No live re-bake on a
light-direction change — the skybox, irradiance, prefiltered specular,
and BRDF LUT (though the LUT itself doesn't depend on light direction at
all — see above) are all baked once from whatever `lightDirection_` is
at startup. `environmentCubemap_`/`irradianceCubemap_` remain single-mip;
only `prefilteredCubemap_` uses the mip-chain support this milestone
added to `VulkanCubemap`. `N=V=R` in the prefilter shader is a standard,
widely-shipped simplifying assumption (see `shaders/prefilterEnv.frag`'s
own comment for the reasoning), not a from-scratch shortcut, but it is
an approximation — grazing-angle reflections on rough surfaces are the
place it's most visibly imperfect.

### 36. Live-resized viewport target

Closes the roadmap's last remaining open item from the dockable-viewport
work (Phase 11, `TECHNICAL_NOTES.md` §24): the offscreen scene target
(`sceneColorTarget_`) was fixed at 1280×720, so resizing the docked
"Viewport" panel just scaled the existing image (`ImGui::Image()`
stretching a fixed-resolution texture) rather than re-rendering at the
panel's actual pixel size. This section makes it genuinely resizable.

**What actually needs to change when the target's size changes.**
Four things reference the scene target's dimensions, and each needed a
different fix:
1. **The color/depth images themselves** (`sceneColorTarget_`/
   `sceneColorDepth_`) — `VulkanSceneColorTarget::create()` grew
   `width`/`height` as required runtime parameters (previously baked in
   as `WIDTH`/`HEIGHT` compile-time constants); `extent()` now reports
   whatever size was last created, not a fixed value.
2. **The camera's projection matrix's aspect ratio** — `Camera::
   ASPECT_RATIO` was a `1280.0f/720.0f` compile-time constant.
   `getProjectionMatrix()` now takes `aspectRatio` as a required
   parameter; every call site (`GPUCullingPass`'s frustum construction,
   `GeometryPass`'s grid and projectile UBOs) recomputes it fresh each
   frame from `context->sceneColorTarget().extent()` — the same
   "recompute, don't cache" discipline this codebase already applies to
   frustum planes and `lightViewProj()`. Getting this wrong doesn't
   crash anything - it silently renders a stretched/squashed image,
   exactly the visual artifact this whole feature exists to fix.
3. **The screen-space LOD threshold's projection scale** (§32) —
   `frustum.lodParams.z` was derived from
   `VulkanSceneColorTarget::HEIGHT`, a compile-time constant that no
   longer exists once the target's size is a runtime value. Fixed by
   reading `context->sceneColorTarget().extent().height` fresh each
   frame instead, in the same `GPUCullingPass` lambda that already
   computes the new aspect ratio.
4. **Two pipelines' baked-in `VkViewport`/scissor** (`pipeline_`,
   `skyboxPipeline_`) — every pipeline class in this codebase bakes a
   static viewport at creation time (confirmed via `grep -r
   VK_DYNAMIC_STATE`, zero hits outside `third_party/` — see §35's same
   finding for the specular prefilter's 5 pipeline instances). A resize
   has to destroy and recreate both pipelines at the new extent; there's
   no cheaper "just update the viewport" path available without
   introducing dynamic viewport state as a new pattern, which this
   change deliberately didn't do.

**`VulkanContext::resizeSceneTarget(width, height)`** — destroys and
recreates `sceneFramebuffer_`/`pipeline_`/`skyboxPipeline_`/
`sceneColorDepth_`/`sceneColorTarget_` at the new size.
`sceneRenderPass_` itself is untouched: a `VkRenderPass` encodes
attachment format/structure only, not extent — the same fact IBL's bake
(§33) already relied on to reuse one render pass across several
differently-sized framebuffers. Clamps `width`/`height` to a 64px floor
itself (the single authoritative enforcement point — `ImGui::
GetContentRegionAvail()` can transiently report near-zero while a dock
panel is being torn down or rebuilt, and callers shouldn't each need to
know to guard against that). No-ops if the requested size already
matches the current one.

**Why `vkDeviceWaitIdle`, not a per-frame-slot fence wait.**
`sceneColorTarget_`/`sceneColorDepth_`/`pipeline_`/`skyboxPipeline_` are
single, shared instances — not duplicated per frame-in-flight the way
`FrameContext`'s command buffers/fences are. A per-slot fence wait only
proves *one* slot's prior GPU work is done; it says nothing about the
*other* slot, which could still be mid-flight reading the very images
about to be destroyed. Only a full device idle is provably sufficient
here. The accepted cost: a real, visible stall (a brief hitch) every
time the panel is resized, most noticeable while its border is being
actively dragged (see below) — traded deliberately for keeping this
resize path simple rather than introducing per-resource fencing this
codebase has no precedent for anywhere else.

**Why the resize is detected in `ImGuiPass` but applied at the top of
the *next* frame, not immediately.** By the time `ImGuiPass` runs (the
last stage of the frame, per `FrameGraph::PassStage::UI`), `GeometryPass`
has *already* recorded draws this frame against the current
`sceneFramebuffer_`/`pipeline_` into the command buffer. Destroying and
recreating those objects right then would invalidate a command buffer
still mid-recording (or already fully recorded, about to be submitted).
So `ImGuiPass` only detects the size mismatch and *queues* it
(`FrameRenderer::resizePending_`/`pendingWidth_`/`pendingHeight_`);
`drawFrame()` applies it at the very top of the *next* call, before
touching any per-frame-slot state — the one point in the frame loop
where nothing has been recorded yet.

**The ImGui-registered texture has to be re-registered, not just left
alone.** `sceneViewportSet_` (the `VkDescriptorSet` `ImGui_ImplVulkan_
AddTexture()` returns, sampled by `ImGui::Image()` in the "Viewport"
window) is bound to a specific `VkImageView` at registration time. Once
`resizeSceneTarget()` destroys the old `sceneColorTarget_` and creates a
new one, that old view is gone and the registered descriptor would be
pointing at a destroyed resource — ImGui has no way to detect this on
its own. Fixed by calling `ImGui_ImplVulkan_RemoveTexture(sceneViewportSet_)`
then re-`ImGui_ImplVulkan_AddTexture()` against the new view, immediately
after `resizeSceneTarget()` returns, in `FrameRenderer::drawFrame()`.

**No debounce — a deliberate, accepted tradeoff, not an oversight.**
While the Viewport panel's border is being actively dragged,
`ImGui::GetContentRegionAvail()` differs from the current extent on
essentially every frame, so this can queue (and the next frame apply) a
resize on consecutive frames — meaning a `vkDeviceWaitIdle` stall on
every frame during an active drag. A production UI would likely debounce
this (only resize after the drag settles for some interval, or throttle
to a few times per second). This codebase doesn't, matching its existing
"simplest correct implementation" bar elsewhere (e.g. the mutual-collision
O(n²) pass, or IBL's single combined bake) — the visible cost is a
resize-time stutter, not a correctness bug, and worth revisiting only if
that stutter turns out to matter for how this project is actually used.

**Interview-relevant:** *"Why does the aspect ratio need to be a
parameter now instead of a constant, when the FOV itself
(`Camera::FOV_DEGREES`) stays fixed?"* — because `glm::perspective(fovy,
aspect, near, far)` takes the *vertical* FOV directly and derives the
*horizontal* FOV from the aspect ratio; keeping vertical FOV fixed while
the aspect ratio changes is exactly the correct behavior for a resizable
viewport (a taller-than-wide panel should show more vertically, not
distort the existing view) — there was never a reason to touch
`FOV_DEGREES` itself, only the ratio that was previously hardcoded
alongside it.

### 37. Live-resize's `vkDeviceWaitIdle` stall × §20's uncapped `deltaTime` — diagnosed, then fixed as an opt-in toggle

Observed during interactive testing, not a code review find: dragging
"Restitution" (§30) toward `1.0` made the grid's post-scatter bouncing
look like a visibly discrete, time-stepped process. Root cause is a real
interaction between two independently-reasonable, already-documented
design choices — §20 already accepted uncapped `deltaTime` as a
*theoretical* risk ("nothing else in this codebase clamps `deltaTime`
either... not a new gap"); §36 gave it its first concrete trigger.

**The mechanism.** `Application::mainLoop()` computes `deltaTime` from
raw wall-clock time with no clamp anywhere (`Camera::processInput`,
`Projectile::update`, `updateInstanceSimulation`, `updateSpin` all
consume it uncapped). `VulkanContext::resizeSceneTarget()` (§36) calls
`vkDeviceWaitIdle()` — a real synchronous stall — whenever a Viewport
resize is pending, inside `drawFrame()`, which runs at the *end* of a
`mainLoop()` iteration, *after* that iteration's `deltaTime` was already
computed and consumed. So the stall doesn't affect that frame's physics;
it inflates the *next* iteration's `deltaTime` instead (`lastTime` was
captured before the stall), and that inflated, uncapped value flows
straight into position integration (`position += velocity * deltaTime`)
and damping (`pow(kDampingPerSecond, deltaTime)`) for all 343 instances
in one step.

**Why restitution changes the visibility, though the mechanism itself is
restitution-blind.** Two compounding reasons. First, restitution controls
how long a large velocity survives to be available for an inflated
`deltaTime` to act on — at `restitution_≈0` the impulse kills velocity
almost immediately (a narrow, low-probability window for a stall to
land in); at `restitution_=1` large velocities persist for seconds (a
wide window, and `velocity * largeDt` scales with that larger velocity
too). Second, and why 0 vs 1 barely differed *before* §36 existed either
(not a rendering bug — `objectBuffer_` re-uploads unconditionally every
frame, nothing is stale): `updateInstanceSimulation()`'s two
position-changing terms scale with `deltaTime` differently -
```cpp
instanceCurrentPositions_[i] += instanceVelocities_[i] * deltaTime;               // velocity-driven, scales with dt
instanceCurrentPositions_[i] -= n * (overlap * kPositionalCorrectionFactor);      // fixed 10%/frame, ignores dt entirely
```
At normal small `deltaTime`, the restitution-blind fixed correction
dominates visible separation and any bounce velocity gets damped away
within about a second regardless, so restitution's effect - real, but
tiny per frame - was masked. A large `deltaTime` flips which term
dominates: the fixed correction still only contributes 10%, but
`velocity * deltaTime` can now dwarf it, for whatever velocity the
restitution-scaled impulse just produced. Restitution never causes the
spike; it sets how much there is for the spike to multiply.

**Fixed as an opt-in, runtime-tunable toggle, not an unconditional
change.** One clamp, one authoritative location, upstream of every
consumer:
```cpp
// Application::mainLoop(), right after deltaTime is computed
if (context->clampDeltaTimeEnabled())
    deltaTime = std::min(deltaTime, context->maxDeltaTime());
```
`VulkanContext::clampDeltaTimeEnabled_` (`bool`, default `false`) /
`maxDeltaTime_` (`float`, default `1/15s`, the standard real-time-loop
"spiral of death" ceiling) follow this codebase's usual "a public setter
clamps its own invariant" convention (`setRestitution()`,
`setLod1ScreenSize()`), tunable via a new "Simulation Timing" section in
the "GPU Culling Stats" ImGui window next to "Collision". Defaulted to
**off**, deliberately: turning it on unconditionally would be a silent
behavior change for the very first frame after *any* stall (not just
resize - alt-tab, a breakpoint, a driver hitch), and this project's
"expose it, don't guess it" pattern (`shadowBias_`, LOD thresholds,
`restitution_`) already favors a runtime toggle over baking in a single
answer. It also turns the diagnosis above into something demonstrable
rather than just documented: drag restitution to `1.0`, resize the
panel, watch the chained-wave response; toggle the clamp on, watch it
not happen - the same "prove the mechanism, verify by eye" bar this
project has applied since Phase 5's oscillation test, now covering a
timing bug instead of a rendering one.

**Interview-relevant:** *"Why didn't §20 clamp `deltaTime` originally,
and why a toggle now instead of just fixing it outright?"* — §20 had no
reachable stall source to guard against (`vkDeviceWaitIdle` only existed
at shutdown), so clamping then would have been speculative
future-proofing this project's discipline avoids; §36 made the case
reachable, which is why this is a *revisit* section rather than an edit
to §20 - the original risk assessment was correct when written, and
rewriting it to pretend otherwise would erase the "why." The toggle
(rather than an unconditional clamp) is the same reasoning one level
up: an always-on fix is the right call for a shipping product where
nobody should see the bug, but this is also a portfolio piece meant to
*demonstrate* having found and understood it - collapsing before/after
into one state would remove the ability to show either on demand, for
the cost of one `bool` and a few ImGui lines.

---

### 38. Camera controls: Space/Ctrl vertical movement, UI-reveal moved off Ctrl - and the duplicated-key-check bug that move exposed

`Camera::processInput()` gained `Space`/`Left Ctrl` for movement along
the camera's own local up axis (new `getUp()` helper,
`normalize(cross(right(), forward()))` - not world-up, though the two
coincide until the camera actually pitches), alongside the existing
WASD horizontal movement. Since `Left Ctrl` was the modifier that
revealed the cursor for UI adjustment (§18), and it's now a movement
key, the UI-reveal modifier moved to `Shift` in the same change.

That modifier move exposed a real bug, not just a doc-staleness issue.
`Application::mainLoop()` had its own **independent copy** of the
"is the cursor in UI-reveal mode" check, re-derived from a hardcoded
`GLFW_KEY_LEFT_CONTROL` poll, to gate whether a left-click fires a
projectile:
```cpp
bool ctrlHeld = glfwGetKey(...GLFW_KEY_LEFT_CONTROL...) == GLFW_PRESS || ...;
if (leftPressed && !prevLeftMousePressed_ && !ImGui::GetIO().WantCaptureMouse)
    if (!ctrlHeld) { /* launch projectile */ }
```
This was never updated when `Camera.cpp`'s modifier changed, so it went
stale in two directions at once: holding `Ctrl` (now legitimately bound
to "move down") would incorrectly suppress firing even in ordinary
mouse-look mode, while `Shift` - the actual new UI-reveal key - wasn't
checked by this duplicate at all. The fix wasn't to swap `CONTROL` for
`SHIFT` in `Application.cpp` (that just reintroduces the same
duplication, ready to go stale again the next time the binding
changes) - it was to delete the duplicate check entirely and call
`context->camera().cursorVisible()`, the accessor `Camera` already
exposes and that `Application::mainLoop()` already uses two lines above
for the `ImGuiConfigFlags_NoMouse` sync (§27). One source of truth for
"is the cursor in UI mode," not two independently-derived ones that
happen to agree only as long as nobody touches the key binding.

**Interview-relevant:** *"Why does a controls tweak in `Camera.cpp`
count as a bug fix in `Application.cpp`?"* — because the two files were
each computing the same boolean from raw input state instead of one
computing it and the other reading it. Duplicated derivation from the
same source is a latent bug whether or not the source ever changes;
this change just happened to be the one that collected on it. Same
lesson as §27's `ImGuiConfigFlags_NoMouse` fix one section up - prefer
"read the single accessor" over "recompute the same condition again
and hope both copies stay in sync."

### 39. Live-resized window / swapchain

Closes a gap §36 explicitly left open: that section made the *docked
Viewport panel* live-resizable, but `Application::init()` still created
the GLFW window with `GLFW_RESIZABLE = GLFW_FALSE`, and nothing in the
codebase had ever handled `vkAcquireNextImageKHR`/`vkQueuePresentKHR`
returning `VK_ERROR_OUT_OF_DATE_KHR`/`VK_SUBOPTIMAL_KHR` — dragging the
actual OS window border was never possible, let alone safe, before this
change.

**Why this was a much smaller change than §36's scene-target resize.**
§36 had to recreate two pipelines (`pipeline_`/`skyboxPipeline_`)
because both bake a `VkViewport` against the offscreen scene target's
extent. The swapchain's own render pass/framebuffer (`renderPass_`/
`framebuffer_`) host *only* `ImGuiPass` (Phase 11 moved all 3D geometry
onto `sceneRenderPass_`/`sceneColorTarget_` instead) and ImGui's Vulkan
backend doesn't bake a fixed viewport the way this codebase's other
pipeline classes do — it takes the swapchain's `VkRenderPass` once at
`ImGui_ImplVulkan_Init()` time and reads the actual framebuffer extent
per-frame via `vkCmdSetViewport`-equivalent internal state. So
`VulkanContext::resizeSwapchain()` only has to destroy/recreate
`framebuffer_`/`depthBuffer_`/`swapchain_` — no pipeline, no descriptor,
no shader touches the swapchain's extent anywhere in this codebase.
`renderPass_` itself is untouched, same "format/structure only, no
extent" fact §36 already relied on for `sceneRenderPass_`.

**Two independent resize triggers, not one.** An ordinary window drag
is caught by comparing `glfwGetFramebufferSize(window_)` against
`swapchain_.getExtent()` at the top of every `FrameRenderer::drawFrame()`
call — the same "poll and compare" pattern §36 already established for
the Viewport panel, chosen for consistency rather than wiring up a
`glfwSetFramebufferSizeCallback`. But a plain size comparison can't
catch every reason a swapchain goes stale (e.g. the surface format
becoming suboptimal after a display mode or HDR/SDR switch, with the
*size* unchanged) — Vulkan's own answer to that is the
`VK_SUBOPTIMAL_KHR`/`VK_ERROR_OUT_OF_DATE_KHR` return codes from
`vkAcquireNextImageKHR`/`vkQueuePresentKHR`, so both are checked too,
setting a `swapchainNeedsRecreate_` flag consumed at the top of the
*next* frame (present's out-of-date/suboptimal can't be handled
mid-frame — presentation is the last thing `drawFrame()` does).
`vkAcquireNextImageKHR` returning `VK_ERROR_OUT_OF_DATE_KHR` directly is
the one case that *can't* wait for the next frame: the returned
`imageIndex` isn't valid to render into *this* frame, so that branch
recreates immediately and returns without recording or submitting
anything, deferring the rest of the frame's work entirely.

**The fence-reset-ordering bug this would otherwise have introduced.**
Before this change, `drawFrame()` did `vkResetFences(frame.inFlightFence)`
*before* `vkAcquireNextImageKHR`. Naively bolting an out-of-date check
onto the existing order — reset, acquire, check, bail out on
`return` — would leave that frame slot's fence reset (unsignaled) with
nothing ever going on to signal it, since bailing out skips the
`vkQueueSubmit` that would normally do so. The *next* call to
`drawFrame()` reusing that same slot calls `vkWaitForFences(...,
UINT64_MAX)` on it and blocks forever — the whole render thread hangs
the very first time a resize lands mid-acquire. The fix (the standard
one for this exact Vulkan pitfall): reorder so the fence is only reset
*after* a successful acquire. Waiting on a fence that's still signaled
from this slot's last real submit costs nothing extra; it's exactly the
state `vkWaitForFences` already found true at the top of this same
`drawFrame()` call, a few lines earlier.

**Per-swapchain-image resources, resized conditionally.**
`imagesInFlight` (non-owning `VkFence` pointers, one per swapchain
image) is unconditionally re-sized/reset to the new image count after
every recreate — cheap, and correctness requires it regardless of
whether the count actually changed (stale fence pointers from the old
swapchain's images would otherwise linger). `imageRenderFinished` (real,
owned `VkSemaphore` objects) is only destroyed and recreated if the
image count *changed* — the common case, a plain window resize on the
same surface/device, keeps the same count, so this avoids needless
semaphore churn on every drag frame. `ImGui_ImplVulkan_SetMinImageCount()`
is called unconditionally after every recreate regardless, since it's
cheap and is ImGui's own documented contract for "the swap chain was
recreated."

**Minimize is a real edge case, not a hypothetical one.** A minimized
Win32 window reports a `0×0` framebuffer via `glfwGetFramebufferSize()`,
and `vkCreateSwapchainKHR` with a `0×0` extent is invalid — this isn't
theoretical, it's the very first thing that happens if a user minimizes
the window with this feature half-implemented. Fixed in
`Application::mainLoop()`, not in `VulkanContext::resizeSwapchain()`:
after `glfwPollEvents()`, if the framebuffer is `0×0`, the loop calls
`glfwWaitEvents()` in a blocking loop (re-checking the size each
iteration) instead of calling `drawFrame()` at all, until the window is
restored. `resizeSwapchain()` itself does no defensive 0-size clamping —
the one resize path in this codebase that doesn't clamp its own input —
because there's no sensible fallback size for "the user minimized the
window," unlike §36's 64px floor for a transiently-small dock panel.
`lastTime` is reset to `glfwGetTime()` right after the wait loop exits,
so the minimized duration itself doesn't get counted as one giant
`deltaTime` spike the moment the window is restored (see §37 for why an
uncapped `deltaTime` spike is a real, previously-diagnosed problem in
this codebase, not a hypothetical one).

**No debounce — same accepted tradeoff as §36.** Dragging the window
border can trigger a `vkDeviceWaitIdle` stall (inside
`resizeSwapchain()`) on consecutive frames, exactly like §36's Viewport-
panel resize. Same reasoning: keeping the resize path simple was judged
more valuable than the smoother-but-more-complex alternative (debounce,
or per-resource fencing instead of a full device idle) for a project at
this scale.

**Interview-relevant:** *"Why doesn't this resize need to touch
`pipeline_`/`skyboxPipeline_` when §36's did?"* — because those two
pipelines' `VkViewport` is baked against the *offscreen scene target's*
extent (`sceneColorTarget_`), which Phase 11 already decoupled from the
swapchain entirely. A window resize changes the swapchain's extent, not
the scene target's — the two are now independent by construction, so a
change to one has zero pipeline-recreation cost on the other. That
decoupling was a side effect of Phase 11's dockable-viewport design, not
something this feature had to build — this section is really evidence
that Phase 11's architecture choice paid off later, for a use case it
wasn't originally designed for.

### 40. Mesh-detail-derived LOD2 threshold

Closes the gap §26/§32 both explicitly named but left open: §26's own
writeup called out "not derived from mesh detail or screen-space size"
as a "not yet done" item; §32 closed the screen-space half, leaving
`lod1ScreenSize_`/`lod2ScreenSize_` as two *independent* hand-picked
constants (120px/60px) with nothing tying either one to the actual
LOD1/LOD2 meshes' geometric complexity.

**Why "per-mesh detail" can't mean "per object type" here.** The scene
has exactly one object type — 343 copies of the same Suzanne mesh, at 3
LOD levels. There's no second, differently-detailed object to compare
against, so a design that assigned different LOD thresholds *per object
type* would have nothing to demonstrate: every instance in the grid
would get the identical value regardless. "Per-mesh detail" therefore
has to mean grounding the threshold in the geometric relationship
*between LOD0/LOD1/LOD2 themselves* — the same single mesh's own detail
levels — not in a per-instance or per-type lookup that this scene has no
use for. Building the latter (a real second mesh type, just to have
something to differentiate) was considered and deliberately scoped out —
it would have meant new grid-generation logic, new vertex/index buffers,
and new instance-classification data with no payoff beyond making this
one feature demonstrable, the kind of "designing for a hypothetical
future requirement" this codebase avoids elsewhere.

**Why triangle count, not a geometric error metric.** The theoretically
"more correct" signal would be something like Hausdorff distance or a
per-vertex quadric error between LOD0 and each simplified LOD — how much
does the *shape itself* deviate, not just how many triangles it's built
from. This codebase has no such analysis anywhere (`ObjLoader` computes
a bounding radius and nothing else geometric), and adding one would be a
disproportionate amount of new machinery for a single derived default.
Triangle count (`mesh.indices.size() / 3`, already available for free
from data `ObjLoader::load()` already returns) is a legitimate, if
cruder, proxy for "how much detail was thrown away" — fewer triangles
reliably means less silhouette/shading detail for any real decimation
process — and costs nothing to compute. Consistent with this codebase's
recurring "simplest correct implementation" bar (the small-angle
screen-size approximation in §32 is the same kind of call).

**The formula, and why only `lod2ScreenSize_` is derived.**
`lod2DetailRatio() = lod2TriangleCount_ / lod1TriangleCount_` — for the
current assets, 47/289 ≈ 0.163 (measured via a temporary debug print
during verification; the actual `ObjLoader`-loaded/deduplicated counts,
not a raw `.obj` face-line count). `lod2ScreenSize_`'s *startup default*
becomes `lod1ScreenSize_ * lod2DetailRatio()` ≈ 120 × 0.163 ≈ 19.5px,
down from the old flat 60px. Direction check: a bigger detail drop
between LOD1 and LOD2 (smaller ratio) should *delay* the switch to
LOD2 until the object is smaller on screen, where the pop is less
perceptible — smaller ratio, smaller threshold, later switch. That's
exactly what the formula produces. `lod1ScreenSize_` (the LOD0→LOD1
threshold) has no equivalent derivation and stays the one manually-
anchored constant it always was: there's no "LOD -1" mesh more detailed
than LOD0 to compute a ratio against, so *some* top-level judgment call
is irreducible no matter what formula governs everything below it.

**Why this only changes the default, not the tuning mechanism.** The
"GPU Culling Stats" window's `LOD1 Screen Size`/`LOD2 Screen Size`
sliders are untouched — §26's "easier to find the right feel by eye
than to compute" reasoning for exposing these as live-tunable values in
the first place still holds, and this change doesn't relitigate it. It
only fixes what `lod2ScreenSize_` starts at before anyone touches the
slider, plus adds a "Reset LOD2 to mesh-derived default"
(`VulkanContext::resetLod2ScreenSizeToMeshDefault()`) button so that
starting point stays recoverable after manual tuning drifts away from
it — the same "grounded default, still overridable" shape this codebase
hasn't used elsewhere but that fits naturally alongside the existing
slider-based tuning pattern.

**Zero GPU-side changes.** `culling.comp`, `ComputeDescriptor`, and
`FrustumPlanes` are all untouched — the shader was already reading
`frustum.lodParams.x`/`.y` as opaque threshold values with no assumption
baked in about how the CPU computed them. This entire feature lives in
`VulkanContext::initSceneData()` (capturing the triangle counts) and the
ImGui panel (displaying them) — the lowest-risk possible way to close
this gap, with no shader recompile, descriptor, or buffer-layout risk at
all.

**Verification.** A temporary `std::cout` right after the derivation
(removed before committing) confirmed the actual loaded counts and the
resulting value end-to-end: `tris L0=968 L1=289 L2=47 ratio=0.16263
lod1ScreenSize_=120 lod2ScreenSize_=19.5156` — matching the hand-computed
expectation exactly, and materially different from (smaller than) the
old hardcoded 60px default, confirming the derivation is actually doing
something rather than coincidentally reproducing the old constant.

**Interview-relevant:** *"Why is the LOD1/LOD2 ratio computed from
triangle count instead of, say, vertex count?"* — `ObjLoader` dedups
vertices by position+normal+uv (see its own module notes in
architecture.md), so vertex count already reflects *unique* attribute
combinations, not raw face density, and can undercount the actual
geometric complexity a decimation process removed (two meshes with
similar vertex counts can still differ hugely in triangle count via
fan/strip topology). Triangle count is the more direct proxy for "how
much surface detail" a mesh actually encodes, and it's what every
LOD/decimation tool actually targets when simplifying a mesh in the
first place.

### 41. Swept projectile collision

Closes the tradeoff §20 explicitly named and accepted as *theoretical*,
which §37 later flagged as no longer purely hypothetical once live-
resize gave the frame loop its first real mid-loop stall source: the
projectile-vs-grid hit test only checked the projectile's position once
per frame (`glm::length(instanceCurrentPositions_[i] - projPos) <
hitDist`), so a large enough `deltaTime`/speed could move it further in
one frame than the hit radius, skipping clean over an instance without
the point check ever landing inside it.

**Why this stopped being purely theoretical.** §37's `maxDeltaTime_`
clamp (default `1/15s`) was added to tame a *different* symptom (the
mutual-collision system looking visibly stepped after a resize stall),
but it does nothing to prevent single-frame tunneling on its own - at
the projectile's default speed (30 u/s) and the clamp's own default
ceiling, one frame can still move it `30 * (1/15) = 2.0` units, already
larger than the ~1.3-1.8 unit collision radius the point check compares
against. The clamp bounds *how bad* a spike can get; it was never a fix
for this specific gap, and turning it off (still the project default)
removes even that bound.

**The fix - closest-point-on-segment, not closest-point.** `Projectile`
gained `previousPosition_`, captured at the top of `update()` before
that frame's movement is applied, and a `previousPosition()` accessor
alongside the existing `position()`. `updateInstanceSimulation()`'s
projectile check now treats `[previousPosition(), position()]` as a line
segment and finds each instance's closest point on it (`t = clamp(dot(
toInstance, segDir) / segLenSq, 0, 1)`, standard segment-projection
formula), comparing *that* distance against `hitDist` instead of the
distance to the segment's endpoint alone. A point check is really just
this same test degenerated to a zero-length segment - the fix
generalizes the existing logic rather than replacing it with something
structurally different.

**Earliest hit along the segment, not first array index.** The old loop
found the first instance (by array index) within `hitDist` of the point
and immediately broke. Once the test segment can span a real distance,
more than one instance can plausibly be within `hitDist` of *some* point
along it, and the projectile should logically stop at whichever it
reaches *first*, not an arbitrary later one that happens to sit at a
lower array index. The fix tracks the smallest `t` (`bestT`) across all
343 instances in one pass - same `O(343)` complexity as before, since
each iteration is still one segment-distance computation instead of one
point-distance computation, no algorithmic complexity change.

**Blast origin moved to the actual impact point.** The pre-existing
radial blast falloff used to originate from `projPos` (wherever the
projectile ended up *this frame*) - harmless when the point check and
the swept check agree (small movement per frame), but for exactly the
large-movement case this fix targets, the post-move position can be
well past the true impact point along the segment. The blast now
originates from `segStart + segDir * bestT` - the actual point on the
path closest to the hit instance - so the fix is internally consistent:
the same generalization that finds the hit also relocates where its
effect is centered.

**Reproduced, then fixed - not fixed speculatively.** Verified by
temporarily launching a projectile at 300 u/s (10× default) straight
down a column of instance centers (the grid's `x=0, y=0` line passes
through 7 instances exactly, one per `z` value, since `GRID_SIZE=7` is
odd), combined with one artificially inflated (~250ms)
`std::this_thread::sleep_for` right after `mainLoop()`'s first
`glfwGetTime()` capture to force one genuinely large `deltaTime` frame.
A temporary diagnostic print compared what the old point check would
have found (checking `instanceCurrentPositions_[i]` against `segEnd`
alone) against the new swept result in the same run:
```
[TEMP-VERIFY] swept hit at t=0.519691 segLen=78.893 old-point-check-would-hit=0
```
`segLen≈79` units in one frame, `old-point-check-would-hit=0` - the old
algorithm would have missed this shot entirely, letting the projectile
sail through the grid untouched - while the new swept check correctly
caught it just past the segment's midpoint. All temporary test code
(the fast launch, the sleep, the diagnostic print) was removed before
committing; the fix itself needed none of it.

**Why this needed no GPU-side changes at all.** Unlike every LOD/culling
feature in this codebase, projectile collision has never touched
`culling.comp`, `ComputeDescriptor`, or any GPU buffer - it's pure CPU
simulation state (`Application::mainLoop()` → `updateInstanceSimulation()`,
see §20), so this entire fix lives in two files
(`Projectile.h`/`.cpp` for the new accessor, `VulkanContext.cpp` for the
sweep test) with zero shader recompile or descriptor risk.

**Interview-relevant:** *"Why not just clamp the projectile's per-frame
movement distance instead of implementing a proper sweep?"* — that would
cap *how far* it moves, silently changing the projectile's effective
speed during a stall (it would appear to slow down exactly when the
frame rate drops), rather than correctly resolving what it *passed
through* while moving at its actual speed. A swept test answers the
right question - "did the path this frame's real movement traced
intersect anything" - without changing the simulation's actual physics,
which a movement clamp would.

### 42. Real PBR material sourcing (ambientCG)

Closes Phase 8 milestone 2's other named gap (§25 shipped the sampling
*mechanism* - `Material`, the 4-texture bundle, `triangle.frag`'s
channel reads - using small self-generated flat/gradient PNGs as
placeholders, explicitly flagged as not real material photography).

**Why this needed sourcing, not generation.** Nothing in this codebase
(or its vendored `third_party/`) can *author* a physically-based
material - a real normal/roughness/AO set encodes actual surface
photogrammetry data, not something proceduralizable the way the
project's other placeholder-closing work (e.g. §33's procedural sky) is.
This is squarely asset-content work, not an engineering gap, and was
scoped accordingly rather than forced into a code-shaped solution.

**Why ambientCG, and why `Bricks097` specifically.** ambientCG
(ambientcg.com) publishes its entire library under CC0 1.0 Universal -
public domain, no attribution required, no account/API key needed to
download - the cleanest possible licensing position for a public repo,
same reasoning that made `opengl-tutorials/ogl` (§25) a safe source for
`suzanne_pbr.obj`. Its download URLs follow a stable, scriptable pattern
(`https://ambientcg.com/get?file=<AssetID>_<Resolution>-<Format>.zip`),
confirmed by fetching one directly rather than guessing. Several
candidates were downloaded and inspected before choosing: pure-metal
materials (`Metal032`, `MetalPlates006`) looked good but ship no
AmbientOcclusion map at all (physically sensible - a flat metal panel
has no self-shadowing cavities to bake), which would have meant
synthesizing that channel anyway, undermining the point. `Bricks097`
ships real Color/NormalGL/Roughness/AmbientOcclusion maps - genuine
photogrammetry data for 3 of the 4 channels `Material` needs, missing
only Metalness (bricks aren't metal, so ambientCG doesn't generate one -
a real material fact, not a gap).

**The one synthesized value is a material fact, not a placeholder.**
`metallic_roughness.png`'s blue (metalness) channel is a constant 0
across the whole image - but this isn't the same category of
fabrication the original placeholders were (arbitrary flat/gradient
values standing in for data that was never measured). Brick is
correctly non-metallic; encoding that as a uniform 0 is the *accurate*
value for this real material, the same way a real metal material's
missing AO map would correctly imply "no additional occlusion" rather
than a gap. The green (roughness) channel is 100% real, resized from
ambientCG's own Roughness map.

**Why `test_texture.png` (the albedo) was also replaced, beyond the
named gap.** The roadmap item only named `normal.png`/
`metallic_roughness.png`/`ao.png` - `test_texture.png` was a synthetic
black/white checker test pattern (a UV-sanity-check asset, not itself
claimed as "real material"), left alone by the original scoping. Pairing
`Bricks097`'s real normal/roughness/AO data with an unrelated checker
albedo would look incoherent (bump/occlusion detail with no matching
color variation to justify it) - so this section deliberately widened
scope by one file to keep the whole material internally consistent,
using the same material's own Color map.

**Channel-combining without a compiler on PATH.** This codebase already
vendors `stb_image.h`/`stb_image_write.h` (`third_party/stb/`), the
obvious tool for a one-off image-processing utility - but `cl.exe`
wasn't discoverable on this session's `PATH` without the full Visual
Studio Developer environment, and standing that up just for a disposable
script wasn't worth it. Used PowerShell's `System.Drawing.Bitmap`
instead: `Graphics.DrawImage` with `HighQualityBicubic` interpolation
for the 1024→512 downsize, then a per-pixel `GetPixel`/`SetPixel` loop
(fast enough at 512×512 = 262,144 pixels for a one-time asset-prep pass,
even though it's not how this codebase would ever touch pixels at
runtime) to build `metallic_roughness.png` from the resized Roughness
map's green channel plus a constant blue.

**No code changes anywhere.** `VulkanTexture::create()`'s format split
(albedo sRGB, the other 3 UNORM), `VulkanDescriptor`'s 7 bindings, and
`triangle.frag`'s sampling/channel-convention logic (§25) are all
untouched - `VulkanContext::initCore()` already loads exactly these 4
filenames, so this is a pure content swap under the same paths.
Verified via the existing `[Texture] loaded ...` startup log (confirmed
all 4 now report `512x512`, no load errors) and a visual check of the
running app for crashes or black/NaN artifacts (§25's degenerate-UV
guard is unaffected - only LOD0, which has real UV data, exercises
tangent reconstruction against these maps; LOD1/LOD2 remain on the
constant-texel path regardless of what these files contain).

**Interview-relevant:** *"Why not just generate a synthetic normal map
procedurally (e.g. Perlin noise) instead of sourcing a real one?"* -
that would still be fabricated data with no connection to an actual
photographed surface, the same category of placeholder this section
exists to replace, just a fancier-looking one. The point isn't "make the
bumps look more complex," it's "sample data that actually came from
measuring something real" - a synthetic-but-elaborate map wouldn't
close this gap any more than the original flat one did.

### 43. Transparency: alpha blending + GPU-side back-to-front sort

Motivated by planned future work: swapping the grid's material for
something genuinely translucent (jelly, glass, liquid). The graphics
pipeline had `blendEnable = VK_FALSE` everywhere - fully opaque, no
blend state existed at all. Enabling blending exposes a real
architectural mismatch this section closes.

**The actual problem.** `culling.comp` compacts visible instances into
each LOD bucket via `atomicAdd` - draw order is whichever GPU thread's
atomic increment lands first, which is unspecified and can vary frame to
frame. For opaque rendering this is invisible: the depth test resolves
occlusion regardless of draw order. For alpha blending it's not -
overlapping transparent instances composite in whatever order they
happen to land in, which can flicker between frames and looks visibly
wrong wherever the grid's 343 instances overlap on screen, which is
constantly from most camera angles.

**Why sorting, not order-independent transparency (OIT).** Weighted
blended OIT (accumulate color×weight and revealage into two render
targets, composite once at the end) sidesteps the sorting problem
entirely and would have needed zero changes to `culling.comp`'s
compaction scheme - a legitimate, arguably more idiomatic choice for a
GPU-driven renderer that already embraces unordered compaction
everywhere else. Sorting was the requested direction for this pass, so
that's what got built; OIT remains a valid alternative if approximate
compositing (weighted OIT's known tradeoff) ever proves visually
insufficient.

**Why the LOD bucket structure already gives free coarse ordering.**
`lod1ScreenSize_ >= lod2ScreenSize_` is an existing, already-enforced
invariant (§26/§40), and screen size is monotonically decreasing with
camera distance for a fixed bounding radius. So LOD0's camera-distance
range is *provably* smaller than LOD1's, which is provably smaller than
LOD2's - the buckets are already distance-sorted relative to each other,
for free, as a side effect of an invariant that exists for an unrelated
reason. Drawing bucket order `2, 1, 0` (farthest bucket first) instead
of the existing `0, 1, 2` therefore gives a *provably correct*
back-to-front macro-order between buckets with a one-line loop-direction
change - no new data, no new computation. Sorting only needs to solve
the *within-bucket* problem: instances in the same bucket can still be
at different distances and overlap on screen.

**Why odd-even transposition, not a bitonic network.** A single-
workgroup, shared-memory sort was the right call either way (≤343
elements per bucket comfortably fits one workgroup's shared memory and
avoids any multi-dispatch/multi-barrier orchestration - the whole sort
is internal to one shader invocation, synchronized with GLSL `barrier()`
calls). Between the two classic parallel sort networks, bitonic needs
power-of-2-sized input (padding/sentinel handling) and partner-index bit
manipulation per stage; odd-even transposition needs neither - just N
phases of "compare adjacent pairs, alternate which pairs are compared
each phase." At this scale (≤343 real elements, padded to a fixed
512-wide array) bitonic's O(n log²n) vs. odd-even's O(n²) phase count is
a difference of microseconds on a GPU, not a difference that matters -
so the simpler, easier-to-verify-correct construction won, matching this
codebase's recurring bar (the O(n²) mutual-collision pass, §21, made the
same call for the same reason).

**The padding sentinel.** Slots beyond a bucket's real `instanceCount`
(the array is fixed at 512 wide, but a bucket rarely has 343 real
entries in it, let alone 512) get `position.w = -1.0` - camera distance
is never negative, so padding always sorts to the tail past index
`[count)`, which the indirect draw's `instanceCount` never reads anyway.
No branch needed to skip padding during the sort itself; the compare
function doesn't need to know which slots are real.

**Why `.w`, not a new field.** `InstanceData` stays exactly `vec4
position` - `triangle.vert` was confirmed (by reading it, not assuming)
to only ever consume `.xyz` for translation, so repurposing the
previously-hardcoded `1.0` as a sort key costs zero bytes, zero buffer
changes, zero descriptor changes. `culling.comp` already computes camera
distance for the screen-size LOD test (`camDist`), so writing it into
`.w` instead of a literal `1.0` is a one-line change, not new work.

**Why a second pipeline, not a flag on the existing one.** This
codebase bakes pipeline state (blend, depth-write, viewport) at creation
time throughout, with no dynamic-state precedent anywhere - confirmed
the same way §35/§36 already confirmed no `VK_DYNAMIC_STATE_VIEWPORT`
precedent exists (`grep -r VK_DYNAMIC_STATE`, zero hits outside
`third_party/`). Adding a `transparent` parameter to `VulkanPipeline::
create()` and instantiating a second `transparentPipeline_` matches that
existing pattern rather than introducing dynamic blend state as a new
one. `resizeSceneTarget()` (§36) has to recreate it alongside
`pipeline_`/`skyboxPipeline_` for the same reason all three exist:
they all bake a `VkViewport` against `sceneColorTarget_`'s extent.

**Depth test stays on; only depth *write* turns off.** A transparent
draw still needs to be occluded by opaque geometry in front of it
(depth test on), but must not occlude *other* transparent instances via
the depth buffer the way opaque draws do (depth write off) - correctness
between transparent instances relies entirely on draw order (the sort)
instead. This is the standard transparency pipeline convention, not
something specific to this codebase.

**`material.albedo.a` was always there, just discarded.**
`MaterialPushConstants::albedo` was already a `vec4`, with `.a`
documented as simply "unused" since Phase 8 milestone 1 -
`triangle.frag` hardcoded `outColor = vec4(color, 1.0)` regardless of
what the push constant carried. Wiring `.a` through to the actual output
alpha is necessary plumbing for transparency to mean anything at all
(without it, `transparentPipeline_`'s blend state would have nothing to
blend with - source alpha would always read as 1.0, fully opaque
regardless of `gridAlpha()`), not itself part of "the shading model" -
no refraction, IOR, or subsurface approximation was added here.

**One shared toggle, not per-instance.** `gridAlpha()` drives the whole
grid *and* the projectile together, matching the existing single-shared-
`Material` architecture (`material_` bound by both `descriptor_` and
`projectileDescriptor_`). The projectile is drawn via a single direct
`vkCmdDrawIndexed` (not part of the sorted indirect buckets) and always
issued after the grid loop regardless of its own distance - its blend
order relative to the grid is a known, accepted gap this pass doesn't
address, the same "explicitly out of scope" treatment given to mixed
opaque+transparent scenes and the actual jelly/glass shading model.

**Verification.** Rebuilt with all 3 new/changed shaders compiling
cleanly (`culling.comp`, `triangle.frag`, the new `sortInstances.comp`),
confirmed the default (`gridAlpha_ = 1.0`) case renders byte-identically
to before (same brick-textured grid, no visible change) - the whole
transparent path is unreachable at the default, by construction, so
there's no risk of regressing existing behavior. Then temporarily forced
`gridAlpha_ = 0.4` and screenshotted: correct translucent rendering with
the sort enabled (could see through nearer instances to farther ones,
coherently). With the sort disabled, the app still ran without
crash/corruption - the bucket-level ordering alone already gives a
reasonable approximation from the default camera framing, which is why
the within-bucket sort's marginal visual effect wasn't dramatically
obvious in a single static screenshot comparison; it's still the
provably-necessary piece for the general case (an oblique angle looking
along a row, or a denser/more-overlapping arrangement, would expose it
more clearly). Both temporary overrides were reverted before committing.

**Interview-relevant:** *"Why does sorting only 3 small buckets
(≤343 elements total) need a real parallel sort algorithm instead of
just reading them back to the CPU and using `std::sort`?"* - because
this codebase's entire culling/LOD pipeline is deliberately GPU-driven:
the CPU never reads back which instances passed culling or how many,
only aggregate counts *after* the frame for the debug overlay (§11). A
CPU-side sort would mean reading the compacted buffer back every frame
before the draw that needs it - a GPU→CPU→GPU round trip and stall on
every frame transparency is active, exactly the kind of synchronization
this architecture has avoided everywhere else. A same-frame, GPU-side
sort keeps the whole pipeline's data dependency chain on the GPU, which
is also *why* it has to be a real (if simple) parallel algorithm rather
than a single-threaded shader looping over the array - a workgroup with
sequential logic would still be one GPU "thread" doing all the work,
throwing away the parallelism a compute shader exists to provide.

### 44. Master texture toggle, default off

A runtime switch for material texture sampling, requested explicitly as
default-**off** - a deliberate visible-default change, not the usual
"new feature off by default preserves existing behavior" pattern this
codebase's other toggles follow (`clampDeltaTimeEnabled_`,
`transparencySortEnabled_`). Since real textures have been the shipped
default since Phase 8 milestone 2/§25, and a real sourced material since
Phase 20/§42, turning texture sampling off by default is a genuine
regression in default visual fidelity - accepted here because the point
is to make the *comparison* available on demand (flat PBR vs. textured
PBR, both real lighting), not because flat shading is now the intended
steady state.

**Where the flag lives.** `MaterialPushConstants::metallicRoughness.z`
was documented "unused" since Phase 8 milestone 1 - the same "repurpose
an already-there, already-zero-cost vec4 slot" move this session already
made twice (`InstanceData.position.w` for §43's sort key,
`MaterialPushConstants.albedo.a` for §43's opacity). No struct size
change, no new descriptor, no new buffer.

**Why branching in the shader is safe.** `material.metallicRoughness.z`
is a push constant - identical for every fragment in a given draw call,
not a per-fragment varying value. Branching around `texture()` calls
with implicit LOD (derivative-based mip selection, which `dFdx`/`dFdy`-
based `perturbNormal()` also depends on) is only undefined behavior
under *non-uniform* control flow (different fragments in the same
sub-group taking different branches, which breaks derivative
computation); a value constant across the whole draw call is uniform
control flow by definition, so this branch has no such hazard.

**Why "off" reproduces Phase 8 milestone 1 exactly, not a new flat
look.** `finalAlbedo`/`metallic`/`roughness`/`ao` all already had the
*shape* "push-constant factor × texture sample" (§25's own design, kept
specifically so a texture swap or removal wouldn't need new math) -
setting the texture-sample side of each product to a neutral `1.0`
(`vec3(1.0)` for albedo/metallic-roughness, `1.0` for AO) when
`useTextures` is false reproduces exactly what those expressions
evaluated to before Phase 8 milestone 2 ever added real texture
sampling, not an approximation of it. The normal map needed an actual
branch (not a neutral-multiply trick) since `perturbNormal()` isn't a
multiplicative factor on `fragNormal` - off falls back to
`normalize(fragNormal)`, the pre-milestone-2 behavior exactly.

**Why one shared toggle drives both the grid and the projectile.**
Same reasoning as `gridAlpha()` (§43): both already read from the same
shared `Material material_` instance, and this codebase's per-object
distinctness comes entirely from the push constant's other channels
(different albedo/metallic/roughness factors, §17), not from having
independently-textured materials. Adding a second, projectile-only
toggle would be inventing a distinction the architecture doesn't
otherwise have.

**Verification.** Screenshotted both states directly (not just read the
code): off renders the flat, uniformly gray-white grid with real IBL
reflections/shadows but no brick pattern; on (temporarily forced,
reverted before committing) reproduces the exact brick-textured look
Phase 20 shipped, pixel-for-pixel indistinguishable from before this
toggle existed. Confirms the neutral-multiply/fallback-normal approach
above is actually equivalent to the pre-texture code path, not merely
argued to be.

**Interview-relevant:** *"Why default it off if that's a visible
regression from what the project already shipped?"* - because the
toggle's purpose is explicitly to make a comparison available on demand
for a portfolio/interview context (this project's established pattern -
§37's deltaTime clamp exists for the same "prove the mechanism, verify
by eye" reason), not to change what the project looks like when someone
just runs it to see the finished result. Whether "off" or "on" is the
better *permanent* default is a separate, legitimate product question
this section doesn't resolve - it was an explicit instruction, not a
default this document is claiming is obviously correct in general.

### 45. Refraction/IOR shading (Phase 23 M1), previous-frame capture instead of a render-pass split

The first real transparent *shading model* §43/§44 both flagged as
future work: glass/jelly/liquid materials that visibly bend the scene
behind them by index of refraction, not just alpha-composite with it.
Global toggle, matching `gridAlpha()`'s whole-grid scope (see
docs/roadmap.md's Phase 23 - resolved "global vs. per-instance" in favor
of global before implementation, since per-instance would need M2's
material-bucketing infrastructure as a prerequisite and this milestone
was scoped to ship independently of M2).

**The core constraint, and the design pivot away from the original
plan.** Vulkan can't sample and write the same attachment within one
render pass - a refractive fragment needs to read "the scene behind it,"
but `sceneColorTarget_` is also what's currently being rendered into.
The plan this section was written from originally called for splitting
`GeometryPass` into two render pass instances (opaque draws, then a
mid-frame image copy, then refractive draws sampling that copy) -
architecturally correct, but the single biggest-risk item identified
during planning, since it would have touched `FrameGraph`/`PassStage`
plumbing that every other phase back to §11 has left alone. Implemented
instead: capture the *previous* frame's fully-composited
`sceneColorTarget_` into a new `sceneColorCopy_` once per frame, *before*
this frame's own scene render pass begins (not mid-pass) - refractive
draws sample last frame's result, one frame stale. At normal camera
speeds and 60fps that lag is imperceptible; the tradeoff bought a
substantially smaller, lower-risk change (zero `FrameGraph`/
`GeometryPass`-structure changes at all - every existing pass boundary,
barrier, and draw-order decision elsewhere in the frame is untouched).

**Why `refractivePipeline_` uses opaque depth behavior, not
`transparentPipeline_`'s.** Alpha blending (§43) composites via the
fixed-function blend stage, which is why it needs `depthWriteEnable =
false` plus a back-to-front sort - two overlapping blended layers must
combine in the right order. Refraction composites *in the shader*
(sample the background, mix with the surface response, write the result
directly) - there's no fixed-function blending happening at all
(`blendEnable = false`), so ordinary depth test *and* depth write
(`transparent = false` in `VulkanPipeline::create()`'s existing
parameter) already give correct occlusion between refractive instances,
the same way opaque geometry always has. No sort needed for this path.

**Why a separate shader file, not a branch in `triangle.frag`.**
`refractivePipeline_` needs a 3rd descriptor set
(`refractionDescriptor_`, the captured scene color) that
`pipeline_`/`transparentPipeline_` never bind. A `VkPipelineLayout`'s set
count must match what its shader modules statically reference - putting
`layout(set = 2, ...)` in the one shared `triangle.frag` would make
*every* pipeline using it (including the two that never bind a 3rd set)
incompatible with its own 2-set layout. `triangle_refractive.frag` is a
deliberate fork (duplicating the BRDF/shadow/IBL math verbatim, since
this codebase has no shared-GLSL-include mechanism - every other shader
variant here, `irradianceConvolve.frag`/`prefilterEnv.frag`/etc., is
similarly self-contained) with only the final composite differing.
`VulkanPipeline::create()` gained `refractionLayout`/`fragShaderPath`
parameters, both defaulted so `pipeline_`/`transparentPipeline_`'s
existing call sites needed zero changes - the same "add a parameter with
a default, don't touch existing call sites" precedent
`VulkanComputePipeline::create()`'s `shaderPath` parameter set in §31.

**Where IOR and the screen extent live.** `MaterialPushConstants::
metallicRoughness.w` (previously unused) carries IOR - the third time
this session's lineage has repurposed an idle push-constant/UBO slot
instead of growing a struct (`InstanceData.position.w` for §43's sort
key, `MaterialPushConstants.albedo.a` for §43's opacity, now this).
`SceneData.shadowParams.y`/`.z` (previously unused) carry
`sceneColorTarget_`'s width/height in pixels each frame -
`triangle_refractive.frag` divides `gl_FragCoord.xy` by this to recover
its screen UV, since the fragment stage has no view/projection matrix of
its own to derive it from otherwise.

**The refraction offset is an approximation, not a reprojection.** With
no view/projection matrix available in the fragment shader, the exact
screen-space position of a refracted ray's endpoint can't be computed
directly. Instead: build a local screen-tangent basis (`screenRight`/
`screenUp`) from the straight-through view ray `-V`, then measure how far
`refract(-V, N, 1.0/ior)` deviates from `-V` along that basis - bigger
IOR means bigger deviation means a bigger visible bend, which is the
qualitatively correct behavior, but the magnitude is a hand-tuned
constant (`kRefractionStrength = 0.35`, not exposed as a slider) rather
than a physically derived pixel offset. Same "small-angle approximation,
good enough to look right, not exact" bar as §14's screen-space LOD math.

**The barrier sequencing that makes the copy safe.** Two source-image
subtleties, both handled explicitly: (1) `sceneColorTarget_`'s barrier
before the copy uses `oldLayout = SHADER_READ_ONLY_OPTIMAL` (accurate,
because the copy is gated on `sceneColorEverRendered()` - see next
paragraph) and needs no transition back afterward, since the scene render
pass's color attachment has `initialLayout = UNDEFINED`
(`VulkanRenderPass::createOffscreenColor()`, unchanged) and therefore
doesn't care what layout the copy left it in; (2) `sceneColorCopy_`'s
pre-copy barrier uses `oldLayout = UNDEFINED` unconditionally (valid
regardless of its *actual* prior layout, since `UNDEFINED` means "discard
whatever's there" - simpler than tracking whether this is the first copy
ever or the Nth).

**Why `sceneColorEverRendered()` exists at all.** Right after
`initCore()`/`resizeSceneTarget()`, `sceneColorTarget_` is a freshly
created image that has never been rendered into - its real
`VkImageLayout` is `UNDEFINED`, not the `SHADER_READ_ONLY_OPTIMAL` the
pre-copy barrier assumes. Without this guard, the very next frame after
a Viewport-panel resize while refraction is enabled would record a
barrier claiming a layout the image isn't actually in - a validation
error, not just a visual glitch. `FrameRenderer` sets this flag
unconditionally right after the scene render pass ends every frame
(regardless of whether refraction is on), and `resizeSceneTarget()`
resets it to false - by the time a user could realistically toggle
refraction on mid-session, the app has already rendered many frames, so
in practice this only ever gates the one frame immediately following
`initCore()`/a resize.

**Verification.** Rebuilt clean (new shader compiled with no `glslc`
errors), confirmed the default (`refractionEnabled_ = false`) case is
unreachable-by-construction identical to pre-M1 behavior - the entire
copy/barrier/pipeline-selection path is skipped outright. Manually
enabled "Enable Refraction (glass/jelly)" in the "GPU Culling Stats"
window and dragged the IOR slider: visible background distortion through
the grid, IOR-proportional in magnitude, no validation-layer errors and
no crash. Not screenshotted/reverted the way §43/§44's temporary
overrides were - this toggle ships persistently (default off), so
there's nothing to revert.

**Explicitly out of scope, left for M2/M3 (docs/roadmap.md):** mixed
opaque+transparent+refractive instances in the same scene (this is still
one shared global toggle, same "whole grid together" scope §43 already
established); the projectile's draw order relative to refractive grid
instances (always drawn last, same known gap §43 left open); a
real reprojected (not hand-tuned-constant) refraction offset; exposing
`kRefractionStrength` as a slider.

### 46. Mixed opaque + special-material instances (Phase 23 M2), real GPU bucketing over a `discard` shortcut

Closes the M2 gap §45 left open: `gridAlpha()`/`refractionEnabled()` were
each one shared toggle for the *whole* grid - no way to have some
instances stay opaque brick while others are glass/translucent in the
same frame. Every `materialStride()`'th instance (default every 3rd,
ImGui-tunable 1-20) now uses whichever special material those existing
toggles select; the rest stay on `pipeline_` regardless.

**The rejected alternative, and why.** A `discard`-based approach was
considered first and is genuinely simpler: encode the per-instance flag
in the sign of `InstanceData.position.w` (already carrying camera
distance, §43), have `triangle.vert` pass an "isSpecial" flag through,
and `discard` in the fragment shader whichever half doesn't match the
currently-bound pipeline - two full draws of the *same* compacted bucket
per LOD, no new buffers, no `ComputeDescriptor` changes at all. Rejected
because it cuts directly against this project's stated identity (see
roadmap's "Long-term goals": "GPU-driven rendering research direction") -
the entire point of atomic-compaction culling since Phase 5 has been that
the GPU determines the *exact* visible/relevant set and does no wasted
work past that; a `discard`-based split would silently reintroduce
wasted rasterization/shading (~half of every fragment in each of the two
passes) purely to sidestep doing real compaction. At 343 instances the
performance cost either way is unmeasurable, but the *architectural*
statement differs - real bucketing was implemented instead, matching
what the roadmap's M2 plan actually called for ("same bucketing pattern
Phase 6's LOD buckets... already established").

**Where the flag lives, and why `ObjectData` grew instead of reusing a
slot.** Unlike §43's `position.w` and §44/§45's `metallicRoughness.z/w`
- both cases where an existing vec4 had a genuinely idle component -
`ObjectData` (`boundingSphere`, xyz=center/w=radius) has no spare
component; every one of its 4 floats is already load-bearing. Grew it to
a second `vec4 materialFlags` (only `.x` used, `.y/.z/.w` reserved)
rather than packing a bit into an existing float, keeping the "one clear
field per concern" readability this codebase's structs otherwise have.
`objectBuffer_`'s per-instance stride doubled (16→32 bytes) as a direct
consequence - both upload sites (`initSceneData()`'s startup population
and `updateInstanceSimulation()`'s per-frame reupload) needed the local
mirror struct updated identically, the same manual-sync burden every
C++/GLSL struct pair in this codebase already carries (no shared header
exists).

**Bucket doubling, not tripling.** `culling.comp` now writes each LOD
tier into one of *two* buffer pairs (normal/special) based on
`materialFlags.x`, not three (opaque/alpha-blend/refractive as separate
buckets) - "special" is deliberately whatever the *existing* M1 toggles
already select globally, reusing that selection logic entirely rather
than adding a third axis of choice. This kept `ComputeDescriptor`'s
growth to 14→20 bindings instead of needing yet more, and meant
`GeometryPass`'s pipeline-selection code (`activePipeline`, unchanged
from §45) could be reused as-is for the special bucket - M2 only needed
to decide *how many* instances get it, not invent a new way to decide
*which* material it is.

**Why `sortInstances.comp` dispatches all 6 buckets unconditionally.**
The natural design would gate the special buckets' sort behind "mixed
materials on AND the special material is alpha-blend," mirroring the
existing `transparencySortEnabled() && isTransparent()` gate precisely.
Skipped that extra conditional: sorting an empty bucket (mixed off) or
an opaque one (special material is refractive or plain opaque) is a fast
near-no-op - every thread's `tid < count` check is false, the shared-
memory sort still runs its N phases but touches nothing meaningful. Same
"microseconds regardless at this scale" reasoning §43's own header
comment already used to justify the O(n²) sort algorithm choice in the
first place - a second conditional dispatch would have added real code
complexity to avoid a cost that isn't measurable.

**Why `drawGridBucket` exists.** `GeometryPass`'s per-bucket sequence
(bind pipeline, bind set 0/1/(2), push the material, loop 3 LOD draws)
is identical in shape whether it's drawing the normal or special bucket
- only the pipeline, whether set 2 gets bound, which buffer pair to read,
and the LOD draw order differ, all of which are ordinary parameters. A
local lambda capturing `cmd`/`context` avoided duplicating that ~20-line
sequence a second time with only cosmetic differences - this codebase's
existing preference for reuse over near-duplicate blocks (see the
`drawIndexedIndirect` pattern shared across all 3 LOD iterations already
before M2).

**Why the normal bucket's pipeline changes based on `mixed`, not just
the special bucket's.** Before M2, `activePipeline` covered the *whole*
grid - the normal bucket in non-mixed mode has to keep doing that
(`normalPipeline = activePipeline`) for exact backward compatibility,
since the special buffer is guaranteed empty and never drawn. Once mixed
mode is on, the normal bucket's *meaning* changes to "everything that
isn't special," which by this milestone's design is always opaque
(`normalPipeline = pipeline_`) regardless of what `gridAlpha()`/
`refractionEnabled()` currently say - those toggles now only describe the
special subset. Missing this distinction would have meant either the
"normal" majority incorrectly inheriting transparency/refraction it
shouldn't have, or `gridAlpha() < 1.0` silently doing nothing once mixed
mode was toggled on.

**Verification.** Rebuilt clean (both changed shaders compiled with no
`glslc` errors). Confirmed default (`mixedMaterialsEnabled_ = false`) is
unreachable-by-construction identical to pre-M2 behavior - the special
bucket's compaction always writes zero instances (culling.comp's
`isSpecial` branch is never taken when every `materialFlags.x` is 0.0),
so `drawGridBucket`'s special-bucket call is skipped outright by the
`if (mixed)` guard, never even issuing a draw call with an empty buffer.
Enabled mixed materials together with both Grid Alpha < 1 and
Refraction: every `materialStride()`'th instance visibly took the
selected special material while the remainder stayed opaque brick, no
validation errors, no crash, no missing/flickering instances (which
would have been the expected symptom of a bucketing index-math bug).

**Explicitly out of scope, left for M3 (docs/roadmap.md):** the
projectile's draw order relative to grid instances is still unaffected
by this milestone - it's not part of the grid's instance buffer at all,
so "mixed materials" has no bearing on it either way.

### 47. Projectile transparent draw-order interleaving (Phase 23 M3), coarse bucket-level placement over an exact sort

Closes the last gap §43 flagged: the projectile has always been drawn
after the entire grid, unconditionally - correct occlusion for an opaque
or refractive draw (depth test alone resolves it, order is irrelevant),
but wrong the moment the grid is alpha-blended and the projectile is
actually *behind* some of it - it would still render on top, since
nothing established where in the back-to-front sequence it belonged.

**Scope: only engages when draw order can actually matter.**
`projectileNeedsInterleave = transparent && context->projectile().isActive()`
- `transparent` already excludes the refractive case (§45's
`transparent = !refraction && isTransparent()`), so this section's new
code path is provably unreached whenever occlusion is depth-test-only,
which covers the *majority* of states (default opaque, refraction-only,
mixed-with-non-alpha-blend-special). The projectile's pre-M3 "always
last" position is the exact fallback for every one of those, unchanged.

**Turning a screen-size threshold back into a distance.** The LOD
system's thresholds (`lod1ScreenSize()`/`lod2ScreenSize()`, §32) are in
projected pixels, not world-space distance - by design, so they stay
meaningful across FOV/resolution changes. But the projectile isn't going
through `culling.comp`'s LOD bucketing at all (it's a single direct draw,
always LOD2's mesh, §17) - there's no "screen size" for it to compare
against those thresholds directly. Instead, `culling.comp`'s own forward
relationship (`screenSize = radius * screenScale / camDist`, §32) is
solved backward for `camDist` at each threshold:
`distAtBoundary = radius * screenScale / screenSizeThreshold`. This
turns the two screen-size boundaries into two *equivalent* camera
distances, which the projectile's actual `glm::distance(camera,
projectile)` can be compared against directly - reusing the exact same
mesh-detail-independent math already trusted for LOD selection instead
of inventing a parallel, potentially inconsistent, distance-based
scheme. `VulkanContext::boundingSphereRadius()` (previously private,
now a small public accessor) supplies the one missing piece - the same
radius `culling.comp` already uses for every grid instance.

**Coarse, not exact - a deliberate scope match to what "bucket-level"
means.** This finds *which of the 3 LOD steps' distance range* the
projectile falls into, then inserts it at that range's near boundary in
the reversed `2,1,0` draw sequence - it does not merge-sort the
projectile against individual grid instances' `sortInstances.comp`-sorted
positions within a bucket. A projectile positioned between two
specific grid instances *within* the same LOD tier can still draw
slightly out of order relative to those two - accepted, same "the coarse
pass only ever skips work that would have been culled anyway, not a
correctness issue at finer granularity" bar §31 already established for
cluster-level (not per-object) coarse culling. Exactly matching the
roadmap's own M3 plan wording ("which of the reversed `2,1,0` bucket-draw
slots"), not a downgrade from what M3 was scoped to do.

**Why `drawGridBucket` needed `stepFrom`/`stepTo`, not a second
mechanism.** The 3-LOD draw loop already existed inside `drawGridBucket`
(§46) as a single unconditional `for` loop. Rather than writing a
parallel draw path just for the interleaved case, the loop's bounds
became parameters (default `0,3` - the whole bucket, byte-identical to
every pre-M3 call site) - the interleaved case simply calls the same
lambda twice with a split range and the projectile spliced between.
This means the interleaved and non-interleaved paths share every line of
actual Vulkan binding/draw code; only the range differs.

**Why `drawProjectile` needs no pipeline parameter.** Every call site -
standalone at the end (pre-M3 behavior) or spliced mid-bucket (new) -
happens immediately after a `drawGridBucket` call that already bound
`activePipeline` (or, in the non-mixed case, `normalPipeline`, which
*equals* `activePipeline` whenever `mixed` is false - the only case this
section's interleaving path is ever reached from). `drawProjectile`
only needs `activePipeline.getLayout()` for its own set-0 rebind/push/
draw - never `vkCmdBindPipeline` itself - so no parameter was needed
beyond capturing `activePipeline` by reference, the exact same shape the
pre-M3 projectile code already had.

**Verification.** Rebuilt clean. Confirmed the default/opaque and
refraction-only cases are unreachable-by-construction identical to
pre-M3 (`projectileNeedsInterleave` false, falls through to the same
"draw after everything" call as always). With Grid Alpha < 1, flew the
projectile behind a column of transparent grid instances: it now renders
correctly occluded/blended by the nearer transparent instances instead
of always compositing on top, confirmed both in the non-mixed case
(interleaved into the whole grid's single alpha-blend bucket) and
combined with Mixed Materials (interleaved into just the special
bucket, normal bucket unaffected).

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
  `triangle.frag`, using a real sourced CC0 material (ambientCG's
  `Bricks097`) as of §42, not the original self-generated placeholders.
  All 3 LODs have real UV data as of Phase 23 (`assets/suzanne_lod1_uv.obj`/
  `suzanne_lod2_uv.obj`, Blender-decimated from `suzanne_pbr.obj` - closes
  the gap that had needed a UV-preserving decimation tool this environment
  didn't have; see roadmap.md's Phase 8/Phase 23 notes). Material params
  are still shared by all 343 grid instances by default (the projectile
  gets its own distinct push-constant values, and Phase 23 M2 lets a
  configurable subset of the grid diverge too via `mixedMaterialsEnabled()`
  - see §46) — fully independent per-instance material variation beyond
  that is still future work.
- **IBL is complete, all 3 milestones** (§33/§34/§35) — a procedurally
  baked environment cubemap, a live skybox, a diffuse irradiance cubemap,
  a specular-prefiltered mip-chain cubemap, and a BRDF integration LUT
  all exist; `triangle.frag`'s ambient term is now full split-sum
  image-based lighting (diffuse + specular), replacing the original flat
  `0.03 * albedo` constant entirely. Remaining, unrelated limitation: no
  live re-bake on a light-direction change - all of this is baked once
  at startup from whatever `lightDirection_` is at that moment (§33).
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
  (data, not a shader constant), screen-space-projected-size-based since
  §32 (not a flat world-space distance), and `lod2ScreenSize_`'s default
  is mesh-detail-derived since §40 (`lod1ScreenSize_ = 120px` stays a
  manual anchor; `lod2ScreenSize_`'s startup value is now
  `lod1ScreenSize_ * lod2DetailRatio()` ≈ 19.5px for the current assets,
  not an independent hand-picked constant). Both remain live-tunable
  sliders regardless of their derived defaults. A bucket with 0 instances
  this frame still costs a full `vkCmdDrawIndexedIndirect` call — fine at
  3 LOD levels, would need revisiting (e.g. skip empty buckets, or a 4th
  "culled entirely" bucket merge) if the LOD count grows.
- **Hierarchical (coarse + fine) culling is implemented** (§31) — a
  64-cluster coarse pass gates the existing 343-object fine pass. At this
  instance count (6 fine workgroups) it has no measurable performance
  payoff; the value was closing the named architectural gap and
  demonstrating the two-stage GPU-driven pattern correctly, not a perf
  win yet. Remaining gap: cluster membership is static/index-based, not
  re-clustered by proximity, so a heavily-scattered projectile blast
  degrades the coarse pass's rejection efficacy (correctness is
  unaffected — see §31's containment proof).
- **The dockable Viewport target is live-resized as of §36** — dragging
  the panel border now recreates the offscreen scene target at the new
  pixel size (applied at the top of the next frame, not mid-frame) rather
  than stretching a fixed 1280×720 image. No debounce during an active
  drag (a real, accepted per-frame stall while dragging, see §36) and no
  live re-bake of IBL's baked-once assets (§33/§34/§35) - those stay
  correct regardless of viewport size since they're not resolution-
  dependent.
- **The window/swapchain itself is live-resized as of §39** — the GLFW
  window is resizable, `vkAcquireNextImageKHR`/`vkQueuePresentKHR`'s
  out-of-date/suboptimal codes are handled, and a minimized window pauses
  the main loop instead of building an invalid 0-sized swapchain. Same
  no-debounce tradeoff as §36's viewport resize (a `vkDeviceWaitIdle`
  stall per frame while actively dragging the window border).
- **Texture sampling is implemented and validated** (#13), and reunited
  with the primary demo mesh for LOD0 as of Phase 8 milestone 2 (§25) —
  `assets/suzanne_pbr.obj` has real `vt` data, unlike the original
  `suzanne.obj`. LOD1/LOD2 followed as of Phase 23 - see the PBR materials
  item above.
- **Projectile collision is swept as of §41** — the grid-impact test
  sweeps `[previousPosition(), position()]` against every instance's
  collision sphere and finds the earliest hit along that segment, closing
  the discrete point-check tradeoff §20 accepted and §37 later flagged as
  newly reachable. Mutual instance-vs-instance collision (§21/§30) is
  unaffected — it was never the concern this gap named, since it's driven
  by damped velocity impulses, not a single fast-moving projectile.
- **Alpha blending + GPU-side transparency sort landed as of §43** —
  `transparentPipeline_`, `sortInstances.comp`, and `gridAlpha()` exist,
  proven with the shared brick material at reduced alpha. **All 3 gaps
  this section originally left open are now closed, Phase 23:** the
  actual jelly/glass/liquid shading model landed as global-toggle
  refraction/IOR (§45); mixed opaque+transparent scenes landed as
  GPU-bucketed per-instance material selection, off by default (§46);
  and the projectile's blend order relative to the grid is now
  interleaved at the correct coarse position instead of always drawn
  last (§47).
- **Material texture sampling is now a runtime toggle, default off, as
  of §44** — `texturesEnabled()` gates all 4 material texture samples in
  `triangle.frag`; off reproduces Phase 8 milestone 1's flat PBR look
  exactly, on reproduces Phase 20's real brick material exactly. An
  explicit instruction, not this document's own judgment that
  texture-off is the better permanent default (see §44's own
  interview-relevant note).

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

  