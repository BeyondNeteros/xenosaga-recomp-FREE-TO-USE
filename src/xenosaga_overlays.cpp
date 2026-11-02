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
#include <utility>
#include <vector>

void xs_register_ov01(PS2Runtime::RecompiledFunction *t);
void xs_register_ov02(PS2Runtime::RecompiledFunction *t);
void xs_register_ov10(PS2Runtime::RecompiledFunction *t);
void xs_register_ov11(PS2Runtime::RecompiledFunction *t);
void xs_register_ov12(PS2Runtime::RecompiledFunction *t);
void xs_configureDiscImage();
void ps2x_armGsCapture(); // gs_frontend.cpp
bool ps2x_hostWidescreen(); // ps2_runtime.cpp (settings.ini aspect = 16:9)
bool xs_readDiscFile(const std::string &path, std::vector<uint8_t> &out);
void xs_sceCdSearchFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
void xs_sceCdLayerSearchFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
void xs_sceCdReadDvdDualInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
void xs_startHeartbeat(PS2Runtime &runtime);
void xs_traceDisplay(PS2Runtime &runtime);

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

    // XS_BATTLE_TRACE=1: log calls to OV01's battle functions (and arm the
    // GS capture, PS2X_GS_CAPTURE, when a battle starts).
    struct TracedFunction
    {
        uint32_t addr;
        const char *name;
        bool logReturn;
        PS2Runtime::RecompiledFunction original;
        uint32_t lastReturn;
    };
    TracedFunction g_battleTrace[] = {
        {0x00A07770u, "battleInit", false, nullptr, ~0u},
        {0x00A078B8u, "battleMain", false, nullptr, ~0u},
        {0x00A078E8u, "battleAfter", false, nullptr, ~0u},
        {0x00A0B518u, "battleEndChk", true, nullptr, ~0u},
        {0x00A0B600u, "battleRetCodeSet", false, nullptr, ~0u},
        {0x00A22668u, "battleProc", false, nullptr, ~0u},
        {0x00A22850u, "battleProcEnd", false, nullptr, ~0u},
        {0x00A236C0u, "encountEffDisp", false, nullptr, ~0u},
        {0x00A24950u, "encEffVramCopy", false, nullptr, ~0u},
        {0x00A24B90u, "grVramCopyFBtoFB", false, nullptr, ~0u},
        {0x00A17148u, "calcStatGet", true, nullptr, ~0u},
        {0x00A01000u, "unitLiveNumGet2", true, nullptr, ~0u},
        {0x00A00D98u, "unitTblGet", true, nullptr, ~0u},
        {0x00A1A6F8u, "dataFileLoad", true, nullptr, ~0u},
        {0x00A1D0F8u, "dataThinkLoad", false, nullptr, ~0u},
        {0x00A1D5D0u, "thinkInit", false, nullptr, ~0u},
        {0x00A1CEA0u, "dataBatDatLoad", true, nullptr, ~0u},
        {0x00A1CFB8u, "dataThinkDataSet", true, nullptr, ~0u},
        {0x00A1D0A8u, "dataThinkFileLoad", true, nullptr, ~0u},
        {0x00A1CED8u, "thinkMapGet", true, nullptr, ~0u},
        {0x00A24C68u, "grVramCopy", false, nullptr, ~0u},
        {0x00A25A80u, "grPacketSend", false, nullptr, ~0u},
        {0x00A25EA0u, "grGsRegSet", false, nullptr, ~0u},
        {0x0021DEB8u, "xglCdReadFilePart", true, nullptr, ~0u},
        {0x0021CF98u, "queue_next", true, nullptr, ~0u},
        {0x0020DC98u, "sceCdRead", true, nullptr, ~0u},
        {0x0020D448u, "sceCdSync", true, nullptr, ~0u},
        {0x0021CF18u, "extract", false, nullptr, ~0u},
        {0x002185A8u, "xglArxExtract", false, nullptr, ~0u},
        {0x00A1D348u, "thinkSysInit", false, nullptr, ~0u},
        {0x00A1D380u, "thinkTopSet", false, nullptr, ~0u},
        {0x00A2D8B8u, "sndSeTrans", true, nullptr, ~0u},
        {0x00A2DCA0u, "sndBankGet", true, nullptr, ~0u},
        {0x00A2D388u, "sndSeRegChk", true, nullptr, ~0u},
        {0x00A2A118u, "menuNumPut", false, nullptr, ~0u},
        {0x00A29CD0u, "menuPlStatPut", false, nullptr, ~0u},
        {0x00A2BBE8u, "menuStatSpriteRGB", false, nullptr, ~0u},
    };

    template <size_t I>
    void tracedCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        TracedFunction &f = g_battleTrace[I];
        static uint32_t calls = 0;
        const bool log = !f.logReturn && ++calls <= ((f.addr == 0x00A2A118u || f.addr == 0x00A29CD0u || f.addr == 0x00A2BBE8u) ? 20000u : 400u);
        if (f.logReturn) ++calls;
        if (log)
            std::printf("[battle] %s a0=%08x a1=%08x a2=%08x a3=%08x t0=%08x t1=%08x t2=%08x ra=%08x\n", f.name, GPR_U32(ctx, 4),
                        GPR_U32(ctx, 5), GPR_U32(ctx, 6), GPR_U32(ctx, 7), GPR_U32(ctx, 8), GPR_U32(ctx, 9), GPR_U32(ctx, 10),
                        GPR_U32(ctx, 31));
        static const uint32_t armAt = [] {
            const char *env = std::getenv("XS_GS_ARM_AT"); // battleMain call that arms the GS capture
            return env ? static_cast<uint32_t>(std::atoi(env)) : 0u;
        }();
        if (I == 0 && armAt == 0u)
            ps2x_armGsCapture();
        if (I == 1 && armAt != 0u && calls == armAt)
            ps2x_armGsCapture();
        static const uint32_t dumpAt = [] {
            const char *env = std::getenv("XS_BATTLE_DUMP_AT");
            return env ? static_cast<uint32_t>(std::atoi(env)) : 30u;
        }();
        if (I == 1 && calls == dumpAt) // battleMain call N (XS_BATTLE_DUMP_AT, default 30): dump EE RAM
        {
            const std::filesystem::path out = PS2Runtime::getIoPaths().elfDirectory / "xs_ram_battle.bin";
            std::ofstream file(out, std::ios::binary);
            file.write(reinterpret_cast<const char *>(rdram), 32 * 1024 * 1024);
            std::printf("[battle] RAM dumped to %s\n", out.string().c_str());
        }
        const uint32_t a0 = GPR_U32(ctx, 4), a1 = GPR_U32(ctx, 5), a2 = GPR_U32(ctx, 6);
        if (f.addr == 0x0021DEB8u) // xglCdReadFilePart(name, dest, ...)
            std::printf("[battle] xglCdReadFilePart \"%.80s\" -> %08x busy=%d\n",
                        reinterpret_cast<const char *>(rdram + (a0 & kRdramMask)), a1,
                        static_cast<int>(static_cast<int8_t>(rdram[(0x0093CC00u + 0x30u) & kRdramMask])));
        if (f.addr == 0x00A1A6F8u) // dataFileLoad(name, dest): log the file name
        {
            const char *name = reinterpret_cast<const char *>(rdram + (a0 & kRdramMask));
            std::printf("[battle] dataFileLoad \"%.80s\" -> %08x\n", name, a1);
        }
        f.original(rdram, ctx, runtime);
        {
            // Think data area (0x436F40): show its first bytes after each traced call.
            static uint64_t lastThink = 0;
            uint64_t think;
            std::memcpy(&think, rdram + 0x436F40u, 8);
            // Also watch the think pointers (top 0xA59BE0, base 0xA59BE8).
            uint32_t top, base;
            std::memcpy(&top, rdram + 0xA59BE0u, 4);
            std::memcpy(&base, rdram + 0xA59BE8u, 4);
            static uint32_t lastTop = 1, lastBase = 1;
            if (top != lastTop || base != lastBase)
                std::printf("[battle] think top=%08x base=%08x (after %s)\n", top, base, f.name);
            lastTop = top;
            lastBase = base;
            if (think != lastThink)
                std::printf("[battle] think area now %016llx (after %s)\n", static_cast<unsigned long long>(think), f.name);
            lastThink = think;
        }
        if (f.logReturn)
        {
            const uint32_t v0 = GPR_U32(ctx, 2);
            static uint32_t retLogged = 0;
            if (v0 != f.lastReturn || ++retLogged <= 300u)
                std::printf("[battle] %s(%08x, %08x, %08x) -> %08x\n", f.name, a0, a1, a2, v0);
            f.lastReturn = v0;
        }
    }

    template <size_t... I>
    void installBattleTrace(std::index_sequence<I...>)
    {
        static const PS2Runtime::RecompiledFunction wrappers[] = {tracedCall<I>...};
        for (size_t i = 0; i < sizeof...(I); ++i)
        {
            const uint32_t slot = (g_battleTrace[i].addr - g_ps2RecompiledFunctionTableBase) >> 2;
            if (slot >= g_ps2RecompiledFunctionTableSlotCount || !g_ps2RecompiledFunctionTable[slot])
                continue;
            g_battleTrace[i].original = g_ps2RecompiledFunctionTable[slot];
            g_ps2RecompiledFunctionTable[slot] = wrappers[i];
        }
    }

    // Widescreen: xglCameraViewScreen (0x22B9B8) fills the camera's
    // projection; +0x28 (camera +0x10 block, field 0x18) is the horizontal
    // scale, rebuilt from scratch on every call. Narrowing it by 3/4 makes the
    // 4:3 frame hold a 16:9 field of view (the presenter stretches it back),
    // and the culling frustum built from it in xglCameraViewTravel widens too.
    constexpr uint32_t kCameraViewScreenAddr = 0x0022B9B8u;
    PS2Runtime::RecompiledFunction g_cameraViewScreenOriginal = nullptr;

    void cameraViewScreen_widescreen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t camera = GPR_U32(ctx, 4);
        g_cameraViewScreenOriginal(rdram, ctx, runtime);
        if (!ps2x_hostWidescreen())
            return;
        static const float scale = [] {
            const char *env = std::getenv("XS_WIDESCREEN_SCALE"); // for experiments
            return env ? static_cast<float>(std::atof(env)) : 0.75f;
        }();
        float horizontal;
        std::memcpy(&horizontal, rdram + ((camera + 0x28u) & kRdramMask), 4);
        horizontal *= scale;
        std::memcpy(rdram + ((camera + 0x28u) & kRdramMask), &horizontal, 4);
    }

    void installWidescreenHook()
    {
        const uint32_t slot = (kCameraViewScreenAddr - g_ps2RecompiledFunctionTableBase) >> 2;
        if (g_cameraViewScreenOriginal || slot >= g_ps2RecompiledFunctionTableSlotCount ||
            !g_ps2RecompiledFunctionTable[slot])
            return;
        g_cameraViewScreenOriginal = g_ps2RecompiledFunctionTable[slot];
        g_ps2RecompiledFunctionTable[slot] = cameraViewScreen_widescreen;
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
        installWidescreenHook();
        if (id == 1)
            if (const char *trace = std::getenv("XS_BATTLE_TRACE"); trace && trace[0] == '1')
                installBattleTrace(std::make_index_sequence<sizeof(g_battleTrace) / sizeof(g_battleTrace[0])>{});
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
        xs_traceDisplay(runtime);
    }
}

PS2_REGISTER_GAME_OVERRIDE("Xenosaga Episode I", "SLUS_204.69", 0x00200008u, 0u, applyXenosaga)
