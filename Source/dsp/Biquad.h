#pragma once
#include <cmath>

namespace latrigger
{
/** Simple transposed direct form II biquad (RBJ cookbook coefficients). */
class Biquad
{
public:
    void reset() noexcept { z1 = z2 = 0.0; }

    void setHighPass (double sampleRate, double freq, double q = 0.70710678) noexcept
    {
        const double w0 = 2.0 * M_PI * clampFreq (freq, sampleRate) / sampleRate;
        const double cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        b0 = (1.0 + cw) * 0.5 / a0;
        b1 = -(1.0 + cw) / a0;
        b2 = b0;
        a1 = -2.0 * cw / a0;
        a2 = (1.0 - alpha) / a0;
    }

    void setLowPass (double sampleRate, double freq, double q = 0.70710678) noexcept
    {
        const double w0 = 2.0 * M_PI * clampFreq (freq, sampleRate) / sampleRate;
        const double cw = std::cos (w0), alpha = std::sin (w0) / (2.0 * q);
        const double a0 = 1.0 + alpha;
        b0 = (1.0 - cw) * 0.5 / a0;
        b1 = (1.0 - cw) / a0;
        b2 = b0;
        a1 = -2.0 * cw / a0;
        a2 = (1.0 - alpha) / a0;
    }

    void setBypass() noexcept { b0 = 1.0; b1 = b2 = a1 = a2 = 0.0; }

    inline float process (float x) noexcept
    {
        const double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return (float) y;
    }

private:
    static double clampFreq (double f, double sr) noexcept
    {
        return f < 10.0 ? 10.0 : (f > sr * 0.49 ? sr * 0.49 : f);
    }

    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1 = 0.0, z2 = 0.0;
};

/** 4th order (24 dB/oct) Butterworth-ish filter made of two cascaded biquads. */
class Filter24
{
public:
    void reset() noexcept { s1.reset(); s2.reset(); }

    void setHighPass (double sr, double f) noexcept
    {
        s1.setHighPass (sr, f, 0.54119610);
        s2.setHighPass (sr, f, 1.30656296);
    }

    void setLowPass (double sr, double f) noexcept
    {
        s1.setLowPass (sr, f, 0.54119610);
        s2.setLowPass (sr, f, 1.30656296);
    }

    void setBypass() noexcept { s1.setBypass(); s2.setBypass(); }

    inline float process (float x) noexcept { return s2.process (s1.process (x)); }

private:
    Biquad s1, s2;
};
} // namespace latrigger
