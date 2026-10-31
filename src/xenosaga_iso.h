// Minimal ISO 9660 reader for looking up files in a PS2 disc image.
//
// Xenosaga reads its data archives by absolute sector number, so file lookups
// must return each file's real position on the disc, not a synthetic one.

#pragma once

#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace xs_iso
{
    constexpr uint32_t kSectorSize = 2048;

    struct Entry
    {
        uint32_t lsn = 0;
        uint32_t size = 0;
        bool directory = false;
    };

    inline uint32_t le32(const uint8_t *p)
    {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }

    inline bool readSectors(std::ifstream &in, uint32_t lsn, uint32_t bytes, std::vector<uint8_t> &out)
    {
        out.resize(bytes);
        in.clear();
        in.seekg(static_cast<std::streamoff>(lsn) * kSectorSize);
        in.read(reinterpret_cast<char *>(out.data()), bytes);
        return static_cast<uint32_t>(in.gcount()) == bytes;
    }

    // Upper-case and drop the ";1" version suffix, for case-insensitive matching.
    inline std::string normalizeName(std::string name)
    {
        if (const auto semi = name.find(';'); semi != std::string::npos)
            name.erase(semi);
        if (!name.empty() && name.back() == '.')
            name.pop_back();
        for (auto &c : name)
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return name;
    }

    inline std::vector<std::string> splitPath(const std::string &path)
    {
        std::string p = path;
        if (const auto colon = p.find(':'); colon != std::string::npos)
            p.erase(0, colon + 1); // drop a device prefix like "cdrom0:"
        std::vector<std::string> parts;
        std::string cur;
        for (char c : p)
        {
            if (c == '\\' || c == '/')
            {
                if (!cur.empty())
                    parts.push_back(normalizeName(cur));
                cur.clear();
            }
            else
                cur.push_back(c);
        }
        if (!cur.empty())
            parts.push_back(normalizeName(cur));
        return parts;
    }

    inline bool isPvd(const std::vector<uint8_t> &s)
    {
        return s.size() >= kSectorSize && s[0] == 1 && std::memcmp(&s[1], "CD001", 5) == 0;
    }

    // Dual-layer (DVD9) PS2 images hold layer 1 as a second volume after layer 0.
    // Returns the sector where layer 1 starts, or 0 for a single-layer image.
    // Layer 0's volume size points at it; PCSX2 rounds that up to 16 sectors.
    inline uint32_t findLayer1Start(const std::filesystem::path &image)
    {
        std::ifstream in(image, std::ios::binary);
        std::vector<uint8_t> pvd;
        if (!in || !readSectors(in, 16, kSectorSize, pvd) || !isPvd(pvd))
            return 0;
        const uint32_t layer0Sectors = le32(&pvd[80]);
        const uint32_t rounded = (layer0Sectors + 15u) & ~15u;
        for (uint32_t base : {layer0Sectors, rounded, layer0Sectors - 1u, rounded + 16u})
        {
            std::vector<uint8_t> s;
            if (base > 16 && readSectors(in, base + 16, kSectorSize, s) && isPvd(s))
                return base;
        }
        return 0;
    }

    // Diagnostics: print the volume size, image size, and every volume descriptor
    // near the end of layer 0, to pin down how a dual-layer image is laid out.
    template <typename Stream>
    void logLayout(const std::filesystem::path &image, Stream &log)
    {
        std::ifstream in(image, std::ios::binary);
        std::vector<uint8_t> pvd;
        if (!in || !readSectors(in, 16, kSectorSize, pvd) || !isPvd(pvd))
            return;
        std::error_code ec;
        const uint64_t total = std::filesystem::file_size(image, ec) / kSectorSize;
        const uint32_t vol = le32(&pvd[80]);
        log << "[xenosaga] iso layout: volume sectors=" << vol << " image sectors=" << total << std::endl;
        for (int64_t lsn = int64_t(vol) - 32; lsn <= int64_t(vol) + 64; ++lsn)
        {
            std::vector<uint8_t> s;
            if (lsn > 16 && readSectors(in, uint32_t(lsn), kSectorSize, s) && std::memcmp(&s[1], "CD001", 5) == 0)
                log << "[xenosaga] iso layout: descriptor type " << int(s[0]) << " at sector " << lsn
                    << (s[0] == 1 ? " volume sectors=" + std::to_string(le32(&s[80])) : std::string()) << std::endl;
        }
    }

    // Find a file or directory by path ("\\XENOSAGA.00;1", "cdrom0:\\IOP\\SSD.IRX;1").
    // layerStart selects the volume: 0 for layer 0, findLayer1Start() for layer 1.
    // Returned LSNs are absolute positions in the image.
    inline bool lookup(const std::filesystem::path &image, const std::string &path, Entry &result,
                       uint32_t layerStart = 0)
    {
        std::ifstream in(image, std::ios::binary);
        if (!in)
            return false;

        std::vector<uint8_t> pvd;
        if (!readSectors(in, layerStart + 16, kSectorSize, pvd) || !isPvd(pvd))
            return false;

        const uint8_t *root = &pvd[156];
        Entry current{layerStart + le32(root + 2), le32(root + 10), true};

        for (const std::string &want : splitPath(path))
        {
            if (!current.directory)
                return false;
            std::vector<uint8_t> dir;
            if (!readSectors(in, current.lsn, current.size, dir))
                return false;

            bool found = false;
            for (uint32_t off = 0; off < dir.size();)
            {
                const uint8_t len = dir[off];
                if (len == 0)
                {
                    off = (off / kSectorSize + 1) * kSectorSize; // records never cross sectors
                    continue;
                }
                if (off + 33 > dir.size() || off + len > dir.size())
                    break;
                const uint8_t nameLen = dir[off + 32];
                const std::string name(reinterpret_cast<const char *>(&dir[off + 33]), nameLen);
                if (nameLen > 0 && !(nameLen == 1 && (name[0] == 0 || name[0] == 1)) && normalizeName(name) == want)
                {
                    current = Entry{layerStart + le32(&dir[off + 2]), le32(&dir[off + 10]), (dir[off + 25] & 0x02) != 0};
                    found = true;
                    break;
                }
                off += len;
            }
            if (!found)
                return false;
        }
        result = current;
        return true;
    }

    // List the files (not subdirectories) in a directory of layer 0, e.g. "\\IOP".
    inline std::vector<std::string> listFiles(const std::filesystem::path &image, const std::string &dirPath)
    {
        std::vector<std::string> names;
        Entry d;
        if (!lookup(image, dirPath, d) || !d.directory)
            return names;
        std::ifstream in(image, std::ios::binary);
        std::vector<uint8_t> dir;
        if (!in || !readSectors(in, d.lsn, d.size, dir))
            return names;
        for (uint32_t off = 0; off < dir.size();)
        {
            const uint8_t len = dir[off];
            if (len == 0)
            {
                off = (off / kSectorSize + 1) * kSectorSize;
                continue;
            }
            if (off + 33 > dir.size() || off + len > dir.size())
                break;
            const uint8_t nameLen = dir[off + 32];
            const bool isDir = (dir[off + 25] & 0x02) != 0;
            std::string name(reinterpret_cast<const char *>(&dir[off + 33]), nameLen);
            if (!isDir && nameLen > 1)
                names.push_back(normalizeName(name));
            off += len;
        }
        return names;
    }

    // Read a whole file from the image.
    inline bool readFile(const std::filesystem::path &image, const std::string &path, std::vector<uint8_t> &out)
    {
        Entry e;
        if (!lookup(image, path, e) || e.directory)
            return false;
        std::ifstream in(image, std::ios::binary);
        return in && readSectors(in, e.lsn, e.size, out);
    }
}
