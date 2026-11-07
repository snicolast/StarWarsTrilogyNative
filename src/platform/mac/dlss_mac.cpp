// macOS: no NVIDIA GPU, so no DLAA (dlss.h). The renderer falls back to its
// normal path as it does on any other GPU without it.
#include "dlss.h"

namespace dlss {

std::vector<std::string> instance_extensions() { return {}; }
std::vector<std::string> device_extensions(VkInstance, VkPhysicalDevice) { return {}; }

bool Dlaa::init(VkInstance, VkPhysicalDevice, VkDevice) { return false; }
void Dlaa::shutdown() {}
bool Dlaa::evaluate(VkCommandBuffer, const Image&, const Image&, const Image&, const Image&, uint32_t, uint32_t, float,
                    float, bool, int) {
    return false;
}

}  // namespace dlss
