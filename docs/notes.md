
PS D:\RenderPipeline\vulkan-gpu-pipeline> 
git submodule add https://github.com/glfw/glfw third_party/glfw
git submodule add https://github.com/g-truc/glm third_party/glm

https://vulkan.lunarg.com/sdk/home
1.4.350.0 12-May-2026
vulkan  3-4 G require
install components
This installs the GLM headers.
Install debug shader toolchain libraries.
This installs the VMA header file.

RUN the vulkan confgurator and hardware capabilities viewer

cmake -S . -B build