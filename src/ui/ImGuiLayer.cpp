#include "ui/ImGuiLayer.h"
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include <stdexcept>
#include <array>

void ImGuiLayer::init(
    GLFWwindow* window,
    VkInstance instance,
    VkPhysicalDevice physical,
    VkDevice device,
    uint32_t queueFamily,
    VkQueue queue,
    VkRenderPass renderPass,
    uint32_t imageCount)
{
    std::array<VkDescriptorPoolSize, 3> poolSizes = {{
        { VK_DESCRIPTOR_TYPE_SAMPLER,                4 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,          4 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16 }
    }};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags         = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets       = 16;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes    = poolSizes.data();

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool_) != VK_SUCCESS)
        throw std::runtime_error("Failed to create ImGui descriptor pool");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();

    // Lets the debug windows (GPU Culling Stats / Lighting / Shadow Map)
    // and the scene "Viewport" panel dock against each other and the
    // window edges instead of floating as separate overlapping windows.
    // Deliberately not enabling ImGuiConfigFlags_ViewportsEnable - windows
    // stay inside the one GLFW window, not spawned as separate OS windows.
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui_ImplGlfw_InitForVulkan(window, true);

    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.Instance       = instance;
    initInfo.PhysicalDevice = physical;
    initInfo.Device         = device;
    initInfo.QueueFamily    = queueFamily;
    initInfo.Queue          = queue;
    initInfo.DescriptorPool = pool_;
    initInfo.MinImageCount  = imageCount;
    initInfo.ImageCount     = imageCount;

    // 2025/09/26
    // RenderPass/Subpass/MSAASamples MOVE INTO PipelineInfoMain
    initInfo.PipelineInfoMain.RenderPass   = renderPass;
    initInfo.PipelineInfoMain.Subpass      = 0;
    initInfo.PipelineInfoMain.MSAASamples  = VK_SAMPLE_COUNT_1_BIT;

    ImGui_ImplVulkan_Init(&initInfo);
}

void ImGuiLayer::destroy(VkDevice device)
{
    ImGui_ImplVulkan_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    vkDestroyDescriptorPool(device, pool_, nullptr);
}

void ImGuiLayer::beginFrame()
{
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
}

void ImGuiLayer::render(VkCommandBuffer cmd)
{
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();
    ImGui_ImplVulkan_RenderDrawData(drawData, cmd);
}

