#pragma once

#include <stddef.h>
#include <stdint.h>
#include <math.h>

/*
 * Per-file loudness normalization for short system sound effects.
 * Measure RMS over active samples to discount silence, aim for -18 dBFS RMS,
 * and apply a -3 dBFS sample-peak ceiling. Auto boost is capped at +18 dB
 * to avoid excessively raising very quiet/noisy source files. User gain is
 * applied after normalization, but the peak ceiling still takes precedence.
 *
 * This is RMS-based normalization, not LUFS/EBU loudness metering.
 */
static inline double slidenx_normalized_gain(const int16_t *samples,
                                             size_t count,
                                             int manual_gain_db)
{
    double peak = 0.0;
    double threshold;
    double sum_squares = 0.0;
    size_t active_count = 0;
    double rms, normalization_gain, total_gain, manual_gain;
    const double target_rms = 32767.0 * pow(10.0, -18.0 / 20.0);
    const double peak_ceiling = 32767.0 * pow(10.0, -3.0 / 20.0);
    const double max_auto_gain = pow(10.0, 18.0 / 20.0);

    if (!samples || count == 0)
        return pow(10.0, (double)manual_gain_db / 20.0);

    for (size_t i = 0; i < count; i++) {
        double magnitude = samples[i] < 0 ? -(double)samples[i] : (double)samples[i];
        if (magnitude > peak) peak = magnitude;
    }
    if (peak <= 0.0)
        return pow(10.0, (double)manual_gain_db / 20.0);

    threshold = peak * 0.01;
    if (threshold < 32.0) threshold = 32.0;
    if (threshold > peak) threshold = peak * 0.5;

    for (size_t i = 0; i < count; i++) {
        double value = samples[i];
        double magnitude = value < 0.0 ? -value : value;
        if (magnitude >= threshold) {
            sum_squares += value * value;
            active_count++;
        }
    }
    if (active_count == 0)
        return pow(10.0, (double)manual_gain_db / 20.0);

    rms = sqrt(sum_squares / (double)active_count);
    if (rms <= 0.0)
        return pow(10.0, (double)manual_gain_db / 20.0);

    normalization_gain = target_rms / rms;
    if (normalization_gain > max_auto_gain)
        normalization_gain = max_auto_gain;

    manual_gain = pow(10.0, (double)manual_gain_db / 20.0);
    total_gain = normalization_gain * manual_gain;

    /* Uniform gain reduction guarantees no decoded sample exceeds -3 dBFS. */
    if (peak * total_gain > peak_ceiling)
        total_gain = peak_ceiling / peak;

    return total_gain;
}
