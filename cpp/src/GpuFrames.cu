#include "GpuFrames.h"

#include <stdexcept>
#include <string>

namespace gpu {

void check(cudaError_t err, const char* what, const char* file, int line)
{
    if (err != cudaSuccess)
        throw std::runtime_error(std::string(what) + " failed: " + cudaGetErrorString(err) +
                                 " (" + file + ":" + std::to_string(line) + ")");
}

namespace {

// Reflects out-of-range coordinates about the border. Reflection preserves
// coordinate parity, so the Bayer colour phase stays correct at the edges.
__device__ __forceinline__ int reflect(int v, int n)
{
    return v < 0 ? -v : (v >= n ? 2 * (n - 1) - v : v);
}

__global__ void debayerGbrgKernel(const uint8_t* __restrict__ bayer, size_t pitch,
                                  uchar4* __restrict__ rgba, size_t rgbaPitch, int w, int h)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= w || y >= h)
        return;

    auto at = [&](int dx, int dy) -> float {
        return __ldg(&bayer[size_t(reflect(y + dy, h)) * pitch + reflect(x + dx, w)]);
    };

    const float c = at(0, 0);
    const float horiz = at(-1, 0) + at(1, 0);
    const float vert = at(0, -1) + at(0, 1);
    const float diag = at(-1, -1) + at(1, -1) + at(-1, 1) + at(1, 1);
    const float horiz2 = at(-2, 0) + at(2, 0);
    const float vert2 = at(0, -2) + at(0, 2);

    // Malvar-He-Cutler filters, each normalised by 1/8.
    const float fromHoriz = (5 * c + 4 * horiz - diag - horiz2 + 0.5f * vert2) * 0.125f;  // colour of horizontal neighbours, at a green site
    const float fromVert = (5 * c + 4 * vert - diag - vert2 + 0.5f * horiz2) * 0.125f;    // colour of vertical neighbours, at a green site
    const float greenAtRB = (4 * c + 2 * (horiz + vert) - horiz2 - vert2) * 0.125f;
    const float oppositeAtRB = (6 * c + 2 * diag - 1.5f * (horiz2 + vert2)) * 0.125f;    // B at R, or R at B

    // GBRG layout:  even rows: G B G B ...   odd rows: R G R G ...
    const bool evenRow = (y & 1) == 0;
    const bool evenCol = (x & 1) == 0;
    float r, g, b;
    if (evenRow && evenCol) {          // green on a blue row: blue left/right, red above/below
        g = c; b = fromHoriz; r = fromVert;
    } else if (!evenRow && !evenCol) { // green on a red row: red left/right, blue above/below
        g = c; r = fromHoriz; b = fromVert;
    } else if (evenRow) {              // blue site
        b = c; g = greenAtRB; r = oppositeAtRB;
    } else {                           // red site
        r = c; g = greenAtRB; b = oppositeAtRB;
    }

    auto to8 = [](float v) { return static_cast<unsigned char>(__float2int_rn(fminf(fmaxf(v, 0.f), 255.f))); };
    auto* row = reinterpret_cast<uchar4*>(reinterpret_cast<char*>(rgba) + size_t(y) * rgbaPitch);
    row[x] = make_uchar4(to8(r), to8(g), to8(b), 255);
}

// One thread per 2x2 block: four luma samples and one averaged chroma pair.
__global__ void rgbaToNv12Kernel(const uchar4* __restrict__ rgba, size_t rgbaPitch,
                                 uint8_t* __restrict__ yPlane, size_t yPitch,
                                 uint8_t* __restrict__ uvPlane, size_t uvPitch, int w, int h)
{
    const int bx = blockIdx.x * blockDim.x + threadIdx.x;
    const int by = blockIdx.y * blockDim.y + threadIdx.y;
    if (2 * bx >= w || 2 * by >= h)
        return;

    float sumR = 0, sumG = 0, sumB = 0;
    for (int dy = 0; dy < 2; ++dy) {
        const int y = 2 * by + dy;
        const auto* row = reinterpret_cast<const uchar4*>(reinterpret_cast<const char*>(rgba) + size_t(y) * rgbaPitch);
        for (int dx = 0; dx < 2; ++dx) {
            const uchar4 p = row[2 * bx + dx];
            sumR += p.x; sumG += p.y; sumB += p.z;
            const float luma = 0.2126f * p.x + 0.7152f * p.y + 0.0722f * p.z;
            yPlane[size_t(y) * yPitch + 2 * bx + dx] = static_cast<uint8_t>(__float2int_rn(16.f + luma * (219.f / 255.f)));
        }
    }
    const float r = sumR * 0.25f, g = sumG * 0.25f, b = sumB * 0.25f;
    const float cb = -0.1146f * r - 0.3854f * g + 0.5f * b;
    const float cr = 0.5f * r - 0.4542f * g - 0.0458f * b;
    uint8_t* uv = uvPlane + size_t(by) * uvPitch + 2 * bx;
    uv[0] = static_cast<uint8_t>(__float2int_rn(128.f + cb * (224.f / 255.f)));
    uv[1] = static_cast<uint8_t>(__float2int_rn(128.f + cr * (224.f / 255.f)));
}

}  // namespace

void rgbaToNv12Bt709(const uchar4* rgba, size_t rgbaPitch, uint8_t* y, size_t yPitch, uint8_t* uv, size_t uvPitch,
                     int width, int height, cudaStream_t stream)
{
    const dim3 block(32, 8);
    const dim3 grid((width / 2 + block.x - 1) / block.x, (height / 2 + block.y - 1) / block.y);
    rgbaToNv12Kernel<<<grid, block, 0, stream>>>(rgba, rgbaPitch, y, yPitch, uv, uvPitch, width, height);
    CUDA_CHECK(cudaGetLastError());
}

void debayerGbrgToRgba(const uint8_t* bayer, size_t bayerPitch, uchar4* rgba, size_t rgbaPitch,
                       int width, int height, cudaStream_t stream)
{
    const dim3 block(32, 8);
    const dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);
    debayerGbrgKernel<<<grid, block, 0, stream>>>(bayer, bayerPitch, rgba, rgbaPitch, width, height);
    CUDA_CHECK(cudaGetLastError());
}

}  // namespace gpu
