#pragma once
#include <stddef.h>
#include "audio_lab.h"

int convert_mp3_to_raw(const char *input_path, const char *output_path,
                       int gain_db, const AudioTrimOptions *trim,
                       char *error_text, size_t error_text_size);
