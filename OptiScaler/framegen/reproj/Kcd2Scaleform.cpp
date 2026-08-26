#include "pch.h"
#include "Kcd2Scaleform.h"

#include "Logger.h"
#include <detours/detours.h>

#include <atomic>
#include <cstring>

namespace Kcd2Scaleform
{
namespace
{
using BeginDisplayFn = void(__fastcall*)(void* playback, const void* background, const void* viewport, bool scissor,
                                         const void* scissorViewport, const void* canvas);
using EndDisplayFn = void(__fastcall*)(void* playback);

// The exact type is deliberately opaque: 16-byte ColorF is passed indirectly by the x64 ABI and
// the remaining Scaleform arguments are references. We only bracket the original call.
BeginDisplayFn g_beginOriginal = nullptr;
EndDisplayFn g_endOriginal = nullptr;
std::atomic<int> g_initState { 0 }; // 0 wait, 1 installing, 2 installed, 3 failed closed
std::atomic<uint64_t> g_beginCount { 0 };
std::atomic<uint64_t> g_endCount { 0 };
std::atomic<uint64_t> g_omCount { 0 };
thread_local uint32_t g_displayDepth = 0;
thread_local uint32_t g_loggedScopes = 0;

struct CompleteObjectLocator
{
    uint32_t signature;
    uint32_t offset;
    uint32_t cdOffset;
    uint32_t typeDescriptorRva;
    uint32_t classDescriptorRva;
    uint32_t selfRva;
};

bool IsInModule(uintptr_t module, size_t imageSize, uintptr_t address, size_t size = 1)
{
    return address >= module && size <= imageSize && address - module <= imageSize - size;
}

bool HasPrefix(uintptr_t address, const uint8_t* bytes, size_t size)
{
    __try
    {
        return std::memcmp(reinterpret_cast<const void*>(address), bytes, size) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool FindScaleformVtable(HMODULE moduleHandle, uintptr_t* begin, uintptr_t* end)
{
    const auto module = reinterpret_cast<uintptr_t>(moduleHandle);
    __try
    {
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return false;
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(module + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            return false;
        const size_t imageSize = nt->OptionalHeader.SizeOfImage;
        static constexpr char typeName[] = ".?AVCScaleformPlayback@@";

        uintptr_t typeDescriptor = 0;
        for (size_t offset = 16; offset + sizeof(typeName) <= imageSize; ++offset)
        {
            const auto candidate = module + offset;
            if (std::memcmp(reinterpret_cast<const void*>(candidate), typeName, sizeof(typeName)) == 0)
            {
                typeDescriptor = candidate - 16; // MSVC TypeDescriptor::name follows two pointer-sized fields.
                break;
            }
        }
        if (!typeDescriptor)
            return false;

        const uint32_t typeRva = static_cast<uint32_t>(typeDescriptor - module);
        uintptr_t locatorAddress = 0;
        for (size_t offset = 0; offset + sizeof(CompleteObjectLocator) <= imageSize; offset += alignof(uint32_t))
        {
            const auto* locator = reinterpret_cast<const CompleteObjectLocator*>(module + offset);
            if (locator->signature == 1 && locator->offset == 0 && locator->typeDescriptorRva == typeRva &&
                locator->selfRva == offset)
            {
                locatorAddress = module + offset;
                break;
            }
        }
        if (!locatorAddress)
            return false;

        uintptr_t vtable = 0;
        for (size_t offset = sizeof(uintptr_t); offset + 7 * sizeof(uintptr_t) <= imageSize;
             offset += alignof(uintptr_t))
        {
            const auto* possibleVtable = reinterpret_cast<const uintptr_t*>(module + offset);
            if (possibleVtable[-1] != locatorAddress)
                continue;
            // BeginDisplay/EndDisplay must both point within WHGame's executable image.
            if (IsInModule(module, imageSize, possibleVtable[5], 8) &&
                IsInModule(module, imageSize, possibleVtable[6], 8))
            {
                vtable = reinterpret_cast<uintptr_t>(possibleVtable);
                break;
            }
        }
        if (!vtable)
            return false;

        const auto* slots = reinterpret_cast<const uintptr_t*>(vtable);
        // Retail 1.5.6 signatures: CScaleformPlayback::BeginDisplay and EndDisplay. This keeps
        // unknown KCD2 updates from receiving an ABI-sensitive detour just because RTTI matched.
        static constexpr uint8_t beginPrefix[] = { 0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x44, 0x88, 0x48, 0x20 };
        static constexpr uint8_t endPrefix[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0xD9 };
        if (!HasPrefix(slots[5], beginPrefix, sizeof(beginPrefix)) ||
            !HasPrefix(slots[6], endPrefix, sizeof(endPrefix)))
            return false;
        *begin = slots[5];
        *end = slots[6];
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

void __fastcall BeginDisplayHook(void* playback, const void* background, const void* viewport, bool scissor,
                                 const void* scissorViewport, const void* canvas)
{
    ++g_displayDepth;
    const auto sequence = g_beginCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g_loggedScopes++ < 4)
        LOG_INFO("KCD2 Scaleform trace: begin #{} playback={:X} depth={}", sequence, reinterpret_cast<size_t>(playback),
                 g_displayDepth);
    g_beginOriginal(playback, background, viewport, scissor, scissorViewport, canvas);
}

void __fastcall EndDisplayHook(void* playback)
{
    g_endOriginal(playback);
    const auto sequence = g_endCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (g_displayDepth > 0)
        --g_displayDepth;
    if (g_loggedScopes <= 4)
        LOG_INFO("KCD2 Scaleform trace: end #{} playback={:X} depth={}", sequence, reinterpret_cast<size_t>(playback),
                 g_displayDepth);
}
} // namespace

bool Initialize()
{
    if (g_initState.load(std::memory_order_acquire) == 2)
        return true;
    const auto module = GetModuleHandleW(L"WHGame.dll");
    if (!module)
        return false;
    int expected = 0;
    if (!g_initState.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
        return expected == 2;

    uintptr_t begin = 0, end = 0;
    if (!FindScaleformVtable(module, &begin, &end))
    {
        LOG_WARN("KCD2 Scaleform trace: CScaleformPlayback signature not found; refusing hook");
        g_initState.store(3, std::memory_order_release);
        return false;
    }
    g_beginOriginal = reinterpret_cast<BeginDisplayFn>(begin);
    g_endOriginal = reinterpret_cast<EndDisplayFn>(end);
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());
    DetourAttach(reinterpret_cast<PVOID*>(&g_beginOriginal), BeginDisplayHook);
    DetourAttach(reinterpret_cast<PVOID*>(&g_endOriginal), EndDisplayHook);
    const auto result = DetourTransactionCommit();
    if (result != NO_ERROR)
    {
        LOG_ERROR("KCD2 Scaleform trace: detour failed: {}", result);
        g_beginOriginal = nullptr;
        g_endOriginal = nullptr;
        g_initState.store(3, std::memory_order_release);
        return false;
    }
    g_initState.store(2, std::memory_order_release);
    LOG_INFO("KCD2 Scaleform trace: installed BeginDisplay={:X} EndDisplay={:X}", begin, end);
    return true;
}

bool IsActiveOnThisThread()
{
    if (g_initState.load(std::memory_order_acquire) == 0)
        Initialize();
    return g_displayDepth != 0;
}

void TraceOmSetRenderTargets(ID3D12GraphicsCommandList* commandList, uint32_t targetCount,
                             ID3D12Resource* const* targets)
{
    if (!IsActiveOnThisThread())
        return;
    const auto sequence = g_omCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (sequence <= 12)
    {
        LOG_INFO("KCD2 Scaleform trace: OM #{} cmd={:X} targets={} first={:X}", sequence,
                 reinterpret_cast<size_t>(commandList), targetCount,
                 targetCount > 0 && targets != nullptr ? reinterpret_cast<size_t>(targets[0]) : 0);
    }
}
} // namespace Kcd2Scaleform
