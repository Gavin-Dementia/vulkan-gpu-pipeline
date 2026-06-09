# Renderer Roadmap

This document tracks the planned development stages of the project.

---

# Phase 0 — Vulkan Bootstrap

Status: Complete

Implemented:

* GLFW window
* Vulkan instance
* Validation layers
* Surface creation
* Physical device selection
* Logical device creation
* Swapchain creation

---

# Phase 1 — Frame Presentation

Goal:

Present swapchain images correctly.

Planned:

* Command Pool
* Command Buffer
* Fence
* Semaphore
* Acquire / Submit / Present

Milestone:

First presented frame.

---

# Phase 2 — First Triangle

Goal:

Render first geometry.

Planned:

* Render Pass
* Framebuffer
* Graphics Pipeline
* Shader Modules

Milestone:

Triangle rendering.

---

# Phase 3 — Mesh Rendering

Goal:

Render indexed meshes.

Planned:

* Vertex Buffer
* Index Buffer
* Uniform Buffer
* Descriptor Sets

Milestone:

Mesh rendering system.

---

# Phase 4 — Scene Framework

Goal:

Build renderer foundation.

Planned:

* Camera
* Transform
* Material
* Scene Graph

Milestone:

Mini renderer.

---

# Phase 5 — GPU Driven Rendering

Goal:

Reduce CPU-side draw submission.

Planned:

* Storage Buffers
* Indirect Draw
* Compute-driven visibility

Milestone:

GPU-driven rendering pipeline.

---

# Long-Term Goals

* Modern Vulkan architecture
* GPU-driven rendering research
* Foundation for future rendering experiments
