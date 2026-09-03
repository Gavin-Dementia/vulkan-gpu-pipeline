#version 450

// gl_VertexIndex-driven fullscreen triangle - no vertex/index buffer, no
// uniform/push-constant input. Covers the full [-1,1] NDC square (the
// third vertex extends past the clip volume on purpose - the rasterizer
// clips it, and the visible area is still exactly one full-screen quad
// with no seam down the middle, unlike a two-triangle quad). Shared by
// every fullscreen-pass pipeline in this codebase that needs one (the
// IBL environment capture bake and the live skybox draw - see
// docs/TECHNICAL_NOTES.md) since nothing here is pipeline-specific.
layout(location = 0) out vec2 fragNDC;

void main()
{
    vec2 positions[3] = vec2[](vec2(-1.0, -1.0), vec2(3.0, -1.0), vec2(-1.0, 3.0));
    fragNDC = positions[gl_VertexIndex];
    gl_Position = vec4(fragNDC, 1.0, 1.0);
}
