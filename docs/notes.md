# Development Notes

## TODO

Investigate:

* Command Buffer lifecycle
* Queue submission model
* Synchronization primitives

---

## 2026-06-09

Completed:

* Physical Device Selection
* Logical Device Creation
* Swapchain Creation

Observation:

Swapchain creation depends heavily on surface capabilities.

Important concepts:

* Surface format
* Present mode
* Image count

---

## 2026-06-08

Successfully initialized:

* VkInstance
* Validation Layers
* GLFW Surface

Observation:

VkInstance is effectively the entry point into the Vulkan runtime.

