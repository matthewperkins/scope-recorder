#pragma once

#include <cstddef>
#include <cstdint>

#include <cuda_runtime.h>

// Checks a CUDA runtime call; throws std::runtime_error with the call text on failure.
#define CUDA_CHECK(call) ::gpu::check((call), #call, __FILE__, __LINE__)

namespace gpu {

void check(cudaError_t err, const char* what, const char* file, int line);

// Demosaics an 8-bit GBRG Bayer image (as delivered by the DFK 33UX174) into RGBA8,
// using the Malvar-He-Cutler 5x5 linear filters. White balance and gamma are already
// applied by the camera, so this is the only colour processing needed.
void debayerGbrgToRgba(const uint8_t* bayer, size_t bayerPitch,
                       uchar4* rgba, size_t rgbaPitch,
                       int width, int height, cudaStream_t stream);

// Converts RGBA8 to NV12 (Y plane + interleaved half-resolution UV plane), the encoder's native
// input format, using BT.709 coefficients and video range (Y 16-235, UV 16-240).
// Width and height must be even.
void rgbaToNv12Bt709(const uchar4* rgba, size_t rgbaPitch,
                     uint8_t* y, size_t yPitch, uint8_t* uv, size_t uvPitch,
                     int width, int height, cudaStream_t stream);

}  // namespace gpu
