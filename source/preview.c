#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include <switch.h>
#include "minimp3.h"
#include "preview.h"
#include "slidenx_normalizer.h"

#define TARGET_RATE 48000U
#define MAX_MP3_SIZE (16U * 1024U * 1024U)
#define PREVIEW_SECONDS 8U
#define MAX_SOURCE_SAMPLES (TARGET_RATE * PREVIEW_SECONDS)
#define PCM_FRAMES_PER_BUFFER 1024U
#define PCM_BUFFER_BYTES (PCM_FRAMES_PER_BUFFER * 2U * sizeof(int16_t))

static void set_error(char *text, size_t size, const char *message)
{
    if (text && size) (void)snprintf(text, size, "%s", message);
}

static void wait_for_buffer(AudioOutBuffer **released, u32 *released_count)
{
    while (*released == NULL) {
        *released_count = 0;
        Result rc = audoutWaitPlayFinish(released, released_count, 100000000ULL);
        if (R_FAILED(rc)) {
            *released_count = 0;
            svcSleepThread(1000000ULL);
        }
    }
}

static size_t fill_preview_samples(uint8_t *pcm_buffer, const int16_t *mono,
                                      size_t mono_count, uint64_t base,
                                      uint64_t total_frames, int source_rate,
                                      double gain, const AudioTrimOptions *trim,
                                      bool reached_preview_limit)
{
    size_t frames = (size_t)((total_frames - base) > PCM_FRAMES_PER_BUFFER ?
                             PCM_FRAMES_PER_BUFFER : (total_frames - base));
    int16_t *samples = (int16_t *)pcm_buffer;
    uint64_t fade_in_frames = trim ? ((uint64_t)trim->fade_in_ms * TARGET_RATE) / 1000U : 0U;
    uint64_t fade_out_frames = trim ? ((uint64_t)trim->fade_out_ms * TARGET_RATE) / 1000U : 0U;
    uint64_t trimmed_source_ms = ((uint64_t)mono_count * 1000U) / (uint64_t)source_rate;
    uint64_t requested_ms = trim && trim->trim_end_ms > 0 &&
                            trim->trim_end_ms > trim->trim_start_ms ?
                            trim->trim_end_ms - trim->trim_start_ms : UINT64_MAX;
    bool fade_out_reaches_end = !reached_preview_limit ||
                                (requested_ms != UINT64_MAX &&
                                 requested_ms <= trimmed_source_ms + 30U);

    if (!fade_out_reaches_end) fade_out_frames = 0;
    if (fade_in_frames > total_frames) fade_in_frames = total_frames;
    if (fade_out_frames > total_frames) fade_out_frames = total_frames;

    for (size_t frame = 0; frame < frames; ++frame) {
        uint64_t i = base + frame;
        uint64_t position = i * (uint64_t)source_rate;
        uint64_t index = position / TARGET_RATE;
        uint32_t fraction = (uint32_t)(position % TARGET_RATE);
        int32_t first, second;
        double sample_value;
        long value;

        if (index >= mono_count) index = mono_count - 1U;
        first = mono[index];
        second = index + 1U < mono_count ? mono[index + 1U] : first;
        sample_value = ((double)first +
                        ((double)second - (double)first) * (double)fraction / TARGET_RATE) * gain;

        if (fade_in_frames > 1U && i < fade_in_frames)
            sample_value *= (double)i / (double)(fade_in_frames - 1U);
        if (fade_out_frames > 1U && i >= total_frames - fade_out_frames)
            sample_value *= (double)(total_frames - 1U - i) / (double)(fade_out_frames - 1U);

        value = lrint(sample_value);
        if (value > 32767) value = 32767;
        if (value < -32768) value = -32768;
        samples[frame * 2U] = (int16_t)value;
        samples[frame * 2U + 1U] = (int16_t)value;
    }
    return frames;
}

static int preview_buffer_slot(AudioOutBuffer *buffer, AudioOutBuffer buffers[2])
{
    if (buffer == &buffers[0]) return 0;
    if (buffer == &buffers[1]) return 1;
    return -1;
}

static bool queue_preview_buffer(AudioOutBuffer buffers[2], int slot,
                                 bool pending[2], size_t *pending_count)
{
    if (pending[slot] || R_FAILED(audoutAppendAudioOutBuffer(&buffers[slot])))
        return false;
    pending[slot] = true;
    (*pending_count)++;
    return true;
}

int preview_mp3_trimmed(const char *input_path, int gain_db,
                        const AudioTrimOptions *trim,
                        char *error_text, size_t error_text_size)
{
    FILE *fp = NULL;
    uint8_t *compressed = NULL;
    int16_t *mono = NULL;
    size_t mono_count = 0, mono_capacity = 0;
    size_t max_source_samples = MAX_SOURCE_SAMPLES;
    long file_size;
    size_t got;
    int offset = 0, source_rate = 0, result = -1;
    uint64_t source_samples_seen = 0, trim_start_source = 0, trim_end_source = UINT64_MAX;
    bool reached_preview_limit = false;
    bool audio_initialized = false, started_here = false;
    uint8_t *pcm_buffers[2] = {NULL, NULL};
    double gain;
    mp3dec_t decoder;
    mp3dec_frame_info_t info;
    mp3d_sample_t decoded[MINIMP3_MAX_SAMPLES_PER_FRAME];

    if (!input_path || gain_db < -12 || gain_db > 6) {
        set_error(error_text, error_text_size, "Invalid MP3 path or gain.");
        return -1;
    }

    fp = fopen(input_path, "rb");
    if (!fp) { set_error(error_text, error_text_size, "Could not open selected MP3."); goto cleanup; }
    if (fseek(fp, 0, SEEK_END) != 0 || (file_size = ftell(fp)) <= 0 || fseek(fp, 0, SEEK_SET) != 0) {
        set_error(error_text, error_text_size, "MP3 is empty or unreadable."); goto cleanup;
    }
    if ((unsigned long)file_size > MAX_MP3_SIZE) {
        set_error(error_text, error_text_size, "MP3 exceeds 16 MiB."); goto cleanup;
    }
    compressed = (uint8_t *)malloc((size_t)file_size);
    if (!compressed) { set_error(error_text, error_text_size, "Not enough memory to read MP3."); goto cleanup; }
    got = fread(compressed, 1, (size_t)file_size, fp);
    if (got != (size_t)file_size) { set_error(error_text, error_text_size, "Could not read the entire MP3."); goto cleanup; }

    mp3dec_init(&decoder);
    while (offset < file_size && mono_count < max_source_samples) {
        int samples;
        size_t remaining, take, needed;
        memset(&info, 0, sizeof(info));
        samples = mp3dec_decode_frame(&decoder, compressed + offset,
                                      (int)(file_size - offset), decoded, &info);
        if (info.frame_bytes <= 0) { offset++; continue; }
        offset += info.frame_bytes;
        if (samples <= 0 || info.channels <= 0 || info.channels > 2 || info.hz <= 0) continue;

        if (source_rate == 0) {
            source_rate = info.hz;
            max_source_samples = (size_t)source_rate * PREVIEW_SECONDS;
            if (max_source_samples > MAX_SOURCE_SAMPLES) max_source_samples = MAX_SOURCE_SAMPLES;
            trim_start_source = trim ? ((uint64_t)trim->trim_start_ms * source_rate) / 1000U : 0U;
            trim_end_source = (trim && trim->trim_end_ms > 0)
                            ? ((uint64_t)trim->trim_end_ms * source_rate) / 1000U : UINT64_MAX;
        } else if (source_rate != info.hz) {
            set_error(error_text, error_text_size, "MP3 sample rate changes mid-file."); goto cleanup;
        }

        remaining = max_source_samples - mono_count;
        take = 0;
        for (int i = 0; i < samples; ++i) {
            uint64_t absolute = source_samples_seen + (uint64_t)i;
            if (absolute < trim_start_source) continue;
            if (absolute >= trim_end_source || take >= remaining) break;
            take++;
        }
        needed = mono_count + take;
        if (needed > mono_capacity) {
            size_t capacity = mono_capacity ? mono_capacity : 32768U;
            int16_t *grown;
            if (capacity > max_source_samples) capacity = max_source_samples;
            while (capacity < needed) {
                size_t next = capacity * 2U;
                capacity = next > max_source_samples ? max_source_samples : next;
                if (capacity < needed && capacity == max_source_samples) break;
            }
            if (capacity < needed) { set_error(error_text, error_text_size, "Preview buffer limit reached."); goto cleanup; }
            grown = (int16_t *)realloc(mono, capacity * sizeof(int16_t));
            if (!grown) { set_error(error_text, error_text_size, "Not enough memory for preview."); goto cleanup; }
            mono = grown; mono_capacity = capacity;
        }
        size_t copied = 0;
        bool at_end = false;
        for (int i = 0; i < samples; ++i) {
            uint64_t absolute = source_samples_seen + (uint64_t)i;
            if (absolute < trim_start_source) continue;
            if (absolute >= trim_end_source) { at_end = true; break; }
            if (copied >= remaining) { reached_preview_limit = true; break; }
            int32_t left = decoded[i * (size_t)info.channels];
            int32_t right = info.channels > 1 ? decoded[i * (size_t)info.channels + 1] : left;
            mono[mono_count + copied] = (int16_t)((left + right) / 2);
            copied++;
        }
        mono_count += copied;
        source_samples_seen += (uint64_t)samples;
        if (at_end || reached_preview_limit) break;
    }

    if (!source_rate || !mono_count) { set_error(error_text, error_text_size, "No decodable MP3 audio frames."); goto cleanup; }

    gain = slidenx_normalized_gain(mono, mono_count, gain_db);
    pcm_buffers[0] = (uint8_t *)memalign(0x1000, PCM_BUFFER_BYTES);
    pcm_buffers[1] = (uint8_t *)memalign(0x1000, PCM_BUFFER_BYTES);
    if (!pcm_buffers[0] || !pcm_buffers[1]) {
        set_error(error_text, error_text_size, "Could not allocate aligned double audio buffers.");
        goto cleanup;
    }

    {
        Result rc = audoutInitialize();
        if (R_FAILED(rc)) { set_error(error_text, error_text_size, "Audio output initialization failed."); goto cleanup; }
        audio_initialized = true;
        if (audoutGetDeviceState() != AudioOutState_Started) {
            rc = audoutStartAudioOut();
            if (R_FAILED(rc)) { set_error(error_text, error_text_size, "Could not start audio output."); goto cleanup; }
            started_here = true;
        }
    }

    {
        uint64_t scaled_count = (uint64_t)mono_count * TARGET_RATE;
        uint64_t total_frames = (scaled_count + (uint64_t)source_rate / 2U) / (uint64_t)source_rate;
        AudioOutBuffer buffers[2];
        bool pending[2] = {false, false};
        size_t pending_count = 0;
        uint64_t next_base = 0;
        int next_slot = 0;
        AudioOutBuffer *released = NULL;

        memset(buffers, 0, sizeof(buffers));

        /* Prime the queue before waiting so the device always has audio ready. */
        while (next_base < total_frames && pending_count < 2U) {
            size_t frames = fill_preview_samples(pcm_buffers[next_slot], mono, mono_count,
                                                 next_base, total_frames, source_rate, gain,
                                                 trim, reached_preview_limit);
            buffers[next_slot].buffer = pcm_buffers[next_slot];
            buffers[next_slot].buffer_size = PCM_BUFFER_BYTES;
            buffers[next_slot].data_size = frames * 2U * sizeof(int16_t);
            buffers[next_slot].data_offset = 0;
            if (!frames || !queue_preview_buffer(buffers, next_slot, pending,
                                                  &pending_count)) {
                set_error(error_text, error_text_size, "Audio output rejected a preview buffer.");
                goto cleanup;
            }
            next_base += frames;
            next_slot ^= 1;
        }

        while (next_base < total_frames || pending_count > 0U) {
            int slot;
            u32 released_count = 0;
            if (!released) wait_for_buffer(&released, &released_count);
            slot = preview_buffer_slot(released, buffers);
            if (slot < 0) {
                released = NULL;
                continue;
            }
            if (released_count > 1U) {
                /* Both queued blocks may have completed while the app was busy. */
                pending[0] = false;
                pending[1] = false;
                pending_count = 0;
            } else if (pending[slot]) {
                pending[slot] = false;
                pending_count--;
            }
            released = NULL;

            if (next_base < total_frames) {
                size_t frames = fill_preview_samples(pcm_buffers[slot], mono, mono_count,
                                                     next_base, total_frames, source_rate, gain,
                                                     trim, reached_preview_limit);
                buffers[slot].buffer = pcm_buffers[slot];
                buffers[slot].buffer_size = PCM_BUFFER_BYTES;
                buffers[slot].data_size = frames * 2U * sizeof(int16_t);
                buffers[slot].data_offset = 0;
                if (!frames || !queue_preview_buffer(buffers, slot, pending,
                                                      &pending_count)) {
                    set_error(error_text, error_text_size, "Audio output rejected a preview buffer.");
                    goto cleanup;
                }
                next_base += frames;
            }
        }
    }

    result = 0;
    set_error(error_text, error_text_size, "OK");

cleanup:
    if (audio_initialized) {
        if (result != 0) {
            bool flushed = false;
            (void)audoutFlushAudioOutBuffers(&flushed);
        }
        if (started_here && audoutGetDeviceState() == AudioOutState_Started)
            (void)audoutStopAudioOut();
        audoutExit();
    }
    free(pcm_buffers[0]);
    free(pcm_buffers[1]);
    free(mono);
    free(compressed);
    if (fp) fclose(fp);
    if (result != 0 && error_text && error_text_size && error_text[0] == '\0')
        set_error(error_text, error_text_size, "Preview failed.");
    return result;
}

int preview_mp3(const char *input_path, int gain_db,
                char *error_text, size_t error_text_size)
{
    return preview_mp3_trimmed(input_path, gain_db, NULL, error_text, error_text_size);
}
