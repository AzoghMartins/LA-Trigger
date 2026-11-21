#pragma once
#include <cmath>
#include <complex>
#include <vector>

namespace latrigger
{
/** Small iterative radix-2 FFT. Allocates only in prepare(). */
class FFT
{
public:
    void prepare (int size)
    {
        n = size;
        log2n = 0;
        while ((1 << log2n) < n)
            ++log2n;

        twiddles.resize ((size_t) n / 2);
        for (int i = 0; i < n / 2; ++i)
            twiddles[(size_t) i] = std::polar (1.0f, (float) (-2.0 * M_PI * i / n));

        bitrev.resize ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < log2n; ++b)
                if (i & (1 << b))
                    r |= 1 << (log2n - 1 - b);
            bitrev[(size_t) i] = r;
        }
        work.resize ((size_t) n);
    }

    int size() const noexcept { return n; }

    /** Computes the power spectrum (|X|^2) of a real signal. power must hold n/2 + 1 values. */
    void powerSpectrum (const float* input, float* power) noexcept
    {
        for (int i = 0; i < n; ++i)
            work[(size_t) bitrev[(size_t) i]] = { input[i], 0.0f };

        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len >> 1;
            const int step = n / len;
            for (int i = 0; i < n; i += len)
                for (int j = 0; j < half; ++j)
                {
                    const auto t = twiddles[(size_t) (j * step)] * work[(size_t) (i + j + half)];
                    const auto u = work[(size_t) (i + j)];
                    work[(size_t) (i + j)] = u + t;
                    work[(size_t) (i + j + half)] = u - t;
                }
        }

        for (int i = 0; i <= n / 2; ++i)
            power[i] = std::norm (work[(size_t) i]);
    }

private:
    int n = 0, log2n = 0;
    std::vector<std::complex<float>> twiddles, work;
    std::vector<int> bitrev;
};
} // namespace latrigger
