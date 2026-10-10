/* C interface of the OPNA emulation (sound.cpp). Ports: 0 = 188h, 1 = 18Ah, 2 = 18Ch, 3 = 18Eh. */
#ifndef RUSTY_SOUND_H
#define RUSTY_SOUND_H

#include <stddef.h>
#include <stdint.h>

void sound_init(void);
unsigned sound_output_rate(void);
void sound_set_audio_output(int on);
void sound_write(int port, uint8_t value, int64_t now);
uint8_t sound_read(int port, int64_t now);
unsigned sound_update(int64_t now);
void sound_flush(void);
int64_t sound_next_event(void);
void display_audio(const int16_t *samples, int frames);
void display_message(const char *text, int seconds);
struct StateStream;
void state_io(struct StateStream *s, void *data, size_t size);
int state_loading(const struct StateStream *s);
void sound_snapshot(struct StateStream *s);

#endif
