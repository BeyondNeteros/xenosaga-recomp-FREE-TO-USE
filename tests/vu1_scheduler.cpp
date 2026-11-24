// Synthetic VU1 microprograms for the shared scheduler and its region exits.
#include "runtime/gs/gs_frontend.h"
#include "runtime/gs/ps2_gif_arbiter.h"
#include "runtime/ps2_memory.h"
#include "runtime/ps2_vu1_fastops.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <vector>

namespace
{
constexpr uint32_t lowerNop = 0x8000033cu;
constexpr uint32_t upperNop = 0x000002ffu;
constexpr uint32_t endBit = 1u << 30;

constexpr uint32_t lowerSpecial(uint32_t op)
{
    return 0x8000003cu | ((op & 0x7cu) << 4) | (op & 3u);
}

void check(bool ok, const char *message)
{
    if (!ok)
    {
        std::fprintf(stderr, "VU1 scheduler: %s\n", message);
        std::exit(1);
    }
}

struct Fixture
{
    PS2Memory memory;
    GS gs;
    GifArbiter gif;
    VU1Interpreter vu;
    VU1Interpreter::ReplayDispatch dispatch;
    std::vector<std::vector<uint8_t>> packets;

    Fixture()
    {
        check(memory.initialize(), "memory initialization");
        gs.init(memory.getGSVRAM(), PS2_GS_VRAM_SIZE, &memory.gs());
        gif.setProcessPacketFn([this](const uint8_t *data, uint32_t bytes) {
            packets.emplace_back(data, data + bytes);
        });
        memory.setGifArbiter(&gif);
    }

    void pair(unsigned index, uint32_t lower = lowerNop, uint32_t upper = upperNop)
    {
        const uint32_t words[] = {lower, upper};
        std::memcpy(memory.getVU1Code() + 8 * index, words, sizeof(words));
    }

    void registerRegion(const uint16_t (&range)[2])
    {
        memory.markVU1CodeModified();
        const vu1native::ProgramEntry entry{vu1native::codeHash(memory.getVU1Code(), range, 1), 0,
                                            range, 1, nullptr, true};
        vu1native::registerPrograms(&entry, 1);
    }

    void slice(uint32_t budget)
    {
        vu.resume(memory.getVU1Code(), PS2_VU1_CODE_SIZE, memory.getVU1Data(), PS2_VU1_DATA_SIZE,
                  gs, &memory, 0, 0, budget, &dispatch);
        gif.drain();
    }
};

VU1State scalarStalls(bool regions, bool split)
{
    static constexpr uint16_t range[] = {0, 56};
    Fixture fixture;
    fixture.pair(0, (8u << 21) | (1u << 16)); // LQ.x VF1, 0(VI0)
    fixture.pair(1, lowerSpecial(0x38) | (1u << 11) | (2u << 16)); // DIV VF1.x / VF2.x
    fixture.pair(2, lowerSpecial(0x3b)); // WAITQ
    fixture.pair(3, lowerSpecial(0x70) | (2u << 11)); // ESADD VF2
    fixture.pair(4, lowerSpecial(0x7b)); // WAITP
    fixture.pair(5, lowerNop, endBit | (8u << 21) | (1u << 11) | (3u << 6) | 0x20u); // ADDQ.x VF3, VF1
    fixture.pair(6);
    fixture.registerRegion(range);
    vu1native::setEnabled(regions);
    fixture.vu.state().vf[2][0] = 2.0f;
    const float input = 8.0f;
    std::memcpy(fixture.memory.getVU1Data(), &input, sizeof(input));

    fixture.slice(0);
    check(fixture.dispatch.programs == 0 && fixture.vu.state().pc == 0, "zero budget issued a pair");
    if (split)
    {
        fixture.slice(3);
        check(fixture.vu.state().cycles == 3 && fixture.vu.state().pc == 8, "LQ dependency crossed the budget");
        check(fixture.dispatch.programs == static_cast<unsigned>(regions) && fixture.dispatch.completedPrograms == 0,
              "region completion during budget exhaustion");
        fixture.slice(8);
        check(fixture.vu.state().cycles == 11 && fixture.vu.state().pc == 16, "WAITQ issued at the budget boundary");
    }
    fixture.slice(split ? 15 : 26);
    check(fixture.dispatch.completedPrograms == static_cast<unsigned>(regions && !split),
          "incorrect region completion after E-bit");
    const auto &state = fixture.vu.state();
    check(state.pc == 56 && state.q == 4.0f && state.p == 4.0f && state.vf[3][0] == 12.0f,
          "scalar readiness or E-bit delay slot");
    check(state.cycles > 26, "FMAC flags were not drained after E-bit");
    return state;
}

VU1State indirectExit(bool regions, bool split)
{
    static constexpr uint16_t range[] = {0, 24};
    Fixture fixture;
    fixture.pair(0, lowerSpecial(0x38) | (1u << 11) | (2u << 16));
    fixture.pair(1, (0x24u << 25) | (1u << 11)); // JR VI1
    fixture.pair(2, lowerSpecial(0x6c)); // XGKICK VI0 in branch delay slot
    fixture.pair(8, (1u << 25) | (8u << 21) | (1u << 11) | 1u); // SQ.x VF1, 1(VI0)
    fixture.pair(9, lowerSpecial(0x3b)); // WAITQ after the region exit
    fixture.pair(10, lowerNop, upperNop | endBit);
    fixture.pair(11);
    fixture.registerRegion(range);
    vu1native::setEnabled(regions);
    fixture.vu.state().vi[1] = 8;
    fixture.vu.state().vf[1][0] = 8.0f;
    fixture.vu.state().vf[2][0] = 2.0f;
    const uint64_t tag = (1ull << 60) | 0x8001u; // PACKED, one register, one loop, EOP
    std::memcpy(fixture.memory.getVU1Data(), &tag, sizeof(tag));
    if (split)
    {
        fixture.slice(2);
        check(fixture.vu.state().pc == 16 && fixture.vu.state().branchPending,
              "budget failed to preserve branch delay state");
        fixture.slice(1);
        check(fixture.vu.state().pc == 64 && fixture.packets.empty(),
              "XGKICK completed before the packet payload was read");
    }
    fixture.slice(128);
    const auto &state = fixture.vu.state();
    check(state.pc == 96 && state.q == 4.0f && !state.branchPending, "indirect exit lost branch or pending Q state");
    check(fixture.dispatch.programs == static_cast<unsigned>(regions) && fixture.dispatch.completedPrograms == 0,
          "indirect exit reported as region completion");
    check(fixture.packets.size() == 1 && fixture.packets[0].size() == 32, "incorrect PATH1 packet count or length");
    float payload;
    std::memcpy(&payload, fixture.packets[0].data() + 16, sizeof(payload));
    check(payload == 8.0f, "PATH1 failed to observe store before its read cycle");
    return state;
}

void sameState(const VU1State &reference, const VU1State &actual)
{
    check(reference.cycles == actual.cycles && reference.pc == actual.pc &&
          reference.mac == actual.mac && reference.status == actual.status &&
          reference.q == actual.q && reference.p == actual.p &&
          std::memcmp(reference.vf, actual.vf, sizeof(reference.vf)) == 0 &&
          std::memcmp(reference.vi, actual.vi, sizeof(reference.vi)) == 0,
          "scheduled and reference state differ");
}
}

int main()
{
    const VU1State scalarReference = scalarStalls(false, false);
    sameState(scalarReference, scalarStalls(true, false));
    sameState(scalarReference, scalarStalls(true, true));
    const VU1State indirectReference = indirectExit(false, false);
    sameState(indirectReference, indirectExit(true, false));
    sameState(indirectReference, indirectExit(true, true));
    std::puts("VU1 scheduler tests passed");
}
