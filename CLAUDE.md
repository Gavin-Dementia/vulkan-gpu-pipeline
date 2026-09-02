# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Where to look

- **Build, dependencies, project layout, troubleshooting:** `docs/setup.md`
- **Controls, feature list, demo:** `README.md`
- **Current architecture / module responsibilities — read before any non-trivial rendering-pipeline change:** `docs/architecture.md`
- **Why something was built the way it was, past bugs and their root causes:** `docs/TECHNICAL_NOTES.md`
- **What's implemented vs. planned, per phase:** `docs/roadmap.md`

Keep `docs/architecture.md`, `docs/TECHNICAL_NOTES.md`, and `docs/roadmap.md` in sync with the code when architecture changes — this repo has previously drifted (see `docs/TECHNICAL_NOTES.md` §15) and the drift itself had to be found and fixed.

## Notes not covered elsewhere

- Each **git bash(commit, push etc.)** will effect the history of this respository should get approval by user.
- No lint config and no automated test suite exist in this repo. Verification is: does it build cleanly, and does the running app look/behave correctly (culling/LOD/lighting/collision are all inherently visual).
- stdout is fully buffered when redirected to a file/pipe rather than a console, so a force-killed background run may show an empty captured log even on a successful start.
- `CMakeLists.txt` uses **explicit source file lists** (no globbing), grouped into `set(...)` variables per subsystem — adding a new `.cpp` file requires adding it to the relevant list, not just creating the file.
- Every `HOST_VISIBLE` buffer in this codebase is also created `HOST_COHERENT` and persistently mapped by `VulkanBuffer` — don't add a `HOST_VISIBLE`-only buffer without also handling that.
