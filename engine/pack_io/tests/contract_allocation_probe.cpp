// T011: isolated C++ allocation accounting for the fixed embedded catalog and
// its maximum two simultaneous compiled-validator owners. The pinned Windows
// profile statically links the validator, so its C++ allocations use these hooks.
#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <spectrapack/io/contracts.hpp>

namespace {
std::atomic_bool counting {};
std::atomic<std::size_t> live {}, peak {};
struct alignas(std::max_align_t) Header {
    void* original;
    std::size_t bytes;
    bool counted;
};
void* allocate(std::size_t size, std::size_t alignment)
{
    size = std::max<std::size_t>(size, 1);
    alignment = std::max(alignment, alignof(Header));
    if (size > SIZE_MAX - sizeof(Header) || alignment - 1 > SIZE_MAX - size - sizeof(Header)) {
        throw std::bad_alloc {};
    }
    const auto capacity = size + sizeof(Header) + alignment - 1;
    auto* raw = std::malloc(capacity);
    if (!raw) {
        throw std::bad_alloc {};
    }
    void* aligned = static_cast<std::byte*>(raw) + sizeof(Header);
    auto space = capacity - sizeof(Header);
    if (!std::align(alignment, size, aligned, space)) {
        std::free(raw);
        throw std::bad_alloc {};
    }
    auto* header = reinterpret_cast<Header*>(aligned) - 1;
    const auto tracked = counting.load(std::memory_order_relaxed);
    *header = { raw, size, tracked };
    if (tracked) {
        const auto current = live.fetch_add(size, std::memory_order_relaxed) + size;
        auto previous = peak.load(std::memory_order_relaxed);
        while (previous < current && !peak.compare_exchange_weak(previous, current, std::memory_order_relaxed)) {
        }
    }
    return aligned;
}
void release(void* pointer) noexcept
{
    if (!pointer) {
        return;
    }
    auto* header = reinterpret_cast<Header*>(pointer) - 1;
    if (header->counted) {
        live.fetch_sub(header->bytes, std::memory_order_relaxed);
    }
    std::free(header->original);
}
}  // namespace
void* operator new(std::size_t size) { return allocate(size, alignof(std::max_align_t)); }
void* operator new[](std::size_t size) { return allocate(size, alignof(std::max_align_t)); }
void* operator new(std::size_t size, std::align_val_t alignment)
{
    return allocate(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return allocate(size, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    try {
        return ::operator new(size);
    }
    catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    try {
        return ::operator new[](size);
    }
    catch (...) {
        return nullptr;
    }
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    try {
        return ::operator new(size, alignment);
    }
    catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{
    try {
        return ::operator new[](size, alignment);
    }
    catch (...) {
        return nullptr;
    }
}
void operator delete(void* pointer) noexcept { release(pointer); }
void operator delete[](void* pointer) noexcept { release(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { release(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { release(pointer); }
void operator delete(void* pointer, std::align_val_t) noexcept { release(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { release(pointer); }
void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept { release(pointer); }
void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept { release(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { release(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { release(pointer); }
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { release(pointer); }
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { release(pointer); }

int main()
{
    std::array<std::unique_ptr<spectrapack::io::ContractValidator>, 2> validators;
    counting = true;
    try {
        for (auto& validator : validators) {
            validator = std::make_unique<spectrapack::io::ContractValidator>();
        }
    }
    catch (...) {
        counting = false;
        return 2;
    }
    counting = false;
    const auto retained = live.load(), construction_peak = peak.load();
    for (auto& validator : validators) {
        validator.reset();
    }
    const auto persistent = live.load();
    // No counter operation allocates. Probe headers, malloc bookkeeping/slack,
    // code/read-only schema text, stacks and process RSS are excluded.
    std::cout << "{\"validator_count\":2,\"retained_cpp_bytes\":" << retained
              << ",\"peak_cpp_bytes\":" << construction_peak << ",\"after_destroy_cpp_bytes\":" << persistent << "}\n";
    return construction_peak <= 7ULL * 1024 * 1024 ? 0 : 1;
}
