#include "../../src/xenosaga_iso.h"

int main()
{
    // Synthetic PVD, root directory and one sector of payload.
    std::vector<uint8_t> image(20 * xs_iso::kSectorSize);
    auto pvd = image.data() + 16 * xs_iso::kSectorSize;
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    pvd[156 + 2] = 18;
    pvd[156 + 11] = 8; // 2048 bytes, little-endian
    auto record = image.data() + 18 * xs_iso::kSectorSize;
    const std::string name = "EXAMPLE.DAT;1";
    record[0] = 33 + name.size();
    record[2] = 19;
    record[10] = 1;
    record[32] = name.size();
    std::memcpy(record + 33, name.data(), name.size());
    image[19 * xs_iso::kSectorSize] = 42;
    const auto path = std::filesystem::path("synthetic-disc.img");
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char *>(image.data()), image.size());
    file.close();
    std::vector<uint8_t> payload;
    return !(file && xs_iso::readFile(path, "cdrom0:\\example.dat", payload)
        && payload == std::vector<uint8_t>{42}
        && !xs_iso::readFile(path, "missing.dat", payload));
}
