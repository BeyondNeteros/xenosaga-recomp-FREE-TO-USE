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
#include <cmath>
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
#include <set>
#include <string>
#include <thread>
#include <vector>

void xglRenderDispEnvInit_0x22d338(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
void xglRenderDispOff_0x22d7b8(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
void xglRenderDispOn_0x22d7d8(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);
void xglRenderInit_0x22d7f8(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime);

bool xs_readDiscFile(const std::string &path, std::vector<uint8_t> &out);
uint32_t xs_currentOverlay();

// Defined in the runtime (ps2_runtime.cpp).
void ps2x_armVu1Capture();
std::string ps2x_vu1StatsReport();

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
                    if (c == ps2_profile::Vu1Exec && ns > seconds * 400000000ull)
                        ps2x_armVu1Capture(); // VU1 is the bottleneck: record some calls
                }
                std::printf("\n");
                std::fputs(ps2x_vu1StatsReport().c_str(), stdout);
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
                // Raw 4 MB GS memory next to the frame, for decoding textures
                // offline (port/tools/gsvram.py). Taken without locking, so a
                // texture being uploaded at that moment may be torn.
                if (const uint8_t *vram = runtime.memory().getGSVRAM())
                {
                    std::ofstream out(PS2Runtime::getIoPaths().elfDirectory / "xs_vram.bin", std::ios::binary);
                    out.write(reinterpret_cast<const char *>(vram), PS2_GS_VRAM_SIZE);
                }
                // XS_RAM_DUMP=1: also the 32 MB of EE RAM (xs_ram.bin), to diff
                // against a PCSX2 save state's eeMemory.bin. Game data: never commit.
                static const bool ramDump = std::getenv("XS_RAM_DUMP") != nullptr;
                if (ramDump)
                {
                    std::ofstream out(PS2Runtime::getIoPaths().elfDirectory / "xs_ram.bin", std::ios::binary);
                    out.write(reinterpret_cast<const char *>(runtime.memory().getRDRAM()), PS2_RAM_SIZE);
                }
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


    // --- Current-matrix tracing -------------------------------------------
    // Xenosaga keeps a "current matrix" in VU0 vf27-vf30 (ps2__CurMatrixSet/
    // Mul/Get). The world-to-clip matrix it sends to VU1 comes out of this
    // chain, and it arrives garbled. Keep the last few operations and print
    // them whenever a Get returns a non-finite or absurdly large matrix.
    struct MatrixOp
    {
        char kind;
        uint32_t a0, ra;
        float input[16];
        float before[16];
        float after[16];
    };
    MatrixOp g_matrixRing[24];
    uint32_t g_matrixRingPos = 0;
    int g_matrixReports = 0;
    int g_matrixGoodReports = 0;

    void readCur(const R5900Context *ctx, float out[16])
    {
        for (int r = 0; r < 4; ++r)
            _mm_storeu_ps(out + r * 4, ctx->vu0_vf[27 + r]);
    }

    bool absurd(const float m[16])
    {
        for (int i = 0; i < 16; ++i)
            if (!std::isfinite(m[i]) || std::fabs(m[i]) > 1e15f)
                return true;
        return false;
    }

    void printMatrix(const char *label, const float m[16])
    {
        std::printf("      %s", label);
        for (int r = 0; r < 4; ++r)
            std::printf(" (%.4g %.4g %.4g %.4g)", m[r * 4], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]);
        std::printf("\n");
    }

    template <void (*Original)(uint8_t *, R5900Context *, PS2Runtime *), char Kind>
    void tracedMatrix(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        MatrixOp &op = g_matrixRing[g_matrixRingPos++ % 24];
        op.kind = Kind;
        op.a0 = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
        op.ra = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[31]));
        std::memcpy(op.input, rdram + (op.a0 & 0x01FFFFF0u), sizeof(op.input));
        readCur(ctx, op.before);
        Original(rdram, ctx, runtime);
        readCur(ctx, op.after);
        if (Kind != 'G')
            return;
        const bool bad = absurd(op.after);
        if (bad ? g_matrixReports++ >= 6 : g_matrixGoodReports++ >= 2)
            return;
        std::printf("[xenosaga] CurMatrixGet %s (ctx %p): last operations, oldest first\n", bad ? "returned a bad matrix" : "ok", (void *)ctx);
        for (uint32_t i = 0; i < 24; ++i)
        {
            const MatrixOp &o = g_matrixRing[(g_matrixRingPos + i) % 24];
            if (!o.kind)
                continue;
            std::printf("    %c a0=0x%08x from %s\n", o.kind, o.a0, describePc(o.ra).c_str());
            if (o.kind != 'G')
                printMatrix("in    ", o.input);
            printMatrix("before", o.before);
            printMatrix("after ", o.after);
        }
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

// Texture upload path for 3D models: nmlPacketAddTexture picks either the
// model's own texture list (nmlPacketTextureTrans) or a shared texture
// (xglPacketTextureTrans, model+0x240). Log the first calls with the fields
// that decide it, to see whether 3D textures are ever sent to the GS.
namespace
{
    uint32_t rdWord(const uint8_t *rdram, uint32_t addr) { uint32_t v; std::memcpy(&v, rdram + (addr & 0x01FFFFFCu), 4); return v; }
    std::atomic<int> g_texCalls[3];

    std::atomic<int> g_flushCalls{0};
    void tracedModelFlush(uint8_t *r, R5900Context *c, PS2Runtime *rt)
    {
        // Every 25 flushes: how often the 3D texture upload path ran since the last line.
        if (++g_flushCalls % 25 == 0)
        {
            std::printf("[xenosaga] flushes=%d nmlPacketAddTexture=%d nmlPacketTextureTrans=%d xglPacketTextureTrans=%d\n",
                        g_flushCalls.load(), g_texCalls[0].load(), g_texCalls[1].load(), g_texCalls[2].load());
            std::fflush(stdout);
        }
        nmlModelFlush_0x235ff8(r, c, rt);
    }

    void tracedAddTexture(uint8_t *r, R5900Context *c, PS2Runtime *rt)
    {
        const uint32_t model = static_cast<uint32_t>(_mm_cvtsi128_si32(c->r[4]));
        const int n = g_texCalls[0]++;
        if (n < 12)
        {
            const uint32_t info = rdWord(r, model + 0x254);
            std::printf("[xenosaga] nmlPacketAddTexture model=0x%x flags=0x%x info=0x%x info+0x6c=0x%x shared(0x240)=0x%x last=0x%x\n",
                        model, rdWord(r, model + 0x250), info, info ? rdWord(r, info + 0x6c) : 0u, rdWord(r, model + 0x240),
                        rdWord(r, static_cast<uint32_t>(_mm_cvtsi128_si32(c->r[28])) - 0x36e4u));
        }
        nmlPacketAddTexture_0x237da0(r, c, rt);
    }
    void tracedTextureTrans(uint8_t *r, R5900Context *c, PS2Runtime *rt)
    {
        const uint32_t model = static_cast<uint32_t>(_mm_cvtsi128_si32(c->r[4]));
        if (g_texCalls[1]++ < 12)
        {
            const uint32_t info = rdWord(r, model + 0x254);
            const uint32_t list = info + rdWord(r, info + 0x6c);
            std::printf("[xenosaga] nmlPacketTextureTrans model=0x%x list=0x%x count=%d bank=%u\n", model, list,
                        int(rdWord(r, list)), unsigned(rdWord(r, 0x004a90f8u) >> 16));
        }
        nmlPacketTextureTrans_0x237c38(r, c, rt);
    }
    void tracedXglTextureTrans(uint8_t *r, R5900Context *c, PS2Runtime *rt)
    {
        const uint32_t obj = static_cast<uint32_t>(_mm_cvtsi128_si32(c->r[4]));
        const int n = g_texCalls[2]++;
        static const char *seq = std::getenv("PS2X_GS_SEQLOG");
        // Every call once the GS sequence log runs (it starts at about the same
        // time), otherwise the first 12: the XTX entries become CT32 uploads at
        // dbp = (bank*2048 + entry[8]) / 64.
        if (n < 12 || (seq && n < 400))
        {
            const uint32_t count = rdWord(r, obj + 8), table = obj + rdWord(r, obj + 0xc);
            const uint32_t bank = rdWord(r, 0x004a90f8u) >> 16;
            std::printf("[gs:seq] xglPacketTextureTrans obj=0x%x count=%u bank=%u", obj, count, bank);
            for (uint32_t k = 0; k < count && k < 4; ++k)
            {
                const uint32_t e = table + k * 20;
                std::printf(" [dbp=0x%x wh=%ux%u]", (bank * 2048u + rdWord(r, e + 8)) / 64u, rdWord(r, e) & 0xFFFFu,
                            rdWord(r, e + 4) & 0xFFFFu);
            }
            std::printf("\n");
            std::fflush(stdout);
        }
        // Calls 300-302: dump the DMA tags this call appended to the packet
        // (gp-0x3480 points at the packet; its first word is the write pointer).
        const uint32_t pkt = rdWord(r, static_cast<uint32_t>(_mm_cvtsi128_si32(c->r[28])) - 0x3480u);
        const uint32_t before = rdWord(r, pkt);
        xglPacketTextureTrans_0x22c268(r, c, rt);
        if (n >= 300 && n < 303)
        {
            const uint32_t after = rdWord(r, pkt);
            std::printf("[xenosaga] packet 0x%x grew 0x%x -> 0x%x:\n", pkt, before, after);
            for (uint32_t a = before & ~15u; a < after && a < before + 0x200u; a += 16)
                std::printf("    %08x: %08x %08x %08x %08x\n", a, rdWord(r, a), rdWord(r, a + 4), rdWord(r, a + 8), rdWord(r, a + 12));
            std::fflush(stdout);
        }
    }
}

// xglMatrixStackFrustum(l=f12, r=f13, b=f14, t=f15, n=f16, f=f17) runs a VU0
// program that multiplies a frustum into the matrix stack (vf28-31). Log the
// inputs and vf28-31 before/after for the first calls.
namespace
{
    std::atomic<int> g_frustumCalls{0};
    void tracedFrustum(uint8_t *r, R5900Context *c, PS2Runtime *rt)
    {
        const int n = g_frustumCalls++;
        float before[16];
        readCur(c, before); // vf27-30
        xglMatrixStackFrustum_0x22a548(r, c, rt);
        if (n < 6)
        {
            std::printf("[xenosaga] xglMatrixStackFrustum l=%g r=%g b=%g t=%g n=%g f=%g\n", c->f[12], c->f[13], c->f[14],
                        c->f[15], c->f[16], c->f[17]);
            float after[16];
            for (int i = 0; i < 4; ++i)
                _mm_storeu_ps(after + i * 4, c->vu0_vf[28 + i]);
            printMatrix("vf28-31 after", after);
            std::fflush(stdout);
        }
    }
}

// XS_PARTS_DUMP=<seconds>: that long after the first call, print the part
// structures nmlPacketAddParts receives (a1; matrices at +0x180 and +0x80, a2
// camera) for the first 48 distinct parts, to compare with a PCSX2 save state.
namespace
{
    void tracedAddParts(uint8_t *r, R5900Context *c, PS2Runtime *rt)
    {
        static const char *env = std::getenv("XS_PARTS_DUMP");
        if (env)
        {
            static const auto start = std::chrono::steady_clock::now();
            static std::set<uint32_t> seen;
            const uint32_t part = static_cast<uint32_t>(_mm_cvtsi128_si32(c->r[5]));
            if (std::chrono::steady_clock::now() - start > std::chrono::seconds(std::atoi(env)) && seen.size() < 48 &&
                seen.insert(part).second)
            {
                float m180[16], m80[16];
                std::memcpy(m180, r + ((part + 0x180u) & 0x01FFFFF0u), sizeof(m180));
                std::memcpy(m80, r + ((part + 0x80u) & 0x01FFFFF0u), sizeof(m80));
                std::printf("[parts] part=0x%x flags=0x%x model=0x%x cam=0x%x\n", part, rdWord(r, part + 0x250),
                            static_cast<uint32_t>(_mm_cvtsi128_si32(c->r[4])), static_cast<uint32_t>(_mm_cvtsi128_si32(c->r[6])));
                printMatrix("+0x180", m180);
                printMatrix("+0x80 ", m80);
                std::fflush(stdout);
            }
        }
        nmlPacketAddParts_0x237ee8(r, c, rt);
    }
}

void xs_traceDisplay(PS2Runtime &runtime)
{
    runtime.registerFunction(0x00237EE8u, tracedAddParts);
    runtime.registerFunction(0x0022A548u, tracedFrustum);
    runtime.registerFunction(0x00235FF8u, tracedModelFlush);
    runtime.registerFunction(0x00237DA0u, tracedAddTexture);
    runtime.registerFunction(0x00237C38u, tracedTextureTrans);
    runtime.registerFunction(0x0022C268u, tracedXglTextureTrans);
    runtime.registerFunction(0x0022D7F8u, tracedRenderInit);
    runtime.registerFunction(0x0022D338u, tracedDispEnvInit);
    runtime.registerFunction(0x0022D7B8u, tracedDispOff);
    runtime.registerFunction(0x0022D7D8u, tracedDispOn);
    runtime.registerFunction(0x00236448u, tracedMatrix<ps2__CurMatrixSet_0x236448, 'S'>);
    runtime.registerFunction(0x00236478u, tracedMatrix<ps2__CurMatrixMul_0x236478, 'M'>);
    runtime.registerFunction(0x00236460u, tracedMatrix<ps2__CurMatrixGet_0x236460, 'G'>);
}
