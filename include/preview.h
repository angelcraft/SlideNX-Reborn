#pragma once
#include <stddef.h>
#include "audio_lab.h"

int preview_mp3(const char *input_path, int gain_db,
                char *error_text, size_t error_text_size);
int preview_mp3_trimmed(const char *input_path, int gain_db,
                        const AudioTrimOptions *trim,
                        char *error_text, size_t error_text_size);
