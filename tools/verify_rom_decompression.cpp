// Standalone check: retail ROM -> decompress_hh() must equal the known-good image.
//
// The port's decompression has one hard requirement: its output must be
// BYTE-IDENTICAL to what the decomp's tools/rommy.py produces, because that is
// the image the recompiler consumed and every address in RecompiledFuncs/ is an
// address in it. A decompressor that came out merely almost right would boot and
// then fail somewhere with no apparent connection to the ROM, so this compares
// all 32 MiB rather than a hash of it and reports the first differing offset.
//
// Build and run -- needs no other part of the project:
//   c++ -std=c++20 -O2 -I include -o /tmp/verify_rom_decompression \
//       tools/verify_rom_decompression.cpp src/game/rom_decompression.cpp
//   /tmp/verify_rom_decompression "Hybrid Heaven (USA).z64" hybridheaven.z64
#include <cstdio>
#include <fstream>
#include <vector>
#include "hh_game.h"

static std::vector<uint8_t> read_file(const char* p) {
    std::ifstream f{p, std::ios::binary};
    if (!f.good()) { fprintf(stderr, "cannot open %s\n", p); return {}; }
    f.seekg(0, std::ios::end); std::vector<uint8_t> v(f.tellg()); f.seekg(0);
    f.read(reinterpret_cast<char*>(v.data()), v.size()); return v;
}

int main(int argc, char** argv) {
    if (argc != 3) { fprintf(stderr, "usage: %s <retail.z64> <expected-decompressed.z64>\n", argv[0]); return 2; }
    auto retail = read_file(argv[1]);
    auto expected = read_file(argv[2]);
    if (retail.empty() || expected.empty()) return 2;
    printf("retail %zu bytes, expected %zu bytes\n", retail.size(), expected.size());

    auto got = hybridheaven::decompress_hh(retail);
    printf("produced %zu bytes\n", got.size());
    if (got.size() != expected.size()) { printf("FAIL: size mismatch\n"); return 1; }

    size_t diffs = 0; size_t first = 0;
    for (size_t i = 0; i < got.size(); i++) {
        if (got[i] != expected[i]) { if (!diffs) first = i; diffs++; }
    }
    if (diffs) {
        printf("FAIL: %zu bytes differ, first at %#zx (got %02X want %02X)\n",
               diffs, first, got[first], expected[first]);
        return 1;
    }
    printf("PASS: byte-identical\n");
    return 0;
}
