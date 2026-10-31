// Xenosaga Episode I (SLUS_204.69): debugging aids.
//
// A heartbeat that logs, every few seconds, where each game thread is and
// whether it is running or waiting, by function name. The names come from the
// symbol tables in the user's own SLUS_204.69 and OVnn.OVL files.

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_profile.h"
#include "runtime/ps2_memory.h"
#include "runtime/gs/gs_frontend.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

void xglRenderDispEnvInit_0x22d338(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
void xglRenderDispOff_0x22d7b8(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
void xglRenderDispOn_0x22d7d8(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
void xglRenderInit_0x22d7f8(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

bool xs_readDiscFile(const std::string &path, std::vector<uint8_t> &out);
uint32_t xs_currentOverlay();

namespace
{
    struct Symbol
    {
        uint32_t addr;
        uint32_t size;
        std::string name;
    };

    // FUNC symbols from an ELF32 image, sorted by address.
    std::vector<Symbol> readFunctionSymbols(const std::vector<uint8_t> &d)
    {
        std::vector<Symbol> out;
        auto u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, &d[o], 2); return v; };
        auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, &d[o], 4); return v; };
        if (d.size() < 0x34 || std::memcmp(d.data(), "\x7f" "ELF", 4) != 0)
            return out;
        const uint32_t shoff = u32(0x20);
        const uint16_t shentsize = u16(0x2E), shnum = u16(0x30);
        for (uint16_t i = 0; i < shnum; ++i)
        {
            const size_t sh = shoff + size_t(i) * shentsize;
            if (sh + 40 > d.size() || u32(sh + 4) != 2 /* SHT_SYMTAB */)
                continue;
            const uint32_t off = u32(sh + 16), size = u32(sh + 20), link = u32(sh + 24);
            const size_t strSh = shoff + size_t(link) * shentsize;
            if (strSh + 40 > d.size())
                continue;
            const uint32_t strOff = u32(strSh + 16), strSize = u32(strSh + 20);
            for (uint32_t s = off; s + 16 <= off + size && s + 16 <= d.size(); s += 16)
            {
                const uint8_t info = d[s + 12];
                if ((info & 0xF) != 2 /* STT_FUNC */)
                    continue;
                const uint32_t nameOff = u32(s);
                if (nameOff >= strSize || strOff + nameOff >= d.size())
                    continue;
                out.push_back({u32(s + 4), u32(s + 8), reinterpret_cast<const char *>(&d[strOff + nameOff])});
            }
        }
        std::sort(out.begin(), out.end(), [](const Symbol &a, const Symbol &b) { return a.addr < b.addr; });
        return out;
    }

    std::vector<Symbol> g_mainSymbols;
    std::map<uint32_t, std::vector<Symbol>> g_overlaySymbols;
    std::mutex g_symbolMutex;

    std::string lookupIn(const std::vector<Symbol> &syms, uint32_t pc)
    {
        auto it = std::upper_bound(syms.begin(), syms.end(), pc, [](uint32_t v, const Symbol &s) { return v < s.addr; });
        if (it == syms.begin())
            return {};
        --it;
        if (pc >= it->addr + std::max<uint32_t>(it->size, 4u) && it->size != 0)
            return {};
        char buf[32];
        std::snprintf(buf, sizeof(buf), "+0x%x", pc - it->addr);
        return it->name + buf;
    }

    std::string describePc(uint32_t pc)
    {
        std::lock_guard<std::mutex> lock(g_symbolMutex);
        if (pc >= 0x009FF000u && pc < 0x00A60000u)
        {
            const uint32_t ov = xs_currentOverlay();
            auto &syms = g_overlaySymbols[ov];
            if (syms.empty())
            {
                char name[24];
                std::snprintf(name, sizeof(name), "\\OV%02u.OVL;1", ov);
                std::vector<uint8_t> data;
                if (xs_readDiscFile(name, data))
                    syms = readFunctionSymbols(data);
            }
            char prefix[16];
            std::snprintf(prefix, sizeof(prefix), "ov%02u:", ov);
            return prefix + lookupIn(syms, pc);
        }
        return lookupIn(g_mainSymbols, pc);
    }

    const char *statusName(EeThreadStatus s)
    {
        switch (s)
        {
        case EeThreadStatus::Running: return "running";
        case EeThreadStatus::Ready: return "ready";
        case EeThreadStatus::Waiting: return "waiting";
        case EeThreadStatus::WaitingSuspended: return "waiting+suspended";
        case EeThreadStatus::Suspended: return "suspended";
        case EeThreadStatus::Dormant: return "dormant";
        }
        return "?";
    }

    const char *waitName(EeWaitReason w)
    {
        switch (w)
        {
        case EeWaitReason::None: return "";
        case EeWaitReason::Sleep: return " (sleep)";
        case EeWaitReason::Semaphore: return " (semaphore)";
        case EeWaitReason::EventFlag: return " (event flag)";
        case EeWaitReason::VSync: return " (vsync)";
        case EeWaitReason::External: return " (external)";
        case EeWaitReason::Mpeg: return " (mpeg)";
        }
        return "";
    }
}

namespace
{
    // Save the frame the runtime would present, as a BMP next to the exe, so
    // it can be inspected even when the game window can't be seen.
    bool saveFrameBmp(PS2Runtime &runtime, const std::filesystem::path &path)
    {
        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        if (!runtime.gs().copyLatchedHostPresentationFrame(rgba, w, h, nullptr, nullptr, nullptr) || !w || !h)
            return false;
        const uint32_t rowBytes = w * 3u, pad = (4u - rowBytes % 4u) % 4u;
        const uint32_t dataSize = (rowBytes + pad) * h;
        std::ofstream out(path, std::ios::binary);
        auto put16 = [&](uint16_t v) { out.write(reinterpret_cast<const char *>(&v), 2); };
        auto put32 = [&](uint32_t v) { out.write(reinterpret_cast<const char *>(&v), 4); };
        out.write("BM", 2); put32(54 + dataSize); put32(0); put32(54);
        put32(40); put32(w); put32(h); put16(1); put16(24); put32(0); put32(dataSize);
        put32(2835); put32(2835); put32(0); put32(0);
        const char zeros[3] = {0, 0, 0};
        for (uint32_t y = h; y-- > 0;)
        {
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint8_t *p = &rgba[(size_t(y) * w + x) * 4u];
                const char bgr[3] = {char(p[2]), char(p[1]), char(p[0])};
                out.write(bgr, 3);
            }
            out.write(zeros, pad);
        }
        return true;
    }
}

void xs_startHeartbeat(PS2Runtime &runtime)
{
    const char *env = std::getenv("XS_HEARTBEAT_SECONDS");
    const int seconds = env ? std::atoi(env) : 5;
    if (seconds <= 0)
        return;

    {
        std::ifstream in(PS2Runtime::getIoPaths().elfPath, std::ios::binary);
        std::vector<uint8_t> elf((std::istreambuf_iterator<char>(in)), {});
        g_mainSymbols = readFunctionSymbols(elf);
    }
    std::cout << "[xenosaga] heartbeat every " << seconds << "s (" << g_mainSymbols.size() << " symbols)" << std::endl;

    std::thread([&runtime, seconds]() {
        uint64_t lastCycle = 0;
        for (int beat = 1;; ++beat)
        {
            std::this_thread::sleep_for(std::chrono::seconds(seconds));
            if (runtime.isStopRequested())
                return;
            const EeKernelSnapshot snap = runtime.eeScheduler().snapshot();
            std::cout << "[heartbeat " << beat * seconds << "s] cycles=" << snap.eeCycle
                      << (snap.eeCycle == lastCycle ? " (stalled)" : "")
                      << " running=" << snap.runningThreadId << std::endl;
            lastCycle = snap.eeCycle;
            {
                static const char *names[] = {"vu1", "gs", "vif1"};
                std::printf("  host time over last %ds:", seconds);
                for (uint32_t c = 0; c < ps2_profile::Count; ++c)
                {
                    const uint64_t ns = ps2_profile::g_ns[c].exchange(0);
                    const uint64_t calls = ps2_profile::g_calls[c].exchange(0);
                    std::printf(" %s=%.1f%% (%llu calls)", names[c], 100.0 * ns / (seconds * 1e9),
                                (unsigned long long)calls);
                }
                std::printf("\n");
            }
            {
                const GSRegisters &gs = runtime.memory().gs();
                char regs[200];
                std::snprintf(regs, sizeof(regs),
                              "  gs pmode=0x%llx dispfb1=0x%llx display1=0x%llx dispfb2=0x%llx display2=0x%llx",
                              (unsigned long long)gs.pmode, (unsigned long long)gs.dispfb1,
                              (unsigned long long)gs.display1, (unsigned long long)gs.dispfb2,
                              (unsigned long long)gs.display2);
                const std::filesystem::path shot = PS2Runtime::getIoPaths().elfDirectory / "xs_frame.bmp";
                std::cout << regs << (saveFrameBmp(runtime, shot) ? "  frame saved" : "  no frame") << std::endl;
            }
            for (const auto &t : snap.threads)
            {
                if (t.status == EeThreadStatus::Dormant)
                    continue;
                char head[96];
                std::snprintf(head, sizeof(head), "  thread %d prio %d %s%s pc=0x%06x ", t.id, t.currentPriority,
                              statusName(t.status), waitName(t.waitReason), t.pc);
                std::cout << head << describePc(t.pc) << "  ra=" << describePc(t.ra) << std::endl;
            }
        }
    }).detach();
}

// Trace the display on/off path: log each call with the game's PMODE value
// (dispEnv.pmode at 0x4A9000) before and after.
namespace
{
    uint64_t dispEnvPmode(const uint8_t *rdram)
    {
        uint64_t v;
        std::memcpy(&v, rdram + 0x004A9000u, sizeof(v));
        return v;
    }

    template <void (*Original)(uint8_t *, R5900Context *, PS2Runtime *)>
    void traced(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, const char *name)
    {
        const uint64_t before = dispEnvPmode(rdram);
        const uint32_t ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        Original(rdram, ctx, runtime);
        std::printf("[xenosaga] %s from %s: dispEnv.pmode 0x%llx -> 0x%llx\n", name, describePc(ra).c_str(),
                    (unsigned long long)before, (unsigned long long)dispEnvPmode(rdram));
    }

    // On a PS2, ControlEntry spends many frames loading sound banks before it
    // calls xglRenderDispOn, so the render thread's own xglRenderInit always
    // runs first. With sound skipped that wait is instant and the order flips:
    // the late re-init switches the display back off. Keep it on once requested.
    std::atomic<bool> g_displayOnRequested{false};

    void tracedRenderInit(uint8_t *r, R5900Context *c, PS2Runtime *rt)
    {
        traced<xglRenderInit_0x22d7f8>(r, c, rt, "xglRenderInit");
        if (g_displayOnRequested.load())
        {
            uint64_t pmode = dispEnvPmode(r) | 0x2u; // EN2, as xglRenderDispOn sets
            std::memcpy(r + 0x004A9000u, &pmode, sizeof(pmode));
            std::printf("[xenosaga] display was already switched on; kept on (pmode 0x%llx)\n", (unsigned long long)pmode);
        }
    }
    void tracedDispEnvInit(uint8_t *r, R5900Context *c, PS2Runtime *rt) { traced<xglRenderDispEnvInit_0x22d338>(r, c, rt, "xglRenderDispEnvInit"); }
    void tracedDispOff(uint8_t *r, R5900Context *c, PS2Runtime *rt)
    {
        g_displayOnRequested = false;
        traced<xglRenderDispOff_0x22d7b8>(r, c, rt, "xglRenderDispOff");
    }
    void tracedDispOn(uint8_t *r, R5900Context *c, PS2Runtime *rt)
    {
        g_displayOnRequested = true;
        traced<xglRenderDispOn_0x22d7d8>(r, c, rt, "xglRenderDispOn");
    }
}

void xs_traceDisplay(PS2Runtime &runtime)
{
    runtime.registerFunction(0x0022D7F8u, tracedRenderInit);
    runtime.registerFunction(0x0022D338u, tracedDispEnvInit);
    runtime.registerFunction(0x0022D7B8u, tracedDispOff);
    runtime.registerFunction(0x0022D7D8u, tracedDispOn);
}
