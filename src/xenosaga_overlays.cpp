// Xenosaga Episode I (SLUS_204.69): overlay support.
//
// The game swaps code overlays (OV01/02/10/11/12.OVL) into the same memory
// region at 0xA00000 via xglCdLoadOverlay(int id). Each overlay is recompiled
// separately with an xs_ovNN_ prefix; this hook replaces the original loader,
// copies the overlay's data into guest memory, and points the function table's
// overlay region at the matching recompiled functions.

#include "ps2_runtime.h"
#include "game_overrides.h"
#include "ps2_stubs.h"

#include <array>
#include <cstdio>
#include <cstdlib>
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
void xs_configureDiscImage();
bool xs_readDiscFile(const std::string &path, std::vector<uint8_t> &out);
void xs_sceCdSearchFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
void xs_sceCdLayerSearchFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
void xs_sceCdReadDvdDualInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
void xs_startHeartbeat(PS2Runtime &runtime);

namespace
{
    uint32_t g_currentOverlay = 2; // the boot ELF carries ov02
}
uint32_t xs_currentOverlay() { return g_currentOverlay; }

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

    // Overlay bytes come from a loose OVnn.OVL next to the ELF, or from the disc image.
    bool readOverlayFile(uint32_t id, std::vector<uint8_t> &data, std::string &source)
    {
        if (const std::filesystem::path file = findOverlayFile(id); !file.empty())
        {
            std::ifstream in(file, std::ios::binary);
            data.assign(std::istreambuf_iterator<char>(in), {});
            source = file.filename().string();
            return !data.empty();
        }
        char name[24];
        std::snprintf(name, sizeof(name), "\\OV%02u.OVL;1", id);
        source = std::string("disc ") + name;
        return xs_readDiscFile(name, data);
    }

    // Copy the overlay's PT_LOAD segments into guest memory, like sceSifLoadElf.
    bool loadOverlayImage(uint8_t *rdram, const std::vector<uint8_t> &data)
    {
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

        std::vector<uint8_t> data;
        std::string source;
        if (!readOverlayFile(id, data, source) || !loadOverlayImage(rdram, data))
            std::cerr << "[xenosaga] failed to load overlay " << id << " (" << source << ")" << std::endl;
        activateOverlayFunctions(id);
        g_currentOverlay = id;
        std::cout << "[xenosaga] overlay " << id << " active (" << source << ")" << std::endl;
    }

    // Temporary: skip Monolith's sound driver (SSD/RSSD on the IOP) until the
    // real IRX modules are available. Enabled with XS_NO_SOUND=1.
    void soundStub_returnZero(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, 0);
    }

    // CD S-commands the runtime doesn't simulate yet. The originals busy-wait on
    // a real drive, so answer them directly. All return 1 (success).
    void cdReturnSuccess(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, 1);
    }

    void cdReturnZero(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, 0);
    }

    void devctlNoDevice(uint8_t *, R5900Context *ctx, PS2Runtime *)
    {
        setReturnS32(ctx, -19); // -ENODEV
    }


    void applyXenosaga(PS2Runtime &runtime)
    {
        // Write logs straight through, so a hang or crash still leaves a full log.
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        std::ios::sync_with_stdio(true);
        xs_configureDiscImage();
        runtime.registerFunction(kLoadOverlayAddr, xglCdLoadOverlay_hook);
        // File lookups return real disc positions when a disc image is present.
        runtime.registerFunction(0x0020D220u, xs_sceCdSearchFile);      // sceCdSearchFile
        runtime.registerFunction(0x0020CF08u, xs_sceCdLayerSearchFile); // sceCdLayerSearchFile
        runtime.registerFunction(0x0020E1C8u, cdReturnSuccess);   // _sceCdSetTimeout
        runtime.registerFunction(0x0020E358u, cdReturnSuccess);   // sceCdPowerOff
        runtime.registerFunction(0x0020E2A0u, xs_sceCdReadDvdDualInfo); // sceCdReadDvdDualInfo
        // _sceFsWaitS waits on the libc file-I/O semaphore. File I/O runs host-side,
        // so that semaphore is never created; succeed immediately.
        runtime.registerFunction(0x00204338u, cdReturnZero);
        // sceDevctl("hdd0:", ...): report that no PS2 hard drive is attached, so
        // the game uses the disc instead of a HDD install.
        runtime.registerFunction(0x00206E88u, devctlNoDevice);
        if (const char *v = std::getenv("XS_NO_SOUND"); v && v[0] == '1')
        {
            runtime.registerFunction(0x0023F960u, soundStub_returnZero); // SsdInit
            runtime.registerFunction(0x0023FBB8u, soundStub_returnZero); // RssdInitIop
            runtime.registerFunction(0x0023FE68u, soundStub_returnZero); // RssdBusy
            runtime.registerFunction(0x002405E8u, soundStub_returnZero); // SsdSendFuncPacket
            // Every SSD request to the IOP goes through these; answer them locally.
            runtime.registerFunction(0x0023FFF0u, soundStub_returnZero); // RssdCallFunc
            runtime.registerFunction(0x002412B0u, soundStub_returnZero); // RssdRequestCall
            runtime.registerFunction(0x0023FEB8u, soundStub_returnZero); // RssdSpuRead
            runtime.registerFunction(0x00240288u, soundStub_returnZero); // RssdGetCallCompletedCode
            // Sound-data DMA status: 0 = transfer finished. Loaders spin until then.
            runtime.registerFunction(0x002409B0u, soundStub_returnZero); // SsdSpuDmaCompleted
            std::cout << "[xenosaga] sound driver stubbed (XS_NO_SOUND=1)" << std::endl;
        }
        // The boot ELF already contains ov02 at 0xA00000.
        activateOverlayFunctions(2);
        std::cout << "[xenosaga] overlay hook installed" << std::endl;
        xs_startHeartbeat(runtime);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Xenosaga Episode I", "SLUS_204.69", 0x00200008u, 0u, applyXenosaga)
