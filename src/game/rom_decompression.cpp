/**
 * Retail ROM -> the decompressed image the recompiler was built against.
 *
 * Hybrid Heaven stores its files in a Nisitenma-Ichigo table, the format Konami
 * used across its N64 titles, with the individual files compressed using LZKN64.
 * The recompiled code in RecompiledFuncs/ was generated from the DECOMPRESSED
 * 32 MiB image, so every address it knows is an address in that image -- but the
 * ROM a player owns is the 16 MiB retail cartridge dump. This file bridges the
 * two, so the port can accept the ROM someone actually has.
 *
 * librecomp calls this through GameEntry::decompression_routine, after it has
 * validated the stored ROM against GameEntry::rom_hash. So the hash that gates
 * entry is the RETAIL one, the file kept on disk is the player's own dump, and
 * this runs on the way to memory. That ordering is what makes the Android
 * launcher's "pick your ROM" flow work at all.
 *
 * Derived from Goemon64Recomp's src/game/rom_decompression.cpp. The LZKN64
 * decoder and the file-table walk are that game's verbatim -- both games use the
 * same container and the same compression -- and only the constants at the
 * bottom are Hybrid Heaven's.
 *
 * The output is required to be BYTE-IDENTICAL to what the decomp's tools/rommy.py
 * produces, since that is the file the recompiler consumed. tools/verify_rom_
 * decompression.cpp checks exactly that against a known-good image.
 */

#include <cassert>
#include <cstring>

#include "hh_game.h"

#ifdef _MSC_VER
inline uint32_t byteswap(uint32_t val) {
    return _byteswap_ulong(val);
}
#else
constexpr uint32_t byteswap(uint32_t val) {
    return __builtin_bswap32(val);
}
#endif

// LZKN64 command encoding. The top bits of each command byte select the
// operation and the low bits carry its length; every length is biased by 2,
// because 2 is the shortest run worth encoding.
constexpr uint8_t COMMAND_SLIDING_WINDOW_COPY_END = 0x7F;
constexpr uint8_t COMMAND_SLIDING_WINDOW_COPY_LENGTH_MASK = 0x7C;
constexpr uint8_t COMMAND_SLIDING_WINDOW_COPY_OFFSET_FIRST_BYTE_MASK = 0x03;
constexpr uint16_t COMMAND_SLIDING_WINDOW_COPY_OFFSET_MAX_MASK = 0x3FF;

constexpr uint8_t COMMAND_RAW_COPY_END = 0x9F;
constexpr uint8_t COMMAND_RAW_COPY_LENGTH_MASK = 0x1F;

constexpr uint8_t COMMAND_RLE_WRITE_SHORT_ANY_VALUE_END = 0xDF;
constexpr uint8_t COMMAND_RLE_WRITE_SHORT_ANY_VALUE_LENGTH_MASK = 0x1F;

constexpr uint8_t COMMAND_RLE_WRITE_SHORT_ZERO_END = 0xFE;
constexpr uint8_t COMMAND_RLE_WRITE_SHORT_ZERO_LENGTH_MASK = 0x1F;

constexpr uint8_t COMMAND_RLE_WRITE_LONG_ZERO = 0xFF;
constexpr uint8_t COMMAND_RLE_WRITE_LONG_ZERO_LENGTH_MASK = 0xFF;

static size_t lzkn64_decompress(std::span<const uint8_t> input, std::span<uint8_t> output) {
    size_t input_pos = 4;
    size_t output_pos = 0;

    uint32_t compressed_size = byteswap(*reinterpret_cast<const uint32_t*>(input.data()));
    if (compressed_size > input.size()) {
        return 0;
    }

    while (input_pos < compressed_size) {
        uint8_t command = input[input_pos++];

        if (command <= COMMAND_SLIDING_WINDOW_COPY_END) {
            uint8_t length = (command & COMMAND_SLIDING_WINDOW_COPY_LENGTH_MASK) >> 2;
            uint16_t offset_first_byte = (command & COMMAND_SLIDING_WINDOW_COPY_OFFSET_FIRST_BYTE_MASK) << 8;
            uint8_t offset_second_byte = input[input_pos++];
            uint16_t offset = (offset_first_byte | offset_second_byte) & COMMAND_SLIDING_WINDOW_COPY_OFFSET_MAX_MASK;

            // Add 2 to get the actual length since 2 is the minimum length.
            length += 2;

            for (size_t i = 0; i < length; i++) {
                output[output_pos] = output[output_pos - offset];
                output_pos++;
            }
        } else if (command <= COMMAND_RAW_COPY_END) {
            uint8_t length = command & COMMAND_RAW_COPY_LENGTH_MASK;

            for (size_t i = 0; i < length; i++) {
                output[output_pos++] = input[input_pos++];
            }
        } else if (command <= COMMAND_RLE_WRITE_SHORT_ANY_VALUE_END) {
            uint8_t length = command & COMMAND_RLE_WRITE_SHORT_ANY_VALUE_LENGTH_MASK;
            uint8_t value = input[input_pos++];

            // Add 2 to get the actual length since 2 is the minimum length.
            length += 2;

            for (size_t i = 0; i < length; i++) {
                output[output_pos++] = value;
            }
        } else if (command <= COMMAND_RLE_WRITE_SHORT_ZERO_END) {
            uint8_t length = command & COMMAND_RLE_WRITE_SHORT_ZERO_LENGTH_MASK;

            // Add 2 to get the actual length since 2 is the minimum length.
            length += 2;

            for (size_t i = 0; i < length; i++) {
                output[output_pos++] = 0;
            }
        } else if (command == COMMAND_RLE_WRITE_LONG_ZERO) {
            uint16_t length = input[input_pos++] & COMMAND_RLE_WRITE_LONG_ZERO_LENGTH_MASK;

            // Add 2 to get the actual length since 2 is the minimum length.
            length += 2;

            for (size_t i = 0; i < length; i++) {
                output[output_pos++] = 0;
            }
        } else {
            // Invalid command.
        }
    }

    // Return the output position as the output size.
    return output_pos;
}

// Walks the Nisitenma-Ichigo table, expanding each file into the output ROM and
// rewriting the table to match where things landed.
//
// Entries are 32-bit big-endian: bit 31 is the "compressed" flag and bits 0-30
// are the file's ROM offset. Files are stored back to back, so entry N+1's
// offset is what ends file N -- which is why the loop needs both entries and why
// the table is written a pair at a time. Each output file is padded up to a
// 16-byte boundary; that alignment is part of the format, not a choice, and the
// decompressed image would not match the recompiler's addresses without it.
static size_t lzkn64_decompress_rom(std::span<const uint8_t> input_rom, std::span<uint8_t> output_rom, size_t file_table_offset) {
    const uint32_t* input_file_table_entry = reinterpret_cast<const uint32_t*>(input_rom.data() + file_table_offset);
    uint32_t* output_file_table_entry = reinterpret_cast<uint32_t*>(output_rom.data() + file_table_offset);
    uint32_t rom_offset = byteswap(input_file_table_entry[0]) & 0x7FFFFFFF;

    while (input_file_table_entry[0] != 0 && input_file_table_entry[1] != 0) {
        uint8_t file_is_compressed = (byteswap(input_file_table_entry[0]) & 0x80000000) >> 31;
        uint32_t file_offset = byteswap(input_file_table_entry[0]) & 0x7FFFFFFF;
        uint32_t file_size = (byteswap(input_file_table_entry[1]) & 0x7FFFFFFF) - file_offset;

        if (file_is_compressed) {
            std::span input_span = input_rom.subspan(file_offset, file_size);
            std::span output_span = output_rom.subspan(rom_offset);

            file_size = lzkn64_decompress(input_span, output_span);
        } else {
            memcpy(output_rom.data() + rom_offset, input_rom.data() + file_offset, file_size);
        }

        // Update the table entries for the current and the next file.
        output_file_table_entry[0] = byteswap(rom_offset);
        output_file_table_entry[1] = byteswap(rom_offset + ((file_size + 0xF) & ~0xF));

        rom_offset += (file_size + 0xF) & ~0xF;

        input_file_table_entry++;
        output_file_table_entry++;
    }

    return rom_offset;
}

constexpr size_t MAXIMUM_ROM_SIZE = 0x4000000;
constexpr size_t RETAIL_ROM_SIZE = 0x1000000;

// Where the Nisitenma-Ichigo table starts in Hybrid Heaven (USA): the 16-byte
// ASCII signature sits at 0x39BE0 and the entries follow it immediately. The
// offset is the same in the decompressed image, because everything below the
// first file's offset is copied across untouched.
constexpr size_t FILE_TABLE_OFFSET = 0x39BF0;

// The header CRCs the decompressed image carries. The N64 IPL checks these at
// boot and they no longer match once the files have been expanded, so they are
// rewritten to the values tools/rommy.py computes -- which are the values the
// recompiled build has always run against.
constexpr uint32_t DECOMPRESSED_ROM_CRC_1 = 0xB4909F2E;
constexpr uint32_t DECOMPRESSED_ROM_CRC_2 = 0xAE471157;

std::vector<uint8_t> hybridheaven::decompress_hh(std::span<const uint8_t> compressed_rom) {
    // Sanity checks on size and header. librecomp has already matched the ROM's
    // hash by the time this runs, so these should never fire -- they are here so
    // that this file cannot be quietly reused for a different game, which is the
    // mistake the comment in Goemon64Recomp's copy warns about.
    if (compressed_rom.size() != RETAIL_ROM_SIZE) {
        assert(false);
        return {};
    }

    // The cartridge game code, "NHVE": N64, Hybrid Heaven, USA.
    if (compressed_rom[0x3B] != 'N' || compressed_rom[0x3C] != 'H' || compressed_rom[0x3D] != 'V' || compressed_rom[0x3E] != 'E') {
        assert(false);
        return {};
    }

    std::vector<uint8_t> ret{};
    ret.resize(MAXIMUM_ROM_SIZE);
    memcpy(ret.data(), compressed_rom.data(), compressed_rom.size());

    size_t final_size = lzkn64_decompress_rom(compressed_rom, ret, FILE_TABLE_OFFSET);

    // Round the final size up to the nearest power of two, which is what gives
    // the 32 MiB image the recompiler was built from.
    final_size--;
    final_size |= final_size >> 1;
    final_size |= final_size >> 2;
    final_size |= final_size >> 4;
    final_size |= final_size >> 8;
    final_size |= final_size >> 16;
    final_size++;

    ret.resize(final_size);

    // Write the CRC values to the header of the decompressed ROM.
    *reinterpret_cast<uint32_t*>(ret.data() + 0x10) = byteswap(DECOMPRESSED_ROM_CRC_1);
    *reinterpret_cast<uint32_t*>(ret.data() + 0x14) = byteswap(DECOMPRESSED_ROM_CRC_2);

    return ret;
}
