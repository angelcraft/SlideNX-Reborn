#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "audio_lab.h"

bool gui_init(void);
void gui_exit(void);
void gui_draw_main(int selected, const char *connect_name, const char *disconnect_name,
                   int gain_db, const char *status, bool outputs_ready, bool sd_ready);
void gui_draw_picker(bool connecting, size_t count, const char *const *files, int selected);

void gui_draw_audio_lab(bool connecting, const char *filename,
                        const AudioLabInfo *info, const AudioTrimOptions *options,
                        int selected_row, const char *status);
