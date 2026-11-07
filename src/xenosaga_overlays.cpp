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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
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
bool ps2x_hostSettingBool(const char *key, bool fallback); // ps2_runtime.cpp (settings.ini)
bool ps2x_debugOneHitKill();   // ps2_runtime.cpp (debug toggles, menu)
bool ps2x_debugNoEncounters();
void ps2x_setHostBadge(const char *text);                  // ps2_runtime.cpp (top-left label)
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

    // Widescreen: xglCameraViewScreen (0x22B9B8, src/main/xgl_camera.c in the
    // decomp) fills camera->screen (camera +0x10). Two fields set the
    // horizontal view: clipRange.z (+0x28) feeds the projection matrix built in
    // xglCameraViewVolume, projection.x (+0x50) the culling volume. Widening
    // both by 4/3 gives a true 16:9 view (more to the sides, same height) that
    // the presenter shows 16:9. (The first version narrowed +0x28 by 3/4,
    // which zoomed in: runs/wide16.)
    // clipRange.z is computed from the previous call's projection.x, so the
    // one we widened is put back first or the widening would compound.
    constexpr uint32_t kCameraViewScreenAddr = 0x0022B9B8u;
    PS2Runtime::RecompiledFunction g_cameraViewScreenOriginal = nullptr;
    std::unordered_map<uint32_t, std::pair<float, float>> g_widenedProjection; // camera -> (ours, original)

    void cameraViewScreen_widescreen(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t camera = GPR_U32(ctx, 4);
        uint8_t *clipZ = rdram + ((camera + 0x28u) & kRdramMask);
        uint8_t *projX = rdram + ((camera + 0x50u) & kRdramMask);
        if (const auto it = g_widenedProjection.find(camera); it != g_widenedProjection.end())
        {
            float current;
            std::memcpy(&current, projX, 4);
            if (current == it->second.first)
                std::memcpy(projX, &it->second.second, 4);
            g_widenedProjection.erase(it);
        }
        g_cameraViewScreenOriginal(rdram, ctx, runtime);
        if (!ps2x_hostWidescreen())
            return;
        static const float clipScale = [] {
            const char *env = std::getenv("XS_WS_CLIP"); // for experiments
            return env ? static_cast<float>(std::atof(env)) : 4.0f / 3.0f;
        }();
        static const float projScale = [] {
            const char *env = std::getenv("XS_WS_PROJ");
            return env ? static_cast<float>(std::atof(env)) : 4.0f / 3.0f;
        }();
        float value;
        std::memcpy(&value, clipZ, 4);
        value *= clipScale;
        std::memcpy(clipZ, &value, 4);
        float original;
        std::memcpy(&original, projX, 4);
        value = original * projScale;
        std::memcpy(projX, &value, 4);
        g_widenedProjection[camera] = {value, original};
    }

    // Movie audio: xglMpeg2Play (0x222428, decomp src/main/xgl_1.c) starts the
    // movie's audio stream on its first frame with a pitch made from
    // info->audioSampleRate (+0x90), which audioCallback reads from the first
    // audio packet. The FFmpeg-backed sceMpeg can hand over the first picture
    // before that packet is demuxed: the rate is still 0, the stream plays at
    // pitch 0 and the whole movie is silent. PSS audio is 48 kHz, so supply
    // that if no header has arrived by the first frame.
    constexpr uint32_t kMpeg2PlayAddr = 0x00222428u;
    PS2Runtime::RecompiledFunction g_mpeg2PlayOriginal = nullptr;

    void mpeg2Play_audioRate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t info = GPR_U32(ctx, 4);
        int32_t frameCount, rate;
        std::memcpy(&frameCount, rdram + ((info + 0x38u) & kRdramMask), 4);
        std::memcpy(&rate, rdram + ((info + 0x90u) & kRdramMask), 4);
        static bool logged = false;
        if (frameCount == 0)
        {
            if (rate == 0)
            {
                const int32_t standard = 48000;
                std::memcpy(rdram + ((info + 0x90u) & kRdramMask), &standard, 4);
                if (!logged)
                    std::printf("[movie] no audio header before the first frame: using 48000 Hz\n");
            }
            else if (!logged)
                std::printf("[movie] audio header ready at the first frame: %d Hz\n", rate);
            logged = true;
        }
        else
            logged = false;
        g_mpeg2PlayOriginal(rdram, ctx, runtime);
    }

    void installMovieAudioHook()
    {
        const uint32_t slot = (kMpeg2PlayAddr - g_ps2RecompiledFunctionTableBase) >> 2;
        if (g_mpeg2PlayOriginal || slot >= g_ps2RecompiledFunctionTableSlotCount || !g_ps2RecompiledFunctionTable[slot])
            return;
        g_mpeg2PlayOriginal = g_ps2RecompiledFunctionTable[slot];
        g_ps2RecompiledFunctionTable[slot] = mpeg2Play_audioRate;
    }

    void installWidescreenHook()
    {
        installMovieAudioHook();
        const uint32_t slot = (kCameraViewScreenAddr - g_ps2RecompiledFunctionTableBase) >> 2;
        if (g_cameraViewScreenOriginal || slot >= g_ps2RecompiledFunctionTableSlotCount ||
            !g_ps2RecompiledFunctionTable[slot])
            return;
        g_cameraViewScreenOriginal = g_ps2RecompiledFunctionTable[slot];
        g_ps2RecompiledFunctionTable[slot] = cameraViewScreen_widescreen;
    }

    // ---------------------------------------------------------------------
    // Gameplay options (settings.ini [gameplay]), ported from Beyond's PCSX2
    // patch set (6D1276AB.pnach) as wrappers around the game's functions
    // instead of code patches. A wrapper that calls the original checks
    // runtime->m_checkpointYielded afterwards: the original was paused part
    // way (another guest thread runs) and will finish on its own later, so
    // the wrapper must not run its "after" logic then.
    constexpr uint32_t kGameMode = 0x0033868Cu;        // u16, 5 = battle
    constexpr uint32_t kBattleSignature = 0x00A09A90u; // 0x26027568 when the battle module is loaded
    constexpr uint32_t kBattleState = 0x00A5756Cu;     // 1 = choosing commands
    constexpr uint32_t kBattleCounter = 0x00A5A1E8u;   // battle frame counter
    constexpr uint32_t kBattleHandler = 0x00A57B68u;   // current battle action handler
    constexpr uint32_t kTurnInputState = 0x00A57578u;  // 0/1 attack screen, 4 action running, else menus
    constexpr uint32_t kPadButtons = 0x00490DB8u;      // u16 held buttons (0x800 = Start)
    constexpr uint32_t kFaceRemap = 0x00490DE8u;       // one byte per face button (Circle 5, Cross 6)

    uint32_t readU32(const uint8_t *rdram, uint32_t addr)
    {
        uint32_t v;
        std::memcpy(&v, rdram + (addr & kRdramMask), 4);
        return v;
    }

    uint16_t readU16(const uint8_t *rdram, uint32_t addr)
    {
        uint16_t v;
        std::memcpy(&v, rdram + (addr & kRdramMask), 2);
        return v;
    }

    bool inBattle(const uint8_t *rdram)
    {
        return readU16(rdram, kGameMode) == 5u && readU32(rdram, kBattleSignature) == 0x26027568u;
    }

    struct GameOptions
    {
        bool fastForward = ps2x_hostSettingBool("battle_fast_forward", true);
        bool hideFlash = ps2x_hostSettingBool("hide_battle_flash", false);
        bool fasterCombos = ps2x_hostSettingBool("faster_combos", false);
        bool swapConfirm = ps2x_hostSettingBool("swap_confirm_cancel", false);
    };
    const GameOptions &gameOptions()
    {
        static const GameOptions options; // read on first use, after settings.ini is loaded
        return options;
    }

    PS2Runtime::RecompiledFunction g_renderSyncMoveOriginal = nullptr;
    PS2Runtime::RecompiledFunction g_createEffectOriginal = nullptr;
    PS2Runtime::RecompiledFunction g_isFinishEffectOriginal = nullptr;
    PS2Runtime::RecompiledFunction g_battleExecOriginal = nullptr;
    PS2Runtime::RecompiledFunction g_dataPadReadOriginal = nullptr;

    // Battle fast-forward: Start in battle toggles it; while on (and not in
    // the command menu) the frame limiter xglRenderSyncMove (0x22CC08) takes
    // its one-vsync path (mode byte gp-0x37B4 = 1), so the game's full 30fps
    // step runs as often as the port can manage, up to every 60Hz frame.
    // Nothing is skipped. Also the once-per-frame place for the button swap.
    void renderSyncMove_options(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const GameOptions &opt = gameOptions();
        const bool battle = inBattle(rdram);

        if (opt.swapConfirm) // field and menus: Circle confirms, Cross cancels
        {
            rdram[(kFaceRemap + 5u) & kRdramMask] = battle ? 0x20u : 0x40u;
            rdram[(kFaceRemap + 6u) & kRdramMask] = battle ? 0x40u : 0x20u;
        }

        static bool fastForward = false;
        static bool startWasDown = false;
        if (!battle || !opt.fastForward)
        {
            fastForward = false;
            startWasDown = false;
        }
        else
        {
            const bool startDown = (readU16(rdram, kPadButtons) & 0x0800u) != 0u;
            if (startDown && !startWasDown)
                fastForward = !fastForward;
            startWasDown = startDown;
        }
        ps2x_setHostBadge(fastForward ? ">> x2" : nullptr);

        const uint32_t state = readU32(rdram, kBattleState);
        static uint32_t lastState = ~0u;
        if (battle && state != lastState) // which states are what (results screen...)
            std::printf("[options] battle state %u (fast-forward %d)\n", state, fastForward ? 1 : 0);
        lastState = battle ? state : ~0u;

        // The limiter reads the mode byte (gp-0x37B4; 0 = 30fps, 1 = 60fps,
        // only the debug menu changes it) at its top. Set it for every frame
        // rather than saving and restoring around the call: the call can be
        // suspended inside its vsync wait and re-entered, and a restore could
        // then leave the game stuck at 60.
        uint8_t &mode = rdram[(GPR_U32(ctx, 28) - 0x37B4u) & kRdramMask];
        static int baseMode = -1;
        static bool modeForced = false;
        if (baseMode < 0 || !modeForced)
            baseMode = mode; // the game's own value while we're not forcing it

        // At most one fast frame per 1/60 s of real time: after a slow
        // stretch the emulated vsync briefly runs fast to catch up, which
        // would otherwise play attacks at 3-4x.
        using Clock = std::chrono::steady_clock;
        static Clock::time_point lastFast{};
        const Clock::time_point now = Clock::now();
        const bool fast = fastForward && state != 1u && now - lastFast >= std::chrono::microseconds(16000);
        if (fast)
            lastFast = now;
        mode = fast ? 1u : static_cast<uint8_t>(baseMode);
        modeForced = fast;
        g_renderSyncMoveOriginal(rdram, ctx, runtime);
    }

    // Hide battle flash: effect 17 is the full-screen flash. ID 0 creates nothing.
    void createEffect_options(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (gameOptions().hideFlash && GPR_U32(ctx, 4) == 17u)
            SET_GPR_U32(ctx, 4, 0u);
        g_createEffectOriginal(rdram, ctx, runtime);
    }

    // Faster combos, part 1: after each attack the battle (call at 0xA0CB9C
    // in scenarioBatExecPhase50) waits until all effects have faded
    // (sefIsFinishEffect != 0). Treat 4 consecutive ticks of waiting as done.
    void isFinishEffect_options(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = GPR_U32(ctx, 31);
        g_isFinishEffectOriginal(rdram, ctx, runtime);
        if (runtime->m_checkpointYielded || ra != 0x00A0CBA4u || !gameOptions().fasterCombos)
            return;
        static uint32_t ticks = 0;
        static uint32_t lastCall = 0;
        if (GPR_U32(ctx, 2) != 0u)
        {
            ticks = 0;
            return;
        }
        const uint32_t now = readU32(rdram, kBattleCounter);
        ticks = (now - lastCall < 5u) ? ticks + 1u : 1u; // calls in a row = one wait
        lastCall = now;
        if (ticks >= 4u)
            SET_GPR_U32(ctx, 2, 1u);
    }

    // Faster combos, part 2: battleExec (0xA09A80, called once per battle tick
    // from 0xA078D0) moves the battle state one step per tick. Between one
    // action and the next (state 2 or 8 under handler 0xA0BEB8, moving to 8 or
    // 1) run the following steps in the same tick, up to 6 in all.
    void battleExec_options(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        constexpr uint32_t kReturn = 0x00A078D8u;
        if (GPR_U32(ctx, 31) != kReturn || !gameOptions().fasterCombos)
        {
            g_battleExecOriginal(rdram, ctx, runtime);
            return;
        }
        for (int steps = 6;;)
        {
            const uint32_t before = readU32(rdram, kBattleState);
            g_battleExecOriginal(rdram, ctx, runtime);
            if (runtime->m_checkpointYielded || --steps <= 0)
                return;
            const uint32_t after = readU32(rdram, kBattleState);
            if (after == before || (before != 2u && before != 8u) ||
                readU32(rdram, kBattleHandler) != 0x00A0BEB8u || (after != 8u && after != 1u))
                return;
            SET_GPR_U32(ctx, 31, kReturn);
        }
    }

    // Swap confirm/cancel, battle menus: the battle reads buttons through
    // dataPadRead (0xA1D2D0). Swap Circle (0x20) and Cross (0x40) only in the
    // command menu, lists and target pickers (turn states 2-15 except 4).
    void dataPadRead_options(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_dataPadReadOriginal(rdram, ctx, runtime);
        if (runtime->m_checkpointYielded || !gameOptions().swapConfirm)
            return;
        const uint32_t state = readU32(rdram, kTurnInputState);
        if (state - 2u >= 14u || state == 4u)
            return;
        const uint32_t v = GPR_U32(ctx, 2);
        const uint32_t swapped = (v & ~0x60u) | ((v & 0x20u) << 1) | ((v & 0x40u) >> 1);
        SET_GPR_U32(ctx, 2, swapped);
    }

    // The save screen's thumbnail (GameSnapShotSaveThumbnail, called with quality
    // 30 from 0x289DB4) must fit 4 KB or the game calls exit(1). A busy picture
    // that doesn't fit is encoded again at lower quality rather than quitting.
    // The retry lives at the exit() call, because the encoder can pause at a
    // checkpoint and then return straight into the save screen's code, past
    // anything wrapped around the encoder itself.
    PS2Runtime::RecompiledFunction g_saveThumbnailOriginal = nullptr;
    PS2Runtime::RecompiledFunction g_exitOriginal = nullptr;
    uint32_t g_thumbnailQuality = 0, g_thumbnailBuffer = 0;

    void saveThumbnail_guard(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        constexpr uint32_t kFileScreenReturn = 0x00289DBCu;
        if (GPR_U32(ctx, 31) == kFileScreenReturn)
        {
            g_thumbnailQuality = GPR_U32(ctx, 4);
            g_thumbnailBuffer = GPR_U32(ctx, 5);
            std::printf("[save] encoding thumbnail at quality %u\n", g_thumbnailQuality);
        }
        g_saveThumbnailOriginal(rdram, ctx, runtime);
    }

    // Remembers where the encoder reports its size (request +8).
    PS2Runtime::RecompiledFunction g_jpegEncodeOriginal = nullptr;
    uint32_t g_jpegRequest = 0;

    void jpegEncode_note(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_jpegRequest = GPR_U32(ctx, 4);
        g_jpegEncodeOriginal(rdram, ctx, runtime);
    }

    uint32_t lastJpegSize(const uint8_t *rdram)
    {
        uint32_t size = 0;
        std::memcpy(&size, rdram + ((g_jpegRequest + 8u) & 0x01FFFFFFu), sizeof(size));
        return size;
    }

    void exit_guard(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        constexpr uint32_t kAfterThumbnailCheck = 0x00289DD0u;
        const uint32_t ra = GPR_U32(ctx, 31);
        if (ra == kAfterThumbnailCheck)
        {
            const uint32_t size = lastJpegSize(rdram);
            std::printf("[save] thumbnail was %u bytes at quality %u\n", size, g_thumbnailQuality);
            if (std::getenv("XS_TEST_SAVE") && size < 0x100000u)
            {
                char name[64];
                std::snprintf(name, sizeof(name), "xs_thumb_q%u.jpg", g_thumbnailQuality);
                if (FILE *f = std::fopen(name, "wb"))
                {
                    std::fwrite(rdram + (g_thumbnailBuffer & 0x01FFFFFFu), 1, size, f);
                    std::fclose(f);
                }
            }
        }
        if (ra != kAfterThumbnailCheck)
        {
            std::printf("[exit] the game called exit(%d) from 0x%x\n", static_cast<int>(GPR_U32(ctx, 4)), ra);
            std::fflush(stdout);
            g_exitOriginal(rdram, ctx, runtime);
            return;
        }
        g_thumbnailQuality /= 2u;
        if (g_thumbnailQuality >= 4u)
        {
            std::printf("[save] thumbnail over 4 KB, trying quality %u\n", g_thumbnailQuality);
            SET_GPR_U32(ctx, 4, g_thumbnailQuality);
            SET_GPR_U32(ctx, 5, g_thumbnailBuffer);
            SET_GPR_U32(ctx, 31, 0x00289DBCu);
            ctx->pc = 0x0024DD18u; // GameSnapShotSaveThumbnail
            return;
        }
        // Better a save with a blank picture than no save.
        std::printf("[save] thumbnail still over 4 KB, saving without one\n");
        std::memset(rdram + (g_thumbnailBuffer & 0x01FFFFFFu), 0, 0x1000u);
        ctx->pc = kAfterThumbnailCheck;
    }

    // Test aid, only with XS_TEST_SAVE=<field frames>: opens the save screen that
    // many frames into any field scene, as if standing on a save point, and presses
    // Circle every second inside it, so saving can be tested with nobody at the PC.
    PS2Runtime::RecompiledFunction g_checkTalkPointOriginal = nullptr;
    PS2Runtime::RecompiledFunction g_menuFileOriginal = nullptr;
    uint32_t g_saveTestAfter = 0;
    bool g_saveTestSaved = false;

    void checkTalkPoint_saveTest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        constexpr uint32_t kLoopFlags = 0x00338690u, kPadPressed = 0x00490DBAu; // GameLoopState.flags, PadData.pressed
        static uint32_t calls = 0;
        if (!g_saveTestSaved && ++calls >= g_saveTestAfter && (calls - g_saveTestAfter) % 60u == 0u)
        {
            std::printf("[save-test] asking for the save screen (field frame %u)\n", calls);
            *reinterpret_cast<uint32_t *>(rdram + kLoopFlags) |= 0x20000000u;
            *reinterpret_cast<uint16_t *>(rdram + kPadPressed) |= 0x0020u;
        }
        g_checkTalkPointOriginal(rdram, ctx, runtime);
    }

    // The yes/no question starts on "No": answer Yes.
    PS2Runtime::RecompiledFunction g_menuSelectMoveOriginal = nullptr;

    void menuSelectMove_saveTest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t rows = GPR_U32(ctx, 5);
        g_menuSelectMoveOriginal(rdram, ctx, runtime);
        if (rows == 2u && !runtime->m_checkpointYielded)
            SET_GPR_U32(ctx, 2, 0u);
    }

    void menuFile_saveTest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        constexpr uint32_t kPadPressed = 0x00490DBAu;
        constexpr uint32_t kPadMenu = 0x00490DC0u, kFileWork = 0x004DAA94u; // PadData +0x30, FileWork
        static uint32_t calls = 0;
        static uint8_t lastState = 0;
        uint32_t work = 0;
        std::memcpy(&work, rdram + kFileWork, sizeof(work));
        const uint8_t state = rdram[(work + 3u) & 0x01FFFFFFu];
        if (state != lastState)
        {
            std::printf("[save-test] menu state 0x%02x after 0x%02x\n", state, lastState);
            // The "save complete" message (0x2E) still wants Circle.
            if (lastState == 0x2Eu)
                g_saveTestSaved = true;
            lastState = state;
        }
        if (++calls % 60u == 0u)
        {
            // Circle until something saved, then Cross back out to the field.
            const uint16_t press = g_saveTestSaved ? 0x0040u : 0x0020u;
            *reinterpret_cast<uint16_t *>(rdram + kPadPressed) |= press;
            *reinterpret_cast<uint16_t *>(rdram + kPadMenu) |= press;
        }
        g_menuFileOriginal(rdram, ctx, runtime);
    }

    // XS_RTPS_LOG=<n>: log the first n xglRotTransPersN calls (camera, matrix,
    // points in, screen points out) - menu background debugging.
    PS2Runtime::RecompiledFunction g_rotTransPersNOriginal = nullptr;
    PS2Runtime::RecompiledFunction g_studioGetCamera2Original = nullptr;

    void rotTransPersN_log(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static int budget = []() { const char *e = std::getenv("XS_RTPS_LOG"); return e ? std::atoi(e) : 0; }();
        const uint32_t dest = GPR_U32(ctx, 4), matrix = GPR_U32(ctx, 5), points = GPR_U32(ctx, 6);
        const int32_t count = static_cast<int32_t>(GPR_U32(ctx, 7));
        const uint32_t cameraId = GPR_U32(ctx, 8);
        const uint32_t ra = GPR_U32(ctx, 31);
        const auto mem = [&](uint32_t addr) -> const uint8_t *
        {
            if ((addr & 0xF0000000u) == 0x70000000u)
                return runtime->memory().getScratchpad() + (addr & 0x3FFFu);
            return rdram + (addr & 0x01FFFFFFu);
        };
        const auto f4 = [&](uint32_t addr, const char *name)
        {
            float v[4];
            std::memcpy(v, mem(addr), sizeof(v));
            std::printf(" %s(%g %g %g %g)", name, v[0], v[1], v[2], v[3]);
        };
        const bool log = budget > 0 && count > 0 && count <= 64;
        if (log)
        {
            --budget;
            // xglStudioGetCamera2(id): camera table; read through the game's own function.
            R5900Context probeCtx = *ctx;
            R5900Context *probe = &probeCtx;
            SET_GPR_U32(probe, 4, cameraId);
            SET_GPR_U32(probe, 31, ra);
            g_studioGetCamera2Original(rdram, probe, runtime);
            const uint32_t camera = GPR_U32(probe, 2);
            std::printf("[rtps] ra=0x%x cam=%u @0x%x count=%d matrix=0x%x\n", ra, cameraId, camera, count, matrix);
            std::printf("[rtps]  ");
            f4(camera + 0x70, "offset");
            f4(camera + 0x80, "scale");
            std::printf("\n[rtps]  view");
            for (int r = 0; r < 4; ++r)
                f4(camera + 0x470 + r * 16, "");
            if (matrix)
            {
                std::printf("\n[rtps]  mat ");
                for (int r = 0; r < 4; ++r)
                    f4(matrix + r * 16, "");
            }
            std::printf("\n[rtps]  in  ");
            for (int i = 0; i < count && i < 8; ++i)
                f4(points + i * 16, "");
            std::printf("\n");
        }
        g_rotTransPersNOriginal(rdram, ctx, runtime);
        if (log)
        {
            std::printf("[rtps]  out ");
            for (int i = 0; i < count && i < 8; ++i)
            {
                int32_t v[4];
                std::memcpy(v, mem(dest + i * 16), sizeof(v));
                std::printf(" (%d %d %d %d)", v[0], v[1], v[2], v[3]);
            }
            std::printf("\n");
            std::fflush(stdout);
        }
    }

    // Puts a wrapper in the function table slot for addr; `original` keeps
    // what was there (an overlay's slot is refilled whenever it loads).
    void wrapSlot(uint32_t addr, PS2Runtime::RecompiledFunction wrapper, PS2Runtime::RecompiledFunction &original)
    {
        const uint32_t slot = (addr - g_ps2RecompiledFunctionTableBase) >> 2;
        if (slot >= g_ps2RecompiledFunctionTableSlotCount)
            return;
        PS2Runtime::RecompiledFunction current = g_ps2RecompiledFunctionTable[slot];
        if (!current || current == wrapper)
            return;
        original = current;
        g_ps2RecompiledFunctionTable[slot] = wrapper;
    }

    // Debug toggles (graphics menu / settings.ini [debug]):
    // - one-hit kills: calcHp (ov01 0xA12428, decomp src/ov01/calc.c) takes
    //   (BattleUnit *unit, int delta); unit +0x10 is its Actor, whose flags
    //   word has ACTOR_FLAG_ENEMY (0x40). Any HP loss on an enemy becomes
    //   lethal (calcHp clamps at 0).
    // - no encounters: Check_Encount (0x2D11D8, src/main/set_motion.c) with
    //   enable = 0 is an enemy touching the player; enable = 1 is a scripted
    //   battle (bosses, story fights), which still happens.
    PS2Runtime::RecompiledFunction g_calcHpOriginal = nullptr;
    PS2Runtime::RecompiledFunction g_checkEncountOriginal = nullptr;

    void calcHp_debug(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ps2x_debugOneHitKill())
        {
            const uint32_t unit = GPR_U32(ctx, 4);
            const int32_t delta = static_cast<int32_t>(GPR_U32(ctx, 5));
            uint32_t actor, flags;
            std::memcpy(&actor, rdram + ((unit + 0x10u) & kRdramMask), 4);
            std::memcpy(&flags, rdram + (actor & kRdramMask), 4);
            if (actor != 0u && (flags & 0x40u) != 0u && delta < 0)
                SET_GPR_S32(ctx, 5, -0x3FFFFFFF);
        }
        g_calcHpOriginal(rdram, ctx, runtime);
    }

    void checkEncount_debug(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ps2x_debugNoEncounters() && static_cast<int8_t>(GPR_U32(ctx, 5)) == 0)
            return; // touched by an enemy: no battle
        g_checkEncountOriginal(rdram, ctx, runtime);
    }

    // Landmarks for following a session in the log: every named disc read
    // (areas, events, models), movie starts and overlay loads, stamped with
    // seconds since start. Same name twice in a row is logged once.
    PS2Runtime::RecompiledFunction g_cdReadFilePartOriginal = nullptr;
    PS2Runtime::RecompiledFunction g_gameMpeg2PlayOriginal = nullptr;
    PS2Runtime::RecompiledFunction g_movieOpenOriginal = nullptr;

    double landmarkSeconds()
    {
        static const auto start = std::chrono::steady_clock::now();
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    }

    std::string guestString(uint8_t *rdram, uint32_t addr)
    {
        std::string out;
        for (uint32_t i = 0; i < 96u; ++i)
        {
            const char c = static_cast<char>(rdram[(addr + i) & kRdramMask]);
            if (c == 0)
                break;
            out.push_back(c >= 32 && c < 127 ? c : '?');
        }
        return out;
    }

    void landmark(const char *kind, const std::string &what)
    {
        static std::string last;
        const std::string key = std::string(kind) + what;
        if (key == last)
            return;
        last = key;
        std::printf("[landmark] %.1fs %s %s\n", landmarkSeconds(), kind, what.c_str());
        std::fflush(stdout);
    }

    void cdReadFilePart_landmark(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (const uint32_t name = GPR_U32(ctx, 4))
            landmark("file", guestString(rdram, name));
        g_cdReadFilePartOriginal(rdram, ctx, runtime);
    }

    PS2Runtime::RecompiledFunction g_cdReadFileOriginal = nullptr;
    void cdReadFile_landmark(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (const uint32_t name = GPR_U32(ctx, 4))
            landmark("file", guestString(rdram, name));
        g_cdReadFileOriginal(rdram, ctx, runtime);
    }

    void gameMpeg2Play_landmark(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        landmark("movie", guestString(rdram, GPR_U32(ctx, 4)));
        g_gameMpeg2PlayOriginal(rdram, ctx, runtime);
        landmark("movie-end", "");
    }

    void movieOpen_landmark(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t name = GPR_U32(ctx, 5); // a name, or a mode number (unit6003.c)
        landmark("ipu-movie", name >= 0x100000u ? guestString(rdram, name) : std::to_string(name));
        g_movieOpenOriginal(rdram, ctx, runtime);
    }

    void installGameOptionHooks(uint32_t overlay)
    {
        landmark("overlay", std::to_string(overlay));
        wrapSlot(0x0021DEB8u, cdReadFilePart_landmark, g_cdReadFilePartOriginal); // xglCdReadFilePart
        wrapSlot(0x0021E310u, cdReadFile_landmark, g_cdReadFileOriginal);         // xglCdReadFile
        wrapSlot(0x002507C8u, gameMpeg2Play_landmark, g_gameMpeg2PlayOriginal);   // GameMpeg2Play
        wrapSlot(0x002210F0u, movieOpen_landmark, g_movieOpenOriginal);           // xglMovieOpen
        wrapSlot(0x002D11D8u, checkEncount_debug, g_checkEncountOriginal); // Check_Encount
        wrapSlot(0x0022CC08u, renderSyncMove_options, g_renderSyncMoveOriginal); // xglRenderSyncMove
        wrapSlot(0x002E7830u, createEffect_options, g_createEffectOriginal);     // sefCreateEffectCf2
        wrapSlot(0x002E7A18u, isFinishEffect_options, g_isFinishEffectOriginal); // sefIsFinishEffect
        wrapSlot(0x0024DD18u, saveThumbnail_guard, g_saveThumbnailOriginal);     // GameSnapShotSaveThumbnail
        wrapSlot(0x0032E168u, exit_guard, g_exitOriginal);                       // exit
        wrapSlot(0x00223FC0u, jpegEncode_note, g_jpegEncodeOriginal);            // xglJpegEncode
        if (std::getenv("XS_RTPS_LOG"))
        {
            const uint32_t slot = (0x0022C6A0u - g_ps2RecompiledFunctionTableBase) >> 2; // xglStudioGetCamera2
            g_studioGetCamera2Original = g_ps2RecompiledFunctionTable[slot];
            wrapSlot(0x00229AB0u, rotTransPersN_log, g_rotTransPersNOriginal); // xglRotTransPersN
        }
        static const uint32_t saveTest = []() -> uint32_t {
            const char *v = std::getenv("XS_TEST_SAVE");
            return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 0u;
        }();
        if (saveTest)
        {
            g_saveTestAfter = saveTest;
            wrapSlot(0x002458D0u, checkTalkPoint_saveTest, g_checkTalkPointOriginal); // checkTalkPoint
            wrapSlot(0x00287888u, menuFile_saveTest, g_menuFileOriginal);             // MenuFile
            wrapSlot(0x00274E08u, menuSelectMove_saveTest, g_menuSelectMoveOriginal); // MenuSelectMove
        }
        if (overlay == 1) // battle/field module
        {
            wrapSlot(0x00A09A80u, battleExec_options, g_battleExecOriginal);   // battleExec
            wrapSlot(0x00A1D2D0u, dataPadRead_options, g_dataPadReadOriginal); // dataPadRead
            wrapSlot(0x00A12428u, calcHp_debug, g_calcHpOriginal);             // calcHp
        }
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
        installGameOptionHooks(id);
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
