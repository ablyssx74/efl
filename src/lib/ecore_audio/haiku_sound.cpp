#include "haiku_sound.h"

#include <SoundPlayer.h>
#include <atomic>
#include <string.h>
#include <time.h>

#define FAILED_PROBE_SECONDS 60

struct _Haiku_Sound
{
   BSoundPlayer *player;
   unsigned char *ring;
   size_t capacity; /* a power of two */
   size_t frame_bytes;
   std::atomic<size_t> head; /* total bytes written */
   std::atomic<size_t> tail; /* total bytes read */
   std::atomic<int> paused;
};

static void
_play_buffer(void *cookie, void *buffer, size_t size,
             const media_raw_audio_format &)
{
   Haiku_Sound *snd = static_cast<Haiku_Sound *>(cookie);
   unsigned char *out = static_cast<unsigned char *>(buffer);
   size_t head, tail, avail, pos, first;

   if (snd->paused.load())
     {
        memset(out, 0, size);
        return;
     }

   head = snd->head.load(std::memory_order_acquire);
   tail = snd->tail.load(std::memory_order_relaxed);
   avail = head - tail;
   if (avail > size) avail = size;
   avail -= avail % snd->frame_bytes;

   pos = tail & (snd->capacity - 1);
   first = snd->capacity - pos;
   if (first > avail) first = avail;
   memcpy(out, snd->ring + pos, first);
   memcpy(out + first, snd->ring, avail - first);
   if (avail < size) memset(out + avail, 0, size - avail);

   snd->tail.store(tail + avail, std::memory_order_release);
}

extern "C" int
haiku_sound_probe(void)
{
   media_raw_audio_format fmt = media_raw_audio_format::wildcard;
   /* Opening a player takes the media server a while, and every sound
    * asks: remember the answer, a negative one only for a short time as
    * the server may be started later. */
   static int usable = 0;
   static time_t checked = 0;
   BSoundPlayer *player;
   time_t now = time(NULL);

   if (checked && (usable || ((now - checked) < FAILED_PROBE_SECONDS)))
     return usable;

   fmt.format = media_raw_audio_format::B_AUDIO_FLOAT;
   fmt.channel_count = 2;
   fmt.frame_rate = 44100;
   fmt.byte_order = B_MEDIA_HOST_ENDIAN;
   player = new BSoundPlayer(&fmt, "probe");
   usable = (player->InitCheck() == B_OK);
   delete player;
   checked = now;
   return usable;
}

extern "C" Haiku_Sound *
haiku_sound_new(int channels, int rate)
{
   media_raw_audio_format fmt = media_raw_audio_format::wildcard;
   Haiku_Sound *snd;
   size_t wanted;

   if ((channels < 1) || (rate < 1)) return NULL;

   snd = new Haiku_Sound();
   snd->frame_bytes = channels * sizeof(float);
   wanted = snd->frame_bytes * rate / 2;
   snd->capacity = 4096;
   while (snd->capacity < wanted) snd->capacity <<= 1;
   snd->ring = new unsigned char[snd->capacity];
   snd->head = 0;
   snd->tail = 0;
   snd->paused = 0;

   fmt.format = media_raw_audio_format::B_AUDIO_FLOAT;
   fmt.channel_count = channels;
   fmt.frame_rate = rate;
   fmt.byte_order = B_MEDIA_HOST_ENDIAN;
   snd->player = new BSoundPlayer(&fmt, "Enlightenment", _play_buffer,
                                  NULL, snd);
   if ((snd->player->InitCheck() != B_OK) || (snd->player->Start() != B_OK))
     {
        delete snd->player;
        delete[] snd->ring;
        delete snd;
        return NULL;
     }
   snd->player->SetHasData(true);
   return snd;
}

extern "C" void
haiku_sound_free(Haiku_Sound *snd)
{
   if (!snd) return;
   snd->player->Stop(true);
   delete snd->player;
   delete[] snd->ring;
   delete snd;
}

extern "C" size_t
haiku_sound_queued_get(const Haiku_Sound *snd)
{
   return snd->head.load(std::memory_order_relaxed) -
     snd->tail.load(std::memory_order_acquire);
}

extern "C" size_t
haiku_sound_queue(Haiku_Sound *snd, const void *data, size_t bytes)
{
   const unsigned char *in = static_cast<const unsigned char *>(data);
   size_t head = snd->head.load(std::memory_order_relaxed);
   size_t tail = snd->tail.load(std::memory_order_acquire);
   size_t space = snd->capacity - (head - tail);
   size_t pos, first;

   if (bytes > space) bytes = space;
   bytes -= bytes % snd->frame_bytes;

   pos = head & (snd->capacity - 1);
   first = snd->capacity - pos;
   if (first > bytes) first = bytes;
   memcpy(snd->ring + pos, in, first);
   memcpy(snd->ring, in + first, bytes - first);

   snd->head.store(head + bytes, std::memory_order_release);
   return bytes;
}

extern "C" void
haiku_sound_paused_set(Haiku_Sound *snd, int paused)
{
   snd->paused.store(paused);
}
