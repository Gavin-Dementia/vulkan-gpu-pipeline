#pragma once
#include <vulkan/vulkan.h>
#include <vector>

class VulkanImageView {
public:
    void create(
            VkDevice device,
            const std::vector<VkImage>& images,
            VkFormat format);

    void destroy(VkDevice device);

    const std::vector<VkImageView>& get() const { return imageViews; }

private:
    std::vector<VkImageView> imageViews;

    VkImageView createImageView(VkDevice device,
                                VkImage image,
                                VkFormat format);
};

