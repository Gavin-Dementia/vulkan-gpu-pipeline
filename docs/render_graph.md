# Render Graph Architecture

## Overview
This document defines the Render Graph system for Vulkan pipeline abstraction.

## Core Idea
Render Graph is a declarative rendering system that replaces manual frame execution logic.

## Components
- RenderGraph
- RenderPassNode
- Resource dependency system (future)

## Execution Flow
1. Declare passes
2. Build dependency graph
3. Execute in topological order

## Current Stage
MVP: Linear execution (no DAG yet)
