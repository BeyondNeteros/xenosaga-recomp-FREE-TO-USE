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

    // Find a file or directory by path ("\\XENOSAGA.00;1", "cdrom0:\\IOP\\SSD.IRX;1").
    inline bool lookup(const std::filesystem::path &image, const std::string &path, Entry &result)
    {
        std::ifstream in(image, std::ios::binary);
        if (!in)
            return false;

        std::vector<uint8_t> pvd;
        if (!readSectors(in, 16, kSectorSize, pvd) || pvd[0] != 1 || std::memcmp(&pvd[1], "CD001", 5) != 0)
            return false;

        const uint8_t *root = &pvd[156];
        Entry current{le32(root + 2), le32(root + 10), true};

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
                    current = Entry{le32(&dir[off + 2]), le32(&dir[off + 10]), (dir[off + 25] & 0x02) != 0};
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
