#include "AsteriaVkHandles.h"
#include <iostream>
#include <limits>
#include <thread>
#include <vector>

int main()
{
    using namespace asteria_vk;
    const auto device = reinterpret_cast<VkDevice>(uintptr_t(0x1000));
    const auto other = reinterpret_cast<VkDevice>(uintptr_t(0x2000));
    const uint64_t high = UINT64_C(0x12345678ABCDEF01);
    uint64_t value = 0;
    for (const auto kind : {HandleKind::Image, HandleKind::Buffer, HandleKind::Pipeline, HandleKind::DeviceMemory, HandleKind::NativeSwapchain})
    {
        void *box = Store(device, kind, high);
        if (!box || Store(device, kind, high) != box || !Resolve(box, device, kind, value) || value != high) return 1;
        if (Resolve(box, other, kind, value) || Resolve(reinterpret_cast<void *>((std::numeric_limits<uintptr_t>::max)()), device, kind, value)) return 2;
        if (!Retire(box) || Resolve(box, device, kind, value) || Retire(box)) return 3;
        void *fresh = Store(device, kind, high);
        if (fresh == box || !Resolve(fresh, device, kind, value) || value != high) return 4;
    }
    void *image = Store(device, HandleKind::Image, high);
    if (Resolve(image, device, HandleKind::Buffer, value)) return 5;
    int object = 17;
    const auto proxy = StoreProxy(device, &object);
    if (!proxy || !IsProxyToken(proxy) || ProxyObject(device, proxy) != &object || ProxyObject(other, proxy)) return 6;
    void *box = ProxyBox(device, proxy);
    if (SwapchainToken(box) != proxy || ProxyObjectFromBox(box, device) != &object) return 7;
    if (!RetireProxy(device, proxy) || !IsProxyToken(proxy) || ProxyObject(device, proxy) || SwapchainToken(box)) return 8;
    std::vector<std::thread> workers;
    for (unsigned worker = 0; worker < 4; ++worker)
        workers.emplace_back([=] { for (unsigned i = 0; i < 1000; ++i) Store(device, HandleKind::Image, high + i); });
    for (auto &worker : workers) worker.join();
    RetireDevice(device);
    if (Resolve(image, device, HandleKind::Image, value)) return 9;
    std::cout << "{\"process_bits\":" << sizeof(void *) * 8 << ",\"high_bit_round_trip\":true,\"stale_device_type_rejection\":true,\"threaded_interning\":true}" << std::endl;
    return 0;
}
