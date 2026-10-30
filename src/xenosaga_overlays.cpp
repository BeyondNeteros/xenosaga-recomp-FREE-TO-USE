// Xenosaga Episode I (SLUS_204.69): overlay support.
//
// The game swaps code overlays (OV01/02/10/11/12.OVL) into the same memory
// region at 0xA00000 via xglCdLoadOverlay(int id). Each overlay is recompiled
// separately with an xs_ovNN_ prefix; this hook replaces the original loader,
// copies the overlay's data into guest memory, and points the function table's
// overlay region at the matching recompiled functions.

#include "ps2_runtime.h"
#include "game_overrides.h"

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

void xs_register_ov01(PS2Runtime::RecompiledFunction *t);
void xs_register_ov02(PS2Runtime::RecompiledFunction *t);
void xs_register_ov10(PS2Runtime::RecompiledFunction *t);
void xs_register_ov11(PS2Runtime::RecompiledFunction *t);
void xs_register_ov12(PS2Runtime::RecompiledFunction *t);

namespace
{
    constexpr uint32_t kLoadOverlayAddr = 0x0021F5A0u;   // xglCdLoadOverlay
    constexpr uint32_t kCurrentOverlayVar = 0x004DC6DCu; // byte at $gp-0x3494
    constexpr uint32_t kOverlayRegionStart = 0x009FF000u;
    constexpr uint32_t kRdramMask = 0x01FFFFFFu;

    using RegisterFn = void (*)(PS2Runtime::RecompiledFunction *);

    RegisterFn registrarFor(uint32_t id)
    {
        switch (id)
        {
        case 1: return xs_register_ov01;
        case 2: return xs_register_ov02;
        case 10: return xs_register_ov10;
        case 11: return xs_register_ov11;
        case 12: return xs_register_ov12;
        default: return nullptr;
        }
    }

    std::filesystem::path findOverlayFile(uint32_t id)
    {
        char name[16];
        std::snprintf(name, sizeof(name), "ov%02u.ovl", id);
        const std::filesystem::path root = PS2Runtime::getIoPaths().cdRoot;
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator(root, ec))
        {
            std::string leaf = entry.path().filename().string();
            for (auto &c : leaf)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (leaf == name || leaf == std::string(name) + ";1")
                return entry.path();
        }
        return {};
    }

    template <typename T>
    T readLE(const std::vector<uint8_t> &d, size_t off)
    {
        T v{};
        std::memcpy(&v, d.data() + off, sizeof(T));
        return v;
    }

    // Copy the overlay's PT_LOAD segments into guest memory, like sceSifLoadElf.
    bool loadOverlayImage(uint8_t *rdram, const std::filesystem::path &path)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
            return false;
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), {});
        if (data.size() < 0x34 || std::memcmp(data.data(), "\x7f" "ELF", 4) != 0)
            return false;

        const uint32_t phoff = readLE<uint32_t>(data, 0x1C);
        const uint16_t phentsize = readLE<uint16_t>(data, 0x2A);
        const uint16_t phnum = readLE<uint16_t>(data, 0x2C);
        for (uint16_t i = 0; i < phnum; ++i)
        {
            const size_t ph = phoff + size_t(i) * phentsize;
            if (ph + 0x20 > data.size() || readLE<uint32_t>(data, ph) != 1u /* PT_LOAD */)
                continue;
            const uint32_t offset = readLE<uint32_t>(data, ph + 0x04);
            const uint32_t vaddr = readLE<uint32_t>(data, ph + 0x08);
            const uint32_t filesz = readLE<uint32_t>(data, ph + 0x10);
            const uint32_t memsz = readLE<uint32_t>(data, ph + 0x14);
            if (offset + filesz > data.size())
                return false;
            uint8_t *dst = rdram + (vaddr & kRdramMask);
            std::memcpy(dst, data.data() + offset, filesz);
            if (memsz > filesz)
                std::memset(dst + filesz, 0, memsz - filesz);
        }
        return true;
    }

    void activateOverlayFunctions(uint32_t id)
    {
        const uint32_t first = (kOverlayRegionStart - g_ps2RecompiledFunctionTableBase) >> 2;
        for (uint32_t slot = first; slot < g_ps2RecompiledFunctionTableSlotCount; ++slot)
            g_ps2RecompiledFunctionTable[slot] = nullptr;
        if (RegisterFn reg = registrarFor(id))
            reg(g_ps2RecompiledFunctionTable);
    }

    void xglCdLoadOverlay_hook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
    {
        const uint32_t id = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4])) & 0xFFu;
        uint8_t &current = rdram[kCurrentOverlayVar & kRdramMask];
        if (current == id)
            return;
        current = static_cast<uint8_t>(id);

        const std::filesystem::path file = findOverlayFile(id);
        if (file.empty() || !loadOverlayImage(rdram, file))
        {
            std::cerr << "[xenosaga] failed to load overlay " << id << " from "
                      << PS2Runtime::getIoPaths().cdRoot << std::endl;
        }
        activateOverlayFunctions(id);
        std::cout << "[xenosaga] overlay " << id << " active (" << file.filename().string() << ")" << std::endl;
    }

    void applyXenosaga(PS2Runtime &runtime)
    {
        runtime.registerFunction(kLoadOverlayAddr, xglCdLoadOverlay_hook);
        // The boot ELF already contains ov02 at 0xA00000.
        activateOverlayFunctions(2);
        std::cout << "[xenosaga] overlay hook installed" << std::endl;
    }
}

PS2_REGISTER_GAME_OVERRIDE("Xenosaga Episode I", "SLUS_204.69", 0x00200008u, 0u, applyXenosaga)
