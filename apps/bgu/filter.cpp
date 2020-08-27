#include <cstdio>
#include <random>

#include "bgu.h"
#ifndef NO_AUTO_SCHEDULE
    #include "bgu_auto_schedule.h"
#endif
#ifndef NO_GRADIENT_AUTO_SCHEDULE
    #include "bgu_gradient_auto_schedule.h"
#endif

#include "benchmark_util.h"
#include "halide_benchmark.h"
#include "halide_image_io.h"

#include "HalideBuffer.h"

using namespace Halide::Tools;

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("Usage: %s in out\n", argv[0]);
        return 1;
    }

    const float r_sigma = 1.f / 8.f;
    const int s_sigma = 16;

    // Bilateral-guided upsampling (BGU) is used for cheaply
    // transferring some effect applied to a low-res image to a
    // high-res input. We'll make a low-res version of the input,
    // sharpen and contrast-enhance it naively, and then call into the
    // Halide BGU implementation to apply the same effect to the
    // original high-res input. We're only interested in benchmarking
    // the last part.

    // BGU will be good at capturing the contrast enhancement and
    // vignette, and bad at capturing the high-frequency sharpening.

    Halide::Runtime::Buffer<float> high_res_in = load_and_convert_image(argv[1]);
    const int W = high_res_in.width();
    const int H = high_res_in.height();
    const int C = high_res_in.channels();

    Halide::Runtime::Buffer<float> high_res_out(W, H, C);
    Halide::Runtime::Buffer<float> low_res_in(W / 8, H / 8, C);
    Halide::Runtime::Buffer<float> low_res_out(W / 8, H / 8, C);

    // Downsample the input with a box filter
    low_res_in.fill(0.0f);
    low_res_in.for_each_element([&](int x, int y, int c) {
        float sum = 0.0f;
        for (int sy = y * 8; sy < y * 8 + 8; sy++) {
            for (int sx = x * 8; sx < x * 8 + 8; sx++) {
                sum += high_res_in(sx, sy, c);
            }
        }
        low_res_in(x, y, c) = sum / 64;
    });

    // Some straw-man black-box image processing algorithm we're
    // trying to accelerate.
    low_res_in.for_each_element([&](int x, int y, int c) {
        float val;
        // Sharpen, ignoring edges
        if (x == 0 || x == W - 1 || y == 0 || y == H - 1) {
            val = low_res_in(x, y, c);
        } else {
            val =
                (2 * low_res_in(x, y, c) -
                 (low_res_in(x - 1, y, c) +
                  low_res_in(x + 1, y, c) +
                  low_res_in(x, y - 1, c) +
                  low_res_in(x, y + 1, c)) /
                     4);
        }
        // Smoothstep for contrast enhancement
        float boosted = val * val * (3 - 2 * val);

        // We'll only boost the center of the image. The center of
        // the low-res image is at W/16, H/16
        float r = std::min(W / 16, H / 16);
        float mask_x = (x - W / 16) / r;
        float mask_y = (y - H / 16) / r;
        float mask = std::sqrt(mask_x * mask_x + mask_y * mask_y);
        val = val * mask + boosted * (1 - mask);

        // Also apply a vignette.
        val *= (2 - mask) / 2;

        // Clamp to range
        val = std::max(std::min(val, 1.0f), 0.0f);
        low_res_out(x, y, c) = val;
    });

    Halide::Runtime::Buffer<float> low_res_in_saved = load_and_convert_image("../images/low_res_in.png");
    bool success = true;
    float epsilon = 0.0001;
    low_res_in.for_each_element([&](int x, int y, int c) {
        float saved = low_res_in_saved(x, y, c);
        float computed = low_res_in(x, y, c);
        if (std::abs(saved - computed) > epsilon) {
            printf("%d %d %d: %f vs %f\n", x, y, c, saved, computed);
            success = false;
        }
    });

    Halide::Runtime::Buffer<float> low_res_out_saved = load_and_convert_image("../images/low_res_out.png");
    low_res_out.for_each_element([&](int x, int y, int c) {
        float saved = low_res_out_saved(x, y, c);
        float computed = low_res_out(x, y, c);
        if (std::abs(saved - computed) > epsilon) {
            printf("%d %d %d: %f vs %f\n", x, y, c, saved, computed);
            success = false;
        }
    });

    if (!success) {
        return -1;
    }

    // To view the low res output for debugging the algorithm above
    // convert_and_save_image(low_res_out, "test.png");

    multi_way_bench({
        {"bgu Manual", [&]() { bgu(r_sigma, s_sigma, low_res_in, low_res_out, high_res_in, high_res_out); high_res_out.device_sync(); }},
#ifndef NO_AUTO_SCHEDULE
        {"bgu Auto-scheduled", [&]() { bgu_auto_schedule(r_sigma, s_sigma, low_res_in, low_res_out, high_res_in, high_res_out); high_res_out.device_sync(); }},
#endif
#ifndef NO_GRADIENT_AUTO_SCHEDULE
        {"bgu Gradient auto-scheduled", [&]() { bgu_gradient_auto_schedule(r_sigma, s_sigma, low_res_in, low_res_out, high_res_in, high_res_out); high_res_out.device_sync(); }}
#endif
    });

    high_res_out.copy_to_host();

    convert_and_save_image(high_res_out, argv[2]);

    printf("Success!\n");
    return 0;
}
