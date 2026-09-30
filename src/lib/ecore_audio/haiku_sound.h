#ifndef HAIKU_SOUND_H
#define HAIKU_SOUND_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A BSoundPlayer fed from a ring buffer. The player thread only reads the
 * ring, the caller only writes it: no lock is needed. Samples are 32 bit
 * floats, interleaved. */
typedef struct _Haiku_Sound Haiku_Sound;

int          haiku_sound_probe(void);
Haiku_Sound *haiku_sound_new(int channels, int rate);
void         haiku_sound_free(Haiku_Sound *snd);
size_t       haiku_sound_queued_get(const Haiku_Sound *snd);
/* Returns how many bytes were taken, whole frames only. */
size_t       haiku_sound_queue(Haiku_Sound *snd, const void *data, size_t bytes);
/* While paused nothing is consumed and silence is played. */
void         haiku_sound_paused_set(Haiku_Sound *snd, int paused);

#ifdef __cplusplus
}
#endif

#endif
