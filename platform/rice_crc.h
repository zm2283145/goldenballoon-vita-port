// rice_crc.h — the Rice/GLideN64 high-resolution texture key, in C.
//
// A Rice pack names each texture by a value computed over the RAW N64 texel
// bytes; this port names textures by a content digest of the decoded image.
// Neither is computable from the other without the game's own data, which is
// why importing a Rice pack has needed an offline crosswalk built from a
// texture dump. The runtime does not need one: at the moment it uploads a
// texture it is holding exactly the bytes this function hashes.
//
// Despite the name this is NOT a polynomial CRC. It is the ROL4 accumulator
// from Rice Video's CalculateRDRAMCRC (mupen64plus-video-rice,
// src/FrameBuffer.cpp), which is why four candidate CRC-32 readings all failed
// to match a single key. It walks BACKWARDS -- x from bytesPerLine-4 down by
// four, y from height-1 down to zero -- and the inner accumulator survives the
// x loop to be xored with y.
//
// The `swap32` reading is deliberate and measured, not a guess. Rice hashes
// RDRAM with a host-order 32-bit load, and an emulator holds RDRAM byte-swapped
// relative to the cartridge, so the DWORD Rice hashed is the byte-swapped one
// here. On 2026-09-08 this reading matched 648 of 798 dumped textures against a
// real ROM and a real pack while every other candidate matched at most one.
// tools/ricepack/rice_crc.py is the reference implementation and names this
// variant `rice-hires-swap32`; tests/test_rice_crc.c holds both to the same
// vectors.
#ifndef MDKR_RICE_CRC_H
#define MDKR_RICE_CRC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Writes the Rice key for one texture into `out_key` and returns 1.
//
// `texels` is the span as it sits in RDRAM, `length` its size in bytes, and
// `pitch` the distance between row starts (pass 0 for a tightly packed image).
// `siz` is the RDP size code, 0..3.
//
// Returns 0 without touching `out_key` for geometry this key is not defined
// over: an undefined size code, a non-positive dimension, a row shorter than
// the four bytes the accumulator reads at a time, a pitch narrower than a row,
// or a span too short for the walk. Returning a plausible number for a span
// that does not hold the texture is the worst outcome for a keying function --
// it names the wrong picture in silence.
int mdkr_rice_crc32(const uint8_t *texels, size_t length, int width,
                    int height, int siz, int pitch, uint32_t *out_key);

#ifdef __cplusplus
}
#endif

#endif  // MDKR_RICE_CRC_H
