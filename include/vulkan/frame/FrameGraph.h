#pragma once
// #include "vulkan/frame/FrameContext.h"

#include <vector>
#include <functional>
#include <vulkan/vulkan.h>
#include <unordered_map>
#include <iostream>

class VulkanContext;

struct RGPass
{
    std::string name;

    std::vector<int> reads;
    std::vector<int> writes;

    VkPipeline pipeline = VK_NULL_HANDLE;

    // GPU command
    std::function<void(VkCommandBuffer)> execute;
};

class FrameGraph
{
public:
    void init(VulkanContext* ctx);

    int addPass(const RGPass& pass);

    void build();   // build DAG order
    void execute(VkCommandBuffer cmd);

private:
    VulkanContext* context = nullptr;

    std::vector<RGPass> passes;
    std::vector<int> executionOrder;

    void topologicalSort(){};
};

