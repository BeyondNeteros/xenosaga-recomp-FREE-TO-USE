// Xenosaga Episode I (SLUS_204.69): disc image support.
//
// The game locates XENOSAGA.00 and friends with sceCdSearchFile, then reads
// game data by absolute sector, spanning from one archive file into the next.
// That only works if lookups return each file's real position on the disc,
// so when a disc image is available, file lookups are answered from the
// image's own ISO 9660 directory. Raw sector reads already go to the image.

#include "ps2_runtime.h"
#include "ps2_stubs.h"
#include "xenosaga_iso.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
    constexpr uint32_t kRdramMask = 0x01FFFFFFu;
    uint32_t g_layer1Start = 0; // 0 = single-layer image
    bool g_layerCall = false;   // set by the sceCdLayerSearchFile entry point

    std::string readGuestString(const uint8_t *rdram, uint32_t addr, size_t maxLen)
    {
        std::string s;
        for (size_t i = 0; i < maxLen; ++i)
        {
            const char c = static_cast<char>(rdram[(addr + i) & kRdramMask]);
            if (c == '\0')
                break;
            s.push_back(c);
        }
        return s;
    }
}

// Find the disc image: XS_DISC_IMAGE, or the first .iso next to the boot ELF.
std::filesystem::path xs_findDiscImage()
{
    if (const char *env = std::getenv("XS_DISC_IMAGE"); env && env[0])
        return env;
    const auto &paths = PS2Runtime::getIoPaths();
    std::error_code ec;
    for (const auto &dir : {paths.elfDirectory, paths.cdRoot})
    {
        for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
        {
            std::string ext = entry.path().extension().string();
            for (auto &c : ext)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".iso")
                return entry.path();
        }
    }
    return {};
}

void xs_configureDiscImage()
{
    const std::filesystem::path image = xs_findDiscImage();
    if (image.empty())
    {
        std::cout << "[xenosaga] no disc image found; using loose files (data reads may fail)" << std::endl;
        return;
    }
    xs_iso::Entry e;
    if (!xs_iso::lookup(image, "\\SYSTEM.CNF;1", e))
    {
        std::cerr << "[xenosaga] " << image << " is not a readable PS2 disc image" << std::endl;
        return;
    }
    PS2Runtime::IoPaths paths = PS2Runtime::getIoPaths();
    paths.cdImage = image;
    PS2Runtime::setIoPaths(paths);
    g_layer1Start = xs_iso::findLayer1Start(image);
    xs_iso::logLayout(image, std::cout);

    // The runtime loads IOP modules (and reads SYSTEM.CNF) as loose files from
    // the CD root. Extract them from the user's own disc once, next to the ELF.
    const std::filesystem::path cdRoot = paths.cdRoot;
    std::error_code ec;
    std::filesystem::create_directories(cdRoot / "IOP", ec);
    auto extract = [&](const std::string &discPath, const std::filesystem::path &dest) {
        if (std::filesystem::exists(dest, ec))
            return;
        std::vector<uint8_t> data;
        if (!xs_iso::readFile(image, discPath, data))
            return;
        std::ofstream out(dest, std::ios::binary);
        out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
        std::cout << "[xenosaga] extracted " << discPath << std::endl;
    };
    extract("\\SYSTEM.CNF;1", cdRoot / "SYSTEM.CNF");
    for (const std::string &name : xs_iso::listFiles(image, "\\IOP"))
        extract("\\IOP\\" + name + ";1", cdRoot / "IOP" / name);
    std::cout << "[xenosaga] disc image: " << image;
    if (g_layer1Start)
        std::cout << " (dual layer, layer 1 at sector " << g_layer1Start << ")";
    std::cout << std::endl;
}

bool xs_readDiscFile(const std::string &path, std::vector<uint8_t> &out)
{
    const std::filesystem::path image = PS2Runtime::getIoPaths().cdImage;
    return !image.empty() && xs_iso::readFile(image, path, out);
}

// int sceCdSearchFile(sceCdlFILE *fp, const char *name)
// int sceCdLayerSearchFile(sceCdlFILE *fp, const char *name, int layer)
// sceCdlFILE: u32 lsn; u32 size; char name[16]; u8 date[8];
void xs_sceCdSearchFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const std::filesystem::path image = PS2Runtime::getIoPaths().cdImage;
    if (image.empty())
    {
        ps2_stubs::sceCdSearchFile(rdram, ctx, runtime);
        return;
    }

    const uint32_t fileAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
    const uint32_t nameAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
    const std::string name = readGuestString(rdram, nameAddr, 260);
    // sceCdLayerSearchFile passes the layer in $a2; plain sceCdSearchFile leaves
    // $a2 unrelated, so the layer is only read for the layer entry point.
    const uint32_t layer = g_layerCall ? static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[6])) : 0u;
    g_layerCall = false;
    const uint32_t layerStart = (layer == 1 && g_layer1Start) ? g_layer1Start : 0u;

    xs_iso::Entry e;
    if (!xs_iso::lookup(image, name, e, layerStart) || e.directory)
    {
        std::cout << "[xenosaga] disc file not found: " << name << std::endl;
        setReturnS32(ctx, 0);
        return;
    }

    uint8_t *fp = rdram + (fileAddr & kRdramMask);
    std::memset(fp, 0, 32);
    std::memcpy(fp + 0, &e.lsn, 4);
    std::memcpy(fp + 4, &e.size, 4);
    std::string leaf = name.substr(name.find_last_of("\\/:") + 1);
    std::strncpy(reinterpret_cast<char *>(fp + 8), leaf.c_str(), 15);
    std::cout << "[xenosaga] disc lookup " << name << " layer=" << layer << " lsn=" << e.lsn << " size=" << e.size << std::endl;
    setReturnS32(ctx, 1);
}

void xs_sceCdLayerSearchFile(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    g_layerCall = true;
    xs_sceCdSearchFile(rdram, ctx, runtime);
}

// int sceCdReadDvdDualInfo(int *on_dual, u_int *layer1_start)
void xs_sceCdReadDvdDualInfo(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
{
    const uint32_t onDualAddr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[4]));
    const uint32_t layer1Addr = static_cast<uint32_t>(_mm_cvtsi128_si32(ctx->r[5]));
    const uint32_t onDual = g_layer1Start ? 1u : 0u;
    if (onDualAddr)
        std::memcpy(rdram + (onDualAddr & kRdramMask), &onDual, 4);
    if (layer1Addr)
        std::memcpy(rdram + (layer1Addr & kRdramMask), &g_layer1Start, 4);
    setReturnS32(ctx, 1);
}
