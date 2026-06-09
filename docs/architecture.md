# Architecture Overview

This document describes the high-level architecture of the renderer.

---

# Current Architecture

Application

↓

VulkanContext

↓

VulkanInstance
VulkanSurface
VulkanDevice
VulkanSwapchain

---

# Module Responsibilities

## Application

Responsible for:

* Application lifecycle
* Main loop
* Window ownership

Not responsible for:

* Vulkan resource management

---

## VulkanContext

Central manager of Vulkan subsystems.

Responsibilities:

* Initialization order
* Resource ownership
* System shutdown

Owns:

* VulkanInstance
* VulkanSurface
* VulkanDevice
* VulkanSwapchain

---

## VulkanInstance

Responsible for:

* VkInstance creation
* Validation layer setup
* Debug messenger

---

## VulkanSurface

Responsible for:

* GLFW surface creation
* Platform abstraction

---

## VulkanDevice

Responsible for:

* Physical device selection
* Queue family discovery
* Logical device creation

---

## VulkanSwapchain

Responsible for:

* Swapchain creation
* Image views
* Presentation configuration

---

# Current Dependency Graph

Application
└── VulkanContext
├── VulkanInstance
├── VulkanSurface
├── VulkanDevice
└── VulkanSwapchain

---

# Planned Frame Pipeline

Acquire Swapchain Image
        ↓
Record Command Buffer
        ↓
Queue Submit
        ↓
Present


