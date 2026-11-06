#include "AsteriaVkHandles.h"
#include <limits>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace asteria_vk
{
struct Key
{
    VkDevice device;
    HandleKind kind;
    uint64_t native;
    bool operator==(const Key &other) const { return device == other.device && kind == other.kind && native == other.native; }
};
struct KeyHash
{
    size_t operator()(const Key &key) const
    {
        return std::hash<uint64_t>{}(Bits(key.device)) ^ (std::hash<uint64_t>{}(key.native) << 1) ^ (size_t(key.kind) << 2);
    }
};
struct Record { Key key; uint64_t token = 0; bool alive = true; };
static std::mutex Mutex;
static std::unordered_map<Key, void *, KeyHash> Interned;
static std::unordered_map<const void *, std::unique_ptr<Record>> Records;
static std::unordered_map<uint64_t, void *> Proxies;
static std::unordered_set<uint64_t> KnownProxies;
static uintptr_t BoxSequence = 0;

static void *NextBoxLocked()
{
    return BoxSequence == (std::numeric_limits<uintptr_t>::max)() ? nullptr : reinterpret_cast<void *>(++BoxSequence);
}

static void *StoreLocked(VkDevice device, HandleKind kind, uint64_t native)
{
    if (!device || !native) return nullptr;
    const Key key{device, kind, native};
    const auto current = Interned.find(key);
    if (current != Interned.end()) return current->second;
    auto record = std::make_unique<Record>();
    record->key = key;
    void *box = NextBoxLocked();
    if (!box) return nullptr;
    Records.emplace(box, std::move(record));
    try { Interned.emplace(key, box); }
    catch (...) { Records.erase(box); throw; }
    return box;
}

void *Store(VkDevice device, HandleKind kind, uint64_t native)
{
    const std::lock_guard<std::mutex> lock(Mutex);
    return StoreLocked(device, kind, native);
}

void *StoreBorrowed(VkDevice device, HandleKind kind, uint64_t native)
{
    if (!device || !native) return nullptr;
    const std::lock_guard<std::mutex> lock(Mutex);
    auto record = std::make_unique<Record>();
    record->key = {device, kind, native};
    void *box = NextBoxLocked();
    if (!box) return nullptr;
    Records.emplace(box, std::move(record));
    return box;
}

static Record *FindLocked(const void *box, VkDevice device, HandleKind kind)
{
    const auto entry = Records.find(box);
    if (entry == Records.end() || !entry->second->alive) return nullptr;
    auto *record = entry->second.get();
    return record->key.kind == kind && (!device || record->key.device == device) ? record : nullptr;
}

bool Resolve(const void *box, VkDevice device, HandleKind kind, uint64_t &native)
{
    const std::lock_guard<std::mutex> lock(Mutex);
    const auto *record = FindLocked(box, device, kind);
    native = record ? record->key.native : 0;
    return record != nullptr;
}

static bool RetireLocked(const void *box)
{
    const auto entry = Records.find(box);
    if (entry == Records.end() || !entry->second->alive) return false;
    auto &record = *entry->second;
    record.alive = false;
    const auto interned = Interned.find(record.key);
    if (interned != Interned.end() && interned->second == box) Interned.erase(interned);
    if (record.token) Proxies.erase(record.token);
    Records.erase(entry);
    return true;
}

bool Retire(const void *box)
{
    const std::lock_guard<std::mutex> lock(Mutex);
    return RetireLocked(box);
}

void RetireDevice(VkDevice device)
{
    const std::lock_guard<std::mutex> lock(Mutex);
    for (auto entry = Records.begin(); entry != Records.end();)
    {
        const auto current = entry++;
        if (current->second->key.device == device) RetireLocked(current->first);
    }
}

VkDevice DeviceOf(const void *box)
{
    const std::lock_guard<std::mutex> lock(Mutex);
    const auto entry = Records.find(box);
    return entry != Records.end() && entry->second->alive ? entry->second->key.device : VK_NULL_HANDLE;
}

VkSwapchainKHR StoreProxy(VkDevice device, void *object)
{
    const std::lock_guard<std::mutex> lock(Mutex);
    void *box = StoreLocked(device, HandleKind::ProxySwapchain, Bits(object));
    if (!box) return VK_NULL_HANDLE;
    auto &record = *Records.at(box);
    if (!record.token)
    {
        for (;;)
        {
            void *tokenObject = std::malloc(sizeof(uintptr_t));
            if (!tokenObject) { RetireLocked(box); throw std::bad_alloc(); }
            const uint64_t token = Bits(tokenObject);
            bool collision = !token || Proxies.find(token) != Proxies.end() || KnownProxies.find(token) != KnownProxies.end();
            for (const auto &entry : Interned)
                collision = collision || (entry.first.kind == HandleKind::NativeSwapchain && entry.first.native == token);
            if (collision) { std::free(tokenObject); continue; }
            try
            {
                Proxies.emplace(token, box);
                KnownProxies.insert(token);
            }
            catch (...)
            {
                std::free(tokenObject);
                Proxies.erase(token);
                RetireLocked(box);
                throw;
            }
            record.token = token;
            break;
        }
    }
    return Handle<VkSwapchainKHR>(record.token);
}

void *ProxyBox(VkDevice device, VkSwapchainKHR token)
{
    const std::lock_guard<std::mutex> lock(Mutex);
    const auto entry = Proxies.find(Bits(token));
    return entry != Proxies.end() && FindLocked(entry->second, device, HandleKind::ProxySwapchain) ? entry->second : nullptr;
}

void *ProxyObject(VkDevice device, VkSwapchainKHR token)
{
    const std::lock_guard<std::mutex> lock(Mutex);
    const auto entry = Proxies.find(Bits(token));
    const auto *record = entry == Proxies.end() ? nullptr : FindLocked(entry->second, device, HandleKind::ProxySwapchain);
    return record && record->key.native <= (std::numeric_limits<uintptr_t>::max)()
        ? reinterpret_cast<void *>(static_cast<uintptr_t>(record->key.native)) : nullptr;
}

void *ProxyObjectFromBox(const void *box, VkDevice device)
{
    uint64_t pointer = 0;
    if (!Resolve(box, device, HandleKind::ProxySwapchain, pointer) || pointer > (std::numeric_limits<uintptr_t>::max)()) return nullptr;
    return reinterpret_cast<void *>(static_cast<uintptr_t>(pointer));
}

VkSwapchainKHR SwapchainToken(const void *box)
{
    const std::lock_guard<std::mutex> lock(Mutex);
    const auto entry = Records.find(box);
    if (entry == Records.end() || !entry->second->alive) return VK_NULL_HANDLE;
    const auto &record = *entry->second;
    if (record.key.kind == HandleKind::NativeSwapchain) return Handle<VkSwapchainKHR>(record.key.native);
    return record.key.kind == HandleKind::ProxySwapchain ? Handle<VkSwapchainKHR>(record.token) : VK_NULL_HANDLE;
}

bool RetireProxy(VkDevice device, VkSwapchainKHR token)
{
    const std::lock_guard<std::mutex> lock(Mutex);
    const auto entry = Proxies.find(Bits(token));
    return entry != Proxies.end() && FindLocked(entry->second, device, HandleKind::ProxySwapchain) && RetireLocked(entry->second);
}

bool IsProxyToken(VkSwapchainKHR token)
{
    const std::lock_guard<std::mutex> lock(Mutex);
    return KnownProxies.find(Bits(token)) != KnownProxies.end();
}
}
