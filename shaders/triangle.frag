#version 450

layout(location = 0) in vec3 fragNormal;
layout(location = 0) out vec4 outColor;

void main()
{//use normal
    vec3 color = normalize(fragNormal) * 0.5 + 0.5;
    outColor = vec4(color, 1.0);
}

