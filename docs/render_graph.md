# Render Graph Architecture

> This document originally tracked the Render Graph design while it was
> still an MVP idea ("linear execution, no DAG yet"). The system has
> since been implemented as a full DAG with Compute/Shadow/Graphics/UI
> stage separation — see the detailed design rationale in
> [`TECHNICAL_NOTES.md`](./TECHNICAL_NOTES.md), specifically:
>
> - §1 — FrameGraph as a DAG, not a fixed pipeline
> - §4 — Compute and Graphics as separate FrameGraph stages
> - §22 — adding a third stage, Shadow, for the shadow map pass
> - §24 — adding a fourth stage, UI, for the dockable ImGui viewport
>
> and the current module breakdown in
> [`architecture.md`](./architecture.md) under **FrameGraph**.

This stub is kept so old links/references to `render_graph.md` don't
break, but `TECHNICAL_NOTES.md` is now the canonical source for design
rationale on this system.

