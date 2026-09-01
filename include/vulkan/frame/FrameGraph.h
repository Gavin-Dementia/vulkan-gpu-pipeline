#pragma once

#include <vector>
#include <functional>
#include <vulkan/vulkan.h>
#include <unordered_map>
#include <iostream>

class VulkanContext;

enum class PassStage
{
    Compute,
    Shadow,
    Graphics
};

struct RGPass
{
    std::string name;

    std::vector<int> reads;

    // GPU command
    std::function<void(VkCommandBuffer)> execute;
    PassStage stage = PassStage::Graphics;  
};

class FrameGraph
{
public:
    void init(VulkanContext* ctx);

    int addPass(const RGPass& pass);

    void build();   // build DAG order

    void executeCompute(VkCommandBuffer cmd);   // 在RenderPass外执行
    void executeShadow(VkCommandBuffer cmd);    // 在shadow RenderPass内执行
    void executeGraphics(VkCommandBuffer cmd);  // 在主RenderPass内执行

private:
    VulkanContext* context = nullptr;

    std::vector<RGPass> passes;
    std::vector<int> executionOrder;
};

