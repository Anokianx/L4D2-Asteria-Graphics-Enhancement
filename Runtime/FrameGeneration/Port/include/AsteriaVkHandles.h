#pragma once
#include <vulkan/vulkan.h>
#include <cstdint>
#include <type_traits>

namespace asteria_vk
{
enum class HandleKind { Image, Buffer, Pipeline, DeviceMemory, NativeSwapchain, ProxySwapchain };
void *Store(VkDevice device, HandleKind kind, uint64_t native);
void *StoreBorrowed(VkDevice device, HandleKind kind, uint64_t native);
bool Resolve(const void *box, VkDevice device, HandleKind kind, uint64_t &native);
bool Retire(const void *box);
void RetireDevice(VkDevice device);
VkDevice DeviceOf(const void *box);
VkSwapchainKHR StoreProxy(VkDevice device, void *object);
void *ProxyBox(VkDevice device, VkSwapchainKHR token);
void *ProxyObject(VkDevice device, VkSwapchainKHR token);
void *ProxyObjectFromBox(const void *box, VkDevice device = VK_NULL_HANDLE);
VkSwapchainKHR SwapchainToken(const void *box);
bool RetireProxy(VkDevice device, VkSwapchainKHR token);
bool IsProxyToken(VkSwapchainKHR token);

template <typename T> uint64_t Bits(T handle)
{
    if constexpr (std::is_pointer_v<T>) return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
    else return static_cast<uint64_t>(handle);
}

template <typename T> T Handle(uint64_t native)
{
    static_assert(sizeof(T) == sizeof(uint64_t), "Non-dispatchable Vulkan handles must retain all 64 bits");
    if constexpr (std::is_pointer_v<T>) return reinterpret_cast<T>(static_cast<uintptr_t>(native));
    else return static_cast<T>(native);
}

template <typename T> T Decode(const void *box, VkDevice device, HandleKind kind)
{
    uint64_t native = 0;
    return Resolve(box, device, kind, native) ? Handle<T>(native) : VK_NULL_HANDLE;
}

inline void *Image(VkDevice device, VkImage image) { return Store(device, HandleKind::Image, Bits(image)); }
inline void *Buffer(VkDevice device, VkBuffer buffer) { return Store(device, HandleKind::Buffer, Bits(buffer)); }
}
