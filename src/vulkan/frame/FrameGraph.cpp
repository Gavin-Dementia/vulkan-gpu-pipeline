#include "vulkan/frame/FrameGraph.h"
#include <queue>
#include <stdexcept>

void FrameGraph::init(VulkanContext* ctx)
{
    context = ctx;
}

int FrameGraph::addPass(const RGPass& pass)
{
    passes.push_back(pass);
    return (int)passes.size() - 1;
}

void FrameGraph::build()
{
    int N = (int)passes.size();

    std::vector<int> indegree(N, 0);
    std::vector<std::vector<int>> graph(N);

    // build adjacency list
    for (int i = 0; i < N; i++)
    {
        for (int dep : passes[i].reads)
        {
            graph[dep].push_back(i);
            indegree[i]++;
        }
    }

    std::queue<int> q;

    for (int i = 0; i < N; i++)
        if (indegree[i] == 0)
            q.push(i);

    executionOrder.clear();

    while (!q.empty())
    {
        int cur = q.front();
        q.pop();

        executionOrder.push_back(cur);

        for (int nxt : graph[cur])
        {
            indegree[nxt]--;
            if (indegree[nxt] == 0)
                q.push(nxt);
        }
    }

    if (executionOrder.size() != passes.size())
        throw std::runtime_error("FrameGraph has cycle!");
}

void FrameGraph::executeCompute(VkCommandBuffer cmd)
{
    for (int idx : executionOrder)
    {
        auto& pass = passes[idx];
        if (pass.stage == PassStage::Compute && pass.execute)
            pass.execute(cmd);
    }
}

void FrameGraph::executeShadow(VkCommandBuffer cmd)
{
    for (int idx : executionOrder)
    {
        auto& pass = passes[idx];
        if (pass.stage == PassStage::Shadow && pass.execute)
            pass.execute(cmd);
    }
}

void FrameGraph::executeGraphics(VkCommandBuffer cmd)
{
    for (int idx : executionOrder)
    {
        auto& pass = passes[idx];
        if (pass.stage == PassStage::Graphics && pass.execute)
            pass.execute(cmd);
    }
}

