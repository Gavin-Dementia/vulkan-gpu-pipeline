#include "vulkan/VulkanContext.h"
#include "vulkan/resource/ObjLoader.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <iostream>
#include <vector>

void VulkanContext::init(GLFWwindow* window)
{
    window_ = window;

    initCore();
    initSceneData();
    initCullingResources();

    imguiLayer_.init(
        window_,
        instance_.get(),
        device_.getPhysical(),
        device_.get(),
        device_.getGraphicsQueueFamily(),
        device_.getGraphicsQueue(),
        renderPass_.get(),
        swapchain_.getImageViews().size()
    );

    std::cout << "Vulkan Context initialized\n";
}

// =========================================================
// Core Vulkan objects: Instance -> Device -> Swapchain ->
// DepthBuffer -> RenderPass -> Pipeline -> Framebuffer
// =========================================================
void VulkanContext::initCore()
{
    instance_.create();
    surface_.create(instance_.get(), window_);

    device_.create(instance_.get(), surface_.get());

    swapchain_.create(
        device_.getPhysical(),
        device_.get(),
        surface_.get(),
        window_
    );

    commandPool_.create(
        device_.get(),
        device_.getGraphicsQueueFamily()
    );

    depthBuffer_.create(
        device_.getPhysical(),
        device_.get(),
        swapchain_.getExtent()
    );

    renderPass_.create(
        device_.get(),
        swapchain_.getImageFormat(),
        depthBuffer_.format()
    );

    uniformBuffer_.create(device_.getPhysical(), device_.get());

    // Phase 8 milestone 2 (see docs/TECHNICAL_NOTES.md): placeholder PBR
    // texture set - normal.png is a flat "no perturbation" (128,128,255)
    // map, metallic_roughness.png follows glTF's G=roughness/B=metallic
    // channel convention, ao.png is flat near-white. Real authored/
    // downloaded texture sets are a stated future swap-in.
    material_.load(
        device_.getPhysical(), device_.get(),
        commandPool_.get(), device_.getGraphicsQueue(),
        "assets/test_texture.png",
        "assets/normal.png",
        "assets/metallic_roughness.png",
        "assets/ao.png"
    );

    // Shared per-frame scene/light data (SceneData.h) - one buffer bound
    // as binding 2 on every material's descriptor set, not duplicated
    // per object, since it's identical for every draw in a given frame.
    sceneDataBuffer_.create(
        device_.getPhysical(), device_.get(), sizeof(SceneData),
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    // Shadow map created here (before both descriptor sets below, which
    // both bind it at binding 3) even though its render pass/framebuffer/
    // pipeline are set up later in this function - only the image/view/
    // sampler need to exist yet.
    shadowMap_.create(device_.getPhysical(), device_.get());

    descriptor_.create(
        device_.get(),
        uniformBuffer_.get(),
        material_,
        sceneDataBuffer_.get(),
        shadowMap_.view(),
        shadowMap_.sampler()
    );

    // Projectile gets its own UBO + descriptor set (same shared texture) -
    // it needs a different model matrix (identity, no spin) than the grid
    // in the same frame, and a single mutable UBO can't hold two different
    // values for two draw calls recorded in the same command buffer (the
    // GPU reads whichever value is in memory at execute time, not at
    // record time - see TECHNICAL_NOTES.md for the full rationale).
    projectileUniformBuffer_.create(device_.getPhysical(), device_.get());
    projectileDescriptor_.create(
        device_.get(),
        projectileUniformBuffer_.get(),
        material_,
        sceneDataBuffer_.get(),
        shadowMap_.view(),
        shadowMap_.sampler()
    );

    // Offscreen scene target: GeometryPass/LightingPass/PostProcess
    // (PassStage::Graphics) render here instead of directly to the
    // swapchain, so ImGui can display the result inside a dockable
    // "Viewport" panel alongside the debug windows instead of everything
    // overlapping the same fullscreen image. Fixed resolution, matching
    // Camera::ASPECT_RATIO's existing "fixed window size" assumption - see
    // TECHNICAL_NOTES.md for why this doesn't need swapchain-style resize
    // handling that doesn't exist anywhere else in this codebase either.
    sceneColorTarget_.create(device_.getPhysical(), device_.get());
    sceneColorDepth_.create(
        device_.getPhysical(), device_.get(), sceneColorTarget_.extent()
    );
    sceneRenderPass_.createOffscreenColor(
        device_.get(), VulkanSceneColorTarget::FORMAT, sceneColorDepth_.format()
    );
    sceneFramebuffer_.create(
        device_.get(),
        sceneRenderPass_.get(),
        { sceneColorTarget_.view() },
        sceneColorDepth_.view(),
        sceneColorTarget_.extent()
    );

    // Geometry pipeline now targets the offscreen scene render pass, not
    // the swapchain's - the swapchain's renderPass_/framebuffer_ below are
    // used only to host the UI-stage pass (see FrameGraph::PassStage::UI).
    pipeline_.create(
        device_.get(),
        sceneColorTarget_.extent(),
        sceneRenderPass_.get(),
        descriptor_.layout()
    );

    framebuffer_.create(
        device_.get(),
        renderPass_.get(),
        swapchain_.getImageViews(),
        depthBuffer_.view(),
        swapchain_.getExtent()
    );

    // Shadow map's image/view/sampler were already created above (before
    // descriptor_/projectileDescriptor_, which both bind it at binding 3);
    // its render pass/framebuffer/pipeline are independent of that and are
    // created here alongside the main pipeline/framebuffer.
    shadowRenderPass_.createDepthOnly(device_.get(), VulkanShadowMap::FORMAT);

    shadowFramebuffer_.createDepthOnly(
        device_.get(),
        shadowRenderPass_.get(),
        shadowMap_.view(),
        shadowMap_.extent()
    );

    shadowPipeline_.create(
        device_.get(),
        shadowMap_.extent(),
        shadowRenderPass_.get()
    );
}

// =========================================================
// Scene data: mesh (Vertex/Index Buffer) + instance grid
// =========================================================
void VulkanContext::initSceneData()
{
    // LOD0 uses a UV/normal-mapped re-export of the same base mesh (Phase 8
    // milestone 2, see docs/TECHNICAL_NOTES.md) so it can actually sample
    // the new texture set; LOD1/LOD2 keep the original UV-less meshes -
    // matching UV-preserving decimations would need a 3D tool this
    // environment doesn't have, so they stay a known, flagged gap rather
    // than blocking this milestone.
    const std::array<std::string, 3> lodPaths = {
        "assets/suzanne_pbr.obj",
        "assets/suzanne_lod1.obj",
        "assets/suzanne_lod2.obj"
    };

    for (int i = 0; i < 3; i++)
    {
        auto mesh = ObjLoader::load(lodPaths[i]);

        lods_[i].vertexBuffer.create(
            device_.getPhysical(), device_.get(),
            commandPool_.get(), device_.getGraphicsQueue(),
            mesh.vertices
        );

        lods_[i].indexBuffer.create(
            device_.getPhysical(), device_.get(),
            commandPool_.get(), device_.getGraphicsQueue(),
            mesh.indices
        );

        // LOD0 is the most detailed variant, so its bounds are the most
        // representative object radius for the (LOD-independent) culling
        // test - LOD1/2 are decimated versions of the same shape and are
        // never larger than LOD0.
        if (i == 0)
        {
            boundingSphereRadius_ = mesh.boundingRadius;
            // Collision volume starts equal to the render/culling bounds
            // (a sensible, mesh-derived default) but is a separate value -
            // see the accessor comment in VulkanContext.h.
            collisionRadius_ = boundingSphereRadius_;
        }
    }

    // 7x7x7 grid, spacing 3.0, centered on origin
    std::vector<InstanceData> instances(OBJECT_COUNT);

    float spacing = 3.0f;
    float halfGrid = (GRID_SIZE - 1) * spacing * 0.5f;

    uint32_t idx = 0;
    for (uint32_t x = 0; x < GRID_SIZE; x++)
    for (uint32_t y = 0; y < GRID_SIZE; y++)
    for (uint32_t z = 0; z < GRID_SIZE; z++)
    {
        glm::vec3 pos = {
            x * spacing - halfGrid,
            y * spacing - halfGrid,
            z * spacing - halfGrid
        };
        instances[idx++].position = glm::vec4(pos, 1.0f);
    }

    // Cache instance world positions for object buffer setup
    cachedInstances_ = std::move(instances);

    // Single-entry instance buffer for the mouse-fired projectile - the
    // graphics pipeline's vertex input layout always expects something
    // bound at binding 1 (per-instance position), even for a non-instanced
    // draw of one object. No STORAGE_BUFFER_BIT needed since nothing
    // writes it via compute - it's just a CPU-updated translation.
    projectileInstanceBuffer_.create(
        device_.getPhysical(), device_.get(), sizeof(InstanceData),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
}

// =========================================================
// GPU culling resources: ObjectBuffer, VisibleInstanceBuffer,
// IndirectDrawBuffer, FrustumBuffer, ComputeDescriptor/Pipeline
// =========================================================
void VulkanContext::initCullingResources()
{
    struct ComputeObjectData { glm::vec4 boundingSphere; };  // xyz=center, w=radius

    std::vector<ComputeObjectData> objects(OBJECT_COUNT);

    for (uint32_t i = 0; i < OBJECT_COUNT; i++)
    {
        objects[i].boundingSphere = glm::vec4(
            glm::vec3(cachedInstances_[i].position),
            boundingSphereRadius_
        );
    }

    VkDeviceSize objSize = sizeof(ComputeObjectData) * OBJECT_COUNT;

    // VERTEX_BUFFER_BIT alongside STORAGE_BUFFER_BIT: the shadow pass
    // (FrameRenderer.cpp) binds this same buffer directly as its
    // per-instance vertex input, reusing culling.comp's per-frame
    // bounding-sphere data instead of a separate buffer - see
    // architecture.md's shadow mapping notes.
    objectBuffer_.create(
        device_.getPhysical(), device_.get(), objSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    objectBuffer_.upload(device_.get(), objects.data(), objSize);

    // Output buffer sized for worst case: all instances visible
    VkDeviceSize visibleInstanceSize = sizeof(InstanceData) * OBJECT_COUNT;
    VkDeviceSize indirectDrawSize    = sizeof(DrawIndirectCommand);

    for (int i = 0; i < 3; i++)
    {
        lods_[i].visibleInstanceBuffer.create(
            device_.getPhysical(), device_.get(), visibleInstanceSize,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        );

        // 每个LOD的indexCount不同
        uint32_t lodIndexCount = lods_[i].indexBuffer.indexCount();
        lods_[i].indirectDrawBuffer.create(
            device_.getPhysical(), device_.get(), lodIndexCount
        );
    }

    // Frustum buffer: 6 planes + cameraPos + lodParams = 8 vec4 = 128 bytes
    VkDeviceSize frustumSize = sizeof(FrustumPlanes);

    frustumBuffer_.create(
        device_.getPhysical(), device_.get(), frustumSize,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    // Shadow pass's light-frustum-culled instance set - same shapes as a
    // single LOD's {visibleInstanceBuffer, indirectDrawBuffer} above, plus
    // its own frustum UBO (only planes[6] is used - camera pos/lodParams
    // are meaningless for the light, but reusing FrustumPlanes keeps the
    // std140 layout identical to the camera frustum's, no separate GLSL
    // struct needed). Sized for worst case (all OBJECT_COUNT visible),
    // indexed by LOD0's mesh - the shadow pass always draws LOD0 geometry
    // regardless of camera distance.
    shadowVisibleInstanceBuffer_.create(
        device_.getPhysical(), device_.get(), visibleInstanceSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    shadowIndirectDrawBuffer_.create(
        device_.getPhysical(), device_.get(), lods_[0].indexBuffer.indexCount()
    );
    lightFrustumBuffer_.create(
        device_.getPhysical(), device_.get(), frustumSize,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    // Hierarchical culling (coarse pass) - 64 cluster bounding spheres,
    // re-uploaded every frame from the CPU (see updateInstanceSimulation()),
    // plus the coarse shader's two output flag buffers. HOST_VISIBLE like
    // every other compute SSBO in this codebase, not DEVICE_LOCAL - the
    // two flag buffers are also read back on the CPU every frame for the
    // "GPU Culling Stats" ImGui counts, same safe post-fence-wait readback
    // FrameRenderer.cpp already uses for the LOD visible counts.
    VkDeviceSize clusterBufferSize  = sizeof(glm::vec4) * CLUSTER_COUNT;
    VkDeviceSize clusterVisibleSize = sizeof(uint32_t) * CLUSTER_COUNT;

    clusterBuffer_.create(
        device_.getPhysical(), device_.get(), clusterBufferSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    clusterVisibleCameraBuffer_.create(
        device_.getPhysical(), device_.get(), clusterVisibleSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );
    clusterVisibleLightBuffer_.create(
        device_.getPhysical(), device_.get(), clusterVisibleSize,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    std::array<VkBuffer, 3> visibleBufs = {
        lods_[0].visibleInstanceBuffer.get(),
        lods_[1].visibleInstanceBuffer.get(),
        lods_[2].visibleInstanceBuffer.get()
    };

    std::array<VkBuffer, 3> indirectBufs = {
        lods_[0].indirectDrawBuffer.get(),
        lods_[1].indirectDrawBuffer.get(),
        lods_[2].indirectDrawBuffer.get()
    };

    computeDescriptor_.create(
        device_.get(),
        objectBuffer_.get(),
        visibleBufs, indirectBufs,
        frustumBuffer_.get(),
        shadowVisibleInstanceBuffer_.get(),
        shadowIndirectDrawBuffer_.get(),
        lightFrustumBuffer_.get(),
        clusterBuffer_.get(),
        clusterVisibleCameraBuffer_.get(),
        clusterVisibleLightBuffer_.get(),
        objSize, visibleInstanceSize,
        indirectDrawSize, frustumSize,
        clusterBufferSize, clusterVisibleSize
    );

    computePipeline_.create(
        device_.get(), computeDescriptor_.layout(),
        "shaders/compiled/culling.comp.spv");
    computePipelineCoarse_.create(
        device_.get(), computeDescriptor_.layout(),
        "shaders/compiled/cullingCoarse.comp.spv");

    // cachedInstances_ is kept alive as the permanent rest formation
    // (used by resetInstanceFormation()) instead of being freed here.
    instanceCurrentPositions_.resize(OBJECT_COUNT);
    instanceVelocities_.assign(OBJECT_COUNT, glm::vec3(0.0f));
    for (uint32_t i = 0; i < OBJECT_COUNT; i++)
        instanceCurrentPositions_[i] = glm::vec3(cachedInstances_[i].position);
}

// =========================================================
// Grid collision + scatter (Phase 7 milestone 2)
// =========================================================
void VulkanContext::updateInstanceSimulation(float deltaTime)
{
    constexpr float kDampingPerSecond = 0.05f;   // fraction of velocity retained after 1 full second
    constexpr float kProjectileRadius = 0.3f;
    constexpr float kBlastRadius      = 6.0f;
    constexpr float kImpulseStrength  = 8.0f;

    float dampingFactor = std::pow(kDampingPerSecond, deltaTime);   // framerate-independent decay

    for (uint32_t i = 0; i < OBJECT_COUNT; i++)
    {
        instanceCurrentPositions_[i] += instanceVelocities_[i] * deltaTime;
        instanceVelocities_[i] *= dampingFactor;
    }

    if (projectile_.isActive())
    {
        glm::vec3 projPos = projectile_.position();
        float hitDist = collisionRadius_ + kProjectileRadius;   // collision volume, not the render/culling radius

        for (uint32_t i = 0; i < OBJECT_COUNT; i++)
        {
            if (glm::length(instanceCurrentPositions_[i] - projPos) < hitDist)
            {
                // Blast: radial push falling off with distance from the
                // impact point, applied to every instance within range -
                // not just the one instance that was actually touched.
                for (uint32_t j = 0; j < OBJECT_COUNT; j++)
                {
                    glm::vec3 offset = instanceCurrentPositions_[j] - projPos;
                    float dist = glm::length(offset);
                    if (dist < kBlastRadius)
                    {
                        glm::vec3 dir = (dist > 0.001f) ? (offset / dist) : glm::vec3(0.0f, 1.0f, 0.0f);
                        float falloff = 1.0f - (dist / kBlastRadius);
                        instanceVelocities_[j] += dir * kImpulseStrength * falloff;
                    }
                }
                projectile_.stop();
                break;   // one explosion per flight
            }
        }
    }

    // Mutual collision: resolve overlaps between instances themselves,
    // not just projectile-vs-instance. Without this, scattered instances
    // that drift close to each other (or to still-resting neighbors -
    // grid spacing is only 3.0 against a ~1.49 bounding radius, so
    // resting instances already sit just 0.03 units apart at closest)
    // visibly clip through each other once their blast velocity settles.
    //
    // Hybrid response (§30): a velocity impulse (equal-mass, restitution-
    // scaled, along the contact normal) handles the visible "bounce apart"
    // motion, applied only while a pair is actually approaching
    // (velAlongNormal < 0) - a separating or already-resting pair gets no
    // impulse, since an impulse can't push apart two objects with ~zero
    // relative velocity. That's exactly the case the positional-pushout
    // step below still exists for: it's now a much lighter safety net
    // (10% of the gap per side, not the pre-impulse 30%) whose only job is
    // guaranteeing eventual separation for resting overlap, not doing the
    // primary separating - the impulse does that dynamically now.
    // Deliberately tuned loose either way, not a strict non-overlap
    // constraint: minSeparation (1.5x radius, not the geometrically "just
    // touching" 2x) tolerates some visual overlap for a denser scatter
    // look. Both steps run every frame (not just on impact) in the same
    // O(n^2) unique-pair pass, so partial correction converges over a few
    // frames rather than needing a one-shot solver - same "converges over
    // frames" reasoning §21 already established, just with the dynamic
    // part now handled by momentum instead of a bigger position snap.
    {
        float minSeparation = 1.5f * boundingSphereRadius_;
        constexpr float kPositionalCorrectionFactor = 0.1f;   // was 0.3f pre-impulse
        for (uint32_t i = 0; i < OBJECT_COUNT; i++)
        {
            for (uint32_t j = i + 1; j < OBJECT_COUNT; j++)
            {
                glm::vec3 delta = instanceCurrentPositions_[j] - instanceCurrentPositions_[i];
                float dist = glm::length(delta);
                if (dist < minSeparation)
                {
                    glm::vec3 n = (dist > 0.001f) ? (delta / dist) : glm::vec3(0.0f, 1.0f, 0.0f);

                    // Equal-mass impulse (every grid instance shares
                    // boundingSphereRadius_, so mass is implicitly 1:1):
                    // -(1+e)*velAlongNormal/2 along the normal, only when
                    // approaching.
                    glm::vec3 relVel = instanceVelocities_[j] - instanceVelocities_[i];
                    float velAlongNormal = glm::dot(relVel, n);
                    if (velAlongNormal < 0.0f)
                    {
                        glm::vec3 impulse = -(1.0f + restitution_) * velAlongNormal * 0.5f * n;
                        instanceVelocities_[i] -= impulse;
                        instanceVelocities_[j] += impulse;
                    }

                    float overlap = minSeparation - dist;
                    instanceCurrentPositions_[i] -= n * (overlap * kPositionalCorrectionFactor);
                    instanceCurrentPositions_[j] += n * (overlap * kPositionalCorrectionFactor);
                }
            }
        }
    }

    // Re-upload the (possibly changed) positions - cheap, persistently
    // mapped memcpy (see the VulkanBuffer persistent-mapping commit).
    // A bare glm::vec4 is byte-identical to the local ComputeObjectData
    // struct above (single member, no padding), safe to upload directly.
    std::vector<glm::vec4> objects(OBJECT_COUNT);
    for (uint32_t i = 0; i < OBJECT_COUNT; i++)
        objects[i] = glm::vec4(instanceCurrentPositions_[i], boundingSphereRadius_);

    objectBuffer_.upload(device_.get(), objects.data(), sizeof(glm::vec4) * OBJECT_COUNT);

    // Hierarchical culling (coarse pass): recompute each cluster's
    // enclosing bounding sphere from the live positions above, same
    // "recompute CPU-side, reupload the SSBO every frame" discipline as
    // objectBuffer_ just above - O(2*OBJECT_COUNT), negligible next to the
    // O(n^2) mutual-collision pass this function already runs. Center is
    // the member mean; radius is the max member distance-from-center plus
    // that member's own bounding radius, so the cluster sphere strictly
    // contains every member sphere (see docs/TECHNICAL_NOTES.md for why
    // that containment property is what makes the coarse gate lossless).
    {
        std::vector<glm::vec3> clusterCenterSum(CLUSTER_COUNT, glm::vec3(0.0f));
        std::vector<uint32_t>  clusterMemberCount(CLUSTER_COUNT, 0);

        for (uint32_t i = 0; i < OBJECT_COUNT; i++)
        {
            uint32_t c = clusterIndexForInstance(i);
            clusterCenterSum[c] += instanceCurrentPositions_[i];
            clusterMemberCount[c]++;
        }

        std::vector<glm::vec4> clusters(CLUSTER_COUNT, glm::vec4(0.0f));
        for (uint32_t c = 0; c < CLUSTER_COUNT; c++)
        {
            if (clusterMemberCount[c] > 0)
                clusters[c] = glm::vec4(clusterCenterSum[c] / float(clusterMemberCount[c]), 0.0f);
        }

        for (uint32_t i = 0; i < OBJECT_COUNT; i++)
        {
            uint32_t c = clusterIndexForInstance(i);
            glm::vec3 center = glm::vec3(clusters[c]);
            float reach = glm::length(instanceCurrentPositions_[i] - center) + boundingSphereRadius_;
            clusters[c].w = std::max(clusters[c].w, reach);
        }

        clusterBuffer_.upload(device_.get(), clusters.data(), sizeof(glm::vec4) * CLUSTER_COUNT);
    }
}

uint32_t VulkanContext::clusterIndexForInstance(uint32_t idx)
{
    // Must match culling.comp's GLSL recovery exactly, and
    // initSceneData()'s x/y/z generation loop order (idx = x*49+y*7+z).
    uint32_t x = idx / (GRID_SIZE * GRID_SIZE);
    uint32_t y = (idx / GRID_SIZE) % GRID_SIZE;
    uint32_t z = idx % GRID_SIZE;
    return (x / CLUSTER_DIM) * (CLUSTERS_PER_AXIS * CLUSTERS_PER_AXIS)
         + (y / CLUSTER_DIM) * CLUSTERS_PER_AXIS
         + (z / CLUSTER_DIM);
}

void VulkanContext::resetInstanceFormation()
{
    for (uint32_t i = 0; i < OBJECT_COUNT; i++)
        instanceCurrentPositions_[i] = glm::vec3(cachedInstances_[i].position);
    std::fill(instanceVelocities_.begin(), instanceVelocities_.end(), glm::vec3(0.0f));
}

glm::mat4 VulkanContext::lightViewProj() const
{
    // Scene bounding radius: derived from the live instance positions
    // every frame instead of a fixed constant, so a projectile blast
    // (§20/§21 - permanent scatter, no auto-return to rest) that pushes
    // instances outward grows the light's frustum to match, rather than
    // silently clipping them out of the shadow map (and, since §28 added
    // light-frustum culling, out of the shadow pass's draw entirely).
    // kMinSceneRadius is the floor this used to be a fixed constant at:
    // the 7x7x7 grid's rest half-extent is (GRID_SIZE-1)*spacing*0.5 =
    // 9.0 per axis (spacing=3.0, see initSceneData()), a ~15.6-unit
    // half-diagonal, plus margin - so the rest-formation case (no blast
    // yet) computes the exact same 24.0 this constant used to be, making
    // this change a no-op until a blast actually happens. Recomputed
    // fresh every frame (an O(343) scan, same order as the O(n^2)
    // mutual-collision pass updateInstanceSimulation() already runs every
    // frame) rather than smoothed/quantized - accepted minor shadow-map
    // texel-density shift ("shadow swimming") in exchange for staying a
    // simple, stateless function of current positions. Grid instances
    // only; the projectile isn't included (short-lived, single instance,
    // not worth the extra coupling if its shadow gets clipped while far
    // outside the grid's footprint).
    constexpr float kMinSceneRadius = 24.0f;

    float maxDist = 0.0f;
    for (const auto& p : instanceCurrentPositions_)
        maxDist = std::max(maxDist, glm::length(p));

    float sceneRadius = std::max(kMinSceneRadius, maxDist + boundingSphereRadius_);

    glm::vec3 dir = glm::normalize(lightDirection_);
    glm::vec3 center(0.0f);
    glm::vec3 eye = center - dir * (sceneRadius * 2.0f);

    // glm::lookAt degenerates when the view direction is parallel to the
    // up vector - the default light direction points mostly straight
    // down, so fall back to a different up axis in that case.
    glm::vec3 up = (std::abs(dir.y) > 0.99f)
        ? glm::vec3(0.0f, 0.0f, 1.0f)
        : glm::vec3(0.0f, 1.0f, 0.0f);

    glm::mat4 view = glm::lookAt(eye, center, up);

    // orthoRH_ZO (not the plain glm::ortho()/Camera::getProjectionMatrix()
    // convention) - Vulkan needs z_ndc in [0,1], not OpenGL's [-1,1]. For a
    // *perspective* matrix that distinction only shifts the near plane by
    // a fraction of a unit (harmless, and why Camera's perspective gets
    // away with plain glm::perspective + a Y-flip) - but for an
    // *orthographic* matrix the mapping is linear, so using the [-1,1]
    // convention here would clip away the near half of the light's
    // frustum outright. This project doesn't define
    // GLM_FORCE_DEPTH_ZERO_TO_ONE globally, so the explicit *_ZO variant
    // is used instead of changing GLM's project-wide default.
    glm::mat4 proj = glm::orthoRH_ZO(
        -sceneRadius, sceneRadius,
        -sceneRadius, sceneRadius,
        0.1f, sceneRadius * 4.0f
    );
    proj[1][1] *= -1;   // Vulkan Y-flip, same as Camera::getProjectionMatrix()

    return proj * view;
}

void VulkanContext::cleanup()
{
    imguiLayer_.destroy(device_.get());
    computePipeline_.destroy(device_.get());
    computePipelineCoarse_.destroy(device_.get());
    computeDescriptor_.destroy(device_.get());
    frustumBuffer_.destroy(device_.get());
    objectBuffer_.destroy(device_.get());
    lightFrustumBuffer_.destroy(device_.get());
    shadowIndirectDrawBuffer_.destroy(device_.get());
    shadowVisibleInstanceBuffer_.destroy(device_.get());
    clusterBuffer_.destroy(device_.get());
    clusterVisibleCameraBuffer_.destroy(device_.get());
    clusterVisibleLightBuffer_.destroy(device_.get());

    for (int i = 0; i < 3; i++)
    {
        lods_[i].indirectDrawBuffer.destroy(device_.get());
        lods_[i].visibleInstanceBuffer.destroy(device_.get());
        lods_[i].indexBuffer.destroy(device_.get());
        lods_[i].vertexBuffer.destroy(device_.get());
    }

    material_.destroy(device_.get());
    projectileDescriptor_.destroy(device_.get());
    projectileUniformBuffer_.destroy(device_.get());
    projectileInstanceBuffer_.destroy(device_.get());
    sceneDataBuffer_.destroy(device_.get());
    descriptor_.destroy(device_.get());
    uniformBuffer_.destroy(device_.get());

    pipeline_.destroy(device_.get());
    sceneFramebuffer_.destroy(device_.get());
    sceneRenderPass_.destroy(device_.get());
    sceneColorDepth_.destroy(device_.get());
    sceneColorTarget_.destroy(device_.get());

    framebuffer_.destroy(device_.get());
    depthBuffer_.destroy(device_.get());
    renderPass_.destroy(device_.get());

    shadowPipeline_.destroy(device_.get());
    shadowFramebuffer_.destroy(device_.get());
    shadowRenderPass_.destroy(device_.get());
    shadowMap_.destroy(device_.get());

    swapchain_.destroy(device_.get());
    commandPool_.destroy(device_.get());

    device_.destroy();
    surface_.destroy(instance_.get());
    instance_.destroy();

    std::cout << "VulkanContext destroyed\n";
}

