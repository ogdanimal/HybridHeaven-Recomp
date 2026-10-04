#ifndef __HH_GAME_H__
#define __HH_GAME_H__

#include <cstdint>
#include <span>
#include <vector>

namespace hybridheaven {
    // Turns the retail Hybrid Heaven (USA) ROM into the decompressed image the
    // recompiled code was generated against. Registered as the GameEntry's
    // `decompression_routine`, so librecomp calls it after validating the stored
    // ROM's hash -- which means what a user supplies, and what is kept on disk,
    // is their own retail dump.
    //
    // Returns an empty vector if the input is not the ROM this expects.
    std::vector<uint8_t> decompress_hh(std::span<const uint8_t> compressed_rom);
}

#endif
