#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include <Eo.h>
#include "ecore_audio_private.h"

#define MY_CLASS ECORE_AUDIO_OUT_SDL_CLASS
#define MY_CLASS_NAME "Ecore_Audio_Out_Sdl"

/* Audio is pushed to SDL from the main loop instead of from SDL's audio
 * thread, so that the inputs are only ever touched from the main loop. */
#define PUMP_INTERVAL 0.02
#define QUEUE_SECONDS 0.15
#define CHUNK_BYTES   4096

typedef struct _Sdl_Stream
{
   Eo *in; /* NULL once detached: the stream only plays out what is queued */
   SDL_AudioDeviceID device;
   int frame_bytes;
   int bytes_per_second;
} Sdl_Stream;

typedef struct _Ecore_Audio_Out_Sdl_Data
{
   Eina_List *streams;
   Eina_List *draining;
   Ecore_Timer *pump_timer;
   Ecore_Job *ready_job;
} Ecore_Audio_Out_Sdl_Data;

Eina_Bool
_ecore_audio_out_sdl_probe(void)
{
   const char *driver;
   Eina_Bool usable;

   if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return EINA_FALSE;
   driver = SDL_GetCurrentAudioDriver();
   usable = driver && strcmp(driver, "dummy") && strcmp(driver, "disk");
   SDL_QuitSubSystem(SDL_INIT_AUDIO);
   return usable;
}

static Sdl_Stream *
_stream_find(Ecore_Audio_Out_Sdl_Data *pd, Eo *in)
{
   Sdl_Stream *stream;
   Eina_List *l;

   EINA_LIST_FOREACH(pd->streams, l, stream)
     if (stream->in == in) return stream;
   return NULL;
}

static void
_stream_free(Sdl_Stream *stream)
{
   SDL_CloseAudioDevice(stream->device);
   free(stream);
}

static void
_stream_fill(Sdl_Stream *stream, double volume)
{
   const Uint32 target = stream->bytes_per_second * QUEUE_SECONDS;
   float samples[CHUNK_BYTES / sizeof(float)];
   Eo *in = stream->in;

   while (SDL_GetQueuedAudioSize(stream->device) < target)
     {
        ssize_t got = ecore_audio_obj_in_read(in, samples, CHUNK_BYTES);

        /* at the end of the input it is stopped, and that can detach and
         * free the stream: do not touch it afterwards */
        if (got <= 0) return;

        got -= got % stream->frame_bytes;
        if (!EINA_DBL_EQ(volume, 1.0))
          {
             size_t i, count = got / sizeof(float);

             for (i = 0; i < count; i++) samples[i] *= volume;
          }
        SDL_QueueAudio(stream->device, samples, got);
     }
}

static Eina_Bool
_pump_cb(void *data)
{
   Eo *eo_obj = data;
   Ecore_Audio_Out_Sdl_Data *pd = efl_data_scope_get(eo_obj, MY_CLASS);
   Ecore_Audio_Object *ea_obj = efl_data_scope_get(eo_obj, ECORE_AUDIO_CLASS);
   Eina_List *l, *next, *inputs = NULL;
   Sdl_Stream *stream;
   Eo *in;

   efl_ref(eo_obj);

   EINA_LIST_FOREACH_SAFE(pd->draining, l, next, stream)
     {
        if (SDL_GetQueuedAudioSize(stream->device) > 0) continue;
        pd->draining = eina_list_remove_list(pd->draining, l);
        _stream_free(stream);
     }

   if (!ea_obj->paused)
     {
        /* the callbacks fired when an input ends may delete inputs */
        EINA_LIST_FOREACH(pd->streams, l, stream)
          inputs = eina_list_append(inputs, efl_ref(stream->in));
        EINA_LIST_FREE(inputs, in)
          {
             stream = _stream_find(pd, in);
             if (stream) _stream_fill(stream, ea_obj->volume);
             efl_unref(in);
          }
     }

   if (!pd->streams && !pd->draining) pd->pump_timer = NULL;
   efl_unref(eo_obj);
   return pd->pump_timer ? ECORE_CALLBACK_RENEW : ECORE_CALLBACK_CANCEL;
}

static void
_pump_start(Eo *eo_obj, Ecore_Audio_Out_Sdl_Data *pd)
{
   if (!pd->pump_timer)
     pd->pump_timer = ecore_timer_add(PUMP_INTERVAL, _pump_cb, eo_obj);
}

static Eina_Bool
_is_input_attached(Eo *eo_obj, Eo *in)
{
   Ecore_Audio_Output *out_obj = efl_data_scope_get(eo_obj, ECORE_AUDIO_OUT_CLASS);

   return out_obj->inputs && eina_list_data_find(out_obj->inputs, in);
}

EOLIAN static Eina_Bool
_ecore_audio_out_sdl_ecore_audio_out_input_attach(Eo *eo_obj, Ecore_Audio_Out_Sdl_Data *pd, Eo *in)
{
   Ecore_Audio_Object *ea_obj = efl_data_scope_get(eo_obj, ECORE_AUDIO_CLASS);
   SDL_AudioSpec want, have;
   Sdl_Stream *stream;

   if (_is_input_attached(eo_obj, in)) return EINA_TRUE;
   if (!ecore_audio_obj_out_input_attach(efl_super(eo_obj, MY_CLASS), in))
     return EINA_FALSE;

   SDL_zero(want);
   want.freq = ecore_audio_obj_in_samplerate_get(in) *
     ecore_audio_obj_in_speed_get(in);
   want.format = AUDIO_F32SYS;
   want.channels = ecore_audio_obj_in_channels_get(in);
   want.samples = 1024;

   stream = calloc(1, sizeof(Sdl_Stream));
   if (stream)
     stream->device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
   if (!stream || !stream->device)
     {
        ERR("Could not open an SDL audio device: %s", SDL_GetError());
        free(stream);
        ecore_audio_obj_out_input_detach(efl_super(eo_obj, MY_CLASS), in);
        return EINA_FALSE;
     }

   stream->in = in;
   stream->frame_bytes = want.channels * sizeof(float);
   stream->bytes_per_second = want.freq * stream->frame_bytes;
   pd->streams = eina_list_append(pd->streams, stream);
   SDL_PauseAudioDevice(stream->device, ea_obj->paused);
   _pump_start(eo_obj, pd);
   return EINA_TRUE;
}

EOLIAN static Eina_Bool
_ecore_audio_out_sdl_ecore_audio_out_input_detach(Eo *eo_obj, Ecore_Audio_Out_Sdl_Data *pd, Eo *in)
{
   Sdl_Stream *stream = _stream_find(pd, in);

   if (!ecore_audio_obj_out_input_detach(efl_super(eo_obj, MY_CLASS), in))
     return EINA_FALSE;
   if (!stream) return EINA_TRUE;

   pd->streams = eina_list_remove(pd->streams, stream);
   stream->in = NULL;
   if (SDL_GetQueuedAudioSize(stream->device) > 0)
     {
        /* let the end of the sound play */
        pd->draining = eina_list_append(pd->draining, stream);
        _pump_start(eo_obj, pd);
     }
   else
     _stream_free(stream);
   return EINA_TRUE;
}

EOLIAN static void
_ecore_audio_out_sdl_ecore_audio_paused_set(Eo *eo_obj, Ecore_Audio_Out_Sdl_Data *pd, Eina_Bool paused)
{
   Sdl_Stream *stream;
   Eina_List *l;

   ecore_audio_obj_paused_set(efl_super(eo_obj, MY_CLASS), paused);
   EINA_LIST_FOREACH(pd->streams, l, stream)
     SDL_PauseAudioDevice(stream->device, paused);
}

static void
_ready_job(void *data)
{
   Ecore_Audio_Out_Sdl_Data *pd = efl_data_scope_get(data, MY_CLASS);

   pd->ready_job = NULL;
   efl_event_callback_call(data, ECORE_AUDIO_OUT_SDL_EVENT_CONTEXT_READY, NULL);
}

EOLIAN static Eo *
_ecore_audio_out_sdl_efl_object_constructor(Eo *eo_obj, Ecore_Audio_Out_Sdl_Data *pd)
{
   Ecore_Audio_Output *out_obj;

   eo_obj = efl_constructor(efl_super(eo_obj, MY_CLASS));
   if (!eo_obj) return NULL;

   if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
     {
        ERR("Could not initialize SDL audio: %s", SDL_GetError());
        return NULL;
     }

   out_obj = efl_data_scope_get(eo_obj, ECORE_AUDIO_OUT_CLASS);
   out_obj->need_writer = EINA_FALSE;

   /* the owner adds its event callbacks after the object is created */
   pd->ready_job = ecore_job_add(_ready_job, eo_obj);
   return eo_obj;
}

EOLIAN static void
_ecore_audio_out_sdl_efl_object_destructor(Eo *eo_obj, Ecore_Audio_Out_Sdl_Data *pd)
{
   Sdl_Stream *stream;

   if (pd->ready_job) ecore_job_del(pd->ready_job);
   if (pd->pump_timer) ecore_timer_del(pd->pump_timer);
   EINA_LIST_FREE(pd->streams, stream)
     _stream_free(stream);
   EINA_LIST_FREE(pd->draining, stream)
     _stream_free(stream);
   SDL_QuitSubSystem(SDL_INIT_AUDIO);

   efl_destructor(efl_super(eo_obj, MY_CLASS));
}

#include "ecore_audio_out_sdl.eo.c"
