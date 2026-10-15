#pragma once

#include <stddef.h>
#include <stdint.h>

#define AUDIO_LAB_WAVE_SAMPLES 128

typedef struct {
    uint32_t trim_start_ms;
    uint32_t trim_end_ms;   /* zero means end of file */
    uint32_t fade_in_ms;
    uint32_t fade_out_ms;
} AudioTrimOptions;

typedef struct {
    uint32_t duration_ms;
    double peak_dbfs;
    double rms_dbfs;
    uint16_t waveform[AUDIO_LAB_WAVE_SAMPLES]; /* 0..1000 relative amplitude */
    int valid;
} AudioLabInfo;

int audio_lab_analyze_mp3(const char *input_path, AudioLabInfo *info,
                          char *error_text, size_t error_text_size);
