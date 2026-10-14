#pragma once

/* Initialize/tear down the audio output and MP3 decoder runtime. */
int mp3SystemInit(void);
void mp3SystemExit(void);

/* Decode and synchronously play one MP3 from the SD-card filesystem. */
int playMp3(const char *file);
