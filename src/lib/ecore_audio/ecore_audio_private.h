#ifndef ECORE_AUDIO_PRIVATE_H_
#define ECORE_AUDIO_PRIVATE_H_

#ifdef __linux__
#include <features.h>
#endif

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifdef HAVE_ALSA
#include <alsa/asoundlib.h>
#endif

#ifdef HAVE_PULSE
#include <pulse/pulseaudio.h>
#endif

#ifdef HAVE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#endif

#ifdef HAVE_SNDFILE
#include <sndfile.h>
#endif

#include <sys/types.h>
#include <sys/stat.h>

#include "Ecore.h"
#include "ecore_private.h"

#include "Ecore_Audio.h"
#include "ecore_audio_protected.h"

extern int _ecore_audio_log_dom;

#ifdef ECORE_AUDIO_DEFAULT_LOG_COLOR
#undef ECORE_AUDIO_DEFAULT_LOG_COLOR
#endif
#define ECORE_AUDIO_DEFAULT_LOG_COLOR EINA_COLOR_BLUE

#ifdef ERR
#undef ERR
#endif
#define ERR(...) EINA_LOG_DOM_ERR(_ecore_audio_log_dom, __VA_ARGS__)

#ifdef DBG
#undef DBG
#endif
#define DBG(...) EINA_LOG_DOM_DBG(_ecore_audio_log_dom, __VA_ARGS__)

#ifdef INF
#undef INF
#endif
#define INF(...) EINA_LOG_DOM_INFO(_ecore_audio_log_dom, __VA_ARGS__)

#ifdef WRN
#undef WRN
#endif
#define WRN(...) EINA_LOG_DOM_WARN(_ecore_audio_log_dom, __VA_ARGS__)

#ifdef CRI
#undef CRI
#endif
#define CRI(...) EINA_LOG_DOM_CRIT(_ecore_audio_log_dom, __VA_ARGS__)

/**
 * @defgroup Ecore_Audio_Module_API_Group Ecore_Audio_Module_API - API for modules
 * @ingroup Ecore_Audio_Group
 *
 * @internal These functions are internal
 *
 * @{
 */

typedef struct _Ecore_Audio_Input Ecore_Audio_Input;
typedef struct _Ecore_Audio_Output Ecore_Audio_Output;

/**
 * @brief The structure representing an Ecore_Audio module
 */
struct _Ecore_Audio_Module
{
   ECORE_MAGIC;
   Ecore_Audio_Type type;
   char              *name;
   Eina_List         *inputs;
   Eina_List         *outputs;

   void              *priv;

   struct input_api  *in_ops;
   struct output_api *out_ops;
};

struct _Ecore_Audio_Vio_Internal {
    Ecore_Audio_Vio *vio;
    void *data;
    efl_key_data_free_func free_func;
};
typedef struct _Ecore_Audio_Vio_Internal Ecore_Audio_Vio_Internal;

/**
 * @brief A common structure, could be input or output
 */
struct _Ecore_Audio_Object
{
   const char         *name;
   const char         *source;

   Eina_Bool           paused;
   double              volume;
   Ecore_Audio_Format  format;
   Ecore_Audio_Vio_Internal *vio;
};

/**
 * @brief The structure representing an Ecore_Audio output
 */
struct _Ecore_Audio_Output
{
   Eina_List          *inputs; /**< The inputs that are connected to this output */
   Ecore_Idler        *write_idler;
   Eina_Bool           need_writer;
};

/**
 * @brief The structure representing an Ecore_Audio input
 */
struct _Ecore_Audio_Input
{
   Eina_Bool           paused; /**< Is the input paused? */
   Eina_Bool           seekable;
   Eina_Bool           seekable_prev;

   Eo                 *output; /**< The output this input is connected to */

   int                 samplerate;
   int                 channels;
   Eina_Bool           looped; /**< Loop the sound */
   double              speed;
   double              length; /**< Length of the sound */
   Eina_Bool           preloaded;
   Eina_Bool           ended;
};

//////////////////////////////////////////////////////////////////////////
#ifdef HAVE_ALSA
/* ecore_audio_alsa */
struct _Ecore_Audio_Alsa
{
   ECORE_MAGIC;
   snd_pcm_t   *handle;
   unsigned int channels;
   unsigned int samplerate;
};

Ecore_Audio_Module *ecore_audio_alsa_init(void);
void                ecore_audio_alsa_shutdown(void);
#endif /* HAVE_ALSA */

//////////////////////////////////////////////////////////////////////////
#ifdef HAVE_PULSE
typedef struct _Ecore_Audio_Lib_Pulse Ecore_Audio_Lib_Pulse;

/* Internal, main-thread probes. Both success and failure are cached for the
 * process lifetime. They use private native loops, never audio Eo objects or
 * a nested Ecore main loop, and wait at most 0.5 seconds for a server reply.
 */
Eina_Bool _ecore_audio_out_pulse_probe(void);

struct _Ecore_Audio_Lib_Pulse
{
   Eina_Module  *mod;

   pa_mainloop        *(*pa_mainloop_new)                  (void);
   void                (*pa_mainloop_free)                 (pa_mainloop *m);
   pa_mainloop_api    *(*pa_mainloop_get_api)              (pa_mainloop *m);
   int                 (*pa_mainloop_prepare)              (pa_mainloop *m, int timeout);
   int                 (*pa_mainloop_poll)                 (pa_mainloop *m);
   int                 (*pa_mainloop_dispatch)             (pa_mainloop *m);

   pa_context         *(*pa_context_new)                   (pa_mainloop_api *mainloop, const char *name);
   pa_context         *(*pa_context_unref)                 (pa_context *c);
   int                 (*pa_context_connect)               (pa_context *c, const char *server, pa_context_flags_t flags, const pa_spawn_api *api);
   void                (*pa_context_disconnect)            (pa_context *c);
   pa_operation       *(*pa_context_set_sink_input_volume) (pa_context *c, uint32_t idx, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata);
   pa_context_state_t  (*pa_context_get_state)             (pa_context *c);
   void                (*pa_context_set_state_callback)    (pa_context *c, pa_context_notify_cb_t cb, void *userdata);
   void                (*pa_operation_unref)               (pa_operation *o);
   pa_cvolume         *(*pa_cvolume_set)                   (pa_cvolume *a, unsigned channels, pa_volume_t v);
   pa_stream          *(*pa_stream_new)                    (pa_context *c, const char *name, const pa_sample_spec *ss, const pa_channel_map *map);
   void                (*pa_stream_unref)                  (pa_stream *s);
   int                 (*pa_stream_connect_playback)       (pa_stream *s, const char *dev, const pa_buffer_attr *attr, pa_stream_flags_t flags, const pa_cvolume *volume, pa_stream *sync_stream);
   int                 (*pa_stream_disconnect)             (pa_stream *s);
   pa_operation       *(*pa_stream_drain)                  (pa_stream *s, pa_stream_success_cb_t cb, void *userdata);
   pa_operation       *(*pa_stream_flush)                  (pa_stream *s, pa_stream_success_cb_t cb, void *userdata);
   pa_operation       *(*pa_stream_cork)                   (pa_stream *s, int b, pa_stream_success_cb_t cb, void *userdata);
   int                 (*pa_stream_write)                  (pa_stream *p, const void *data, size_t nbytes, pa_free_cb_t free_cb, int64_t offset, pa_seek_mode_t seek);
   int                 (*pa_stream_begin_write)            (pa_stream *p, void **data, size_t *nbytes);
   void                (*pa_stream_set_write_callback)     (pa_stream *p, pa_stream_request_cb_t cb, void *userdata);
   pa_operation       *(*pa_stream_trigger)                (pa_stream *s, pa_stream_success_cb_t cb, void *userdata);
   pa_operation       *(*pa_stream_update_sample_rate)     (pa_stream *s, uint32_t rate, pa_stream_success_cb_t cb, void *userdata);
   uint32_t            (*pa_stream_get_index)              (pa_stream *s);
};

#define EPA_CALL(x) ecore_audio_pulse_lib->x
#define EPA_LOAD() ecore_audio_pulse_lib_load()

extern Ecore_Audio_Lib_Pulse *ecore_audio_pulse_lib;

Eina_Bool ecore_audio_pulse_lib_load(void);
void      ecore_audio_pulse_lib_unload(void);

/* These are unused from ecore_audio_pulse.c which isn't used
Ecore_Audio_Module *ecore_audio_pulse_init(void);
void                ecore_audio_pulse_shutdown(void);
 */
#endif /* HAVE_PULSE */

#ifdef HAVE_PIPEWIRE
typedef struct _Ecore_Audio_Lib_Pipewire Ecore_Audio_Lib_Pipewire;

Eina_Bool _ecore_audio_out_pipewire_probe(void);

#ifdef HAVE_SDL_AUDIO
Eina_Bool _ecore_audio_out_sdl_probe(void);
#endif

struct _Ecore_Audio_Lib_Pipewire
{
   Eina_Module *mod;
   Eina_Module *spa_mod;

   void                  (*pw_init)                  (int *argc, char **argv[]);
   void                  (*pw_deinit)                (void);
   struct pw_loop       *(*pw_loop_new)              (const struct spa_dict *props);
   void                  (*pw_loop_destroy)          (struct pw_loop *loop);
   int                   (*pw_loop_get_fd)           (struct pw_loop *loop);
   void                  (*pw_loop_enter)            (struct pw_loop *loop);
   void                  (*pw_loop_leave)            (struct pw_loop *loop);
   int                   (*pw_loop_iterate)          (struct pw_loop *loop, int timeout);
   struct pw_context    *(*pw_context_new)           (struct pw_loop *main_loop, struct pw_properties *props, size_t user_data_size);
   void                  (*pw_context_destroy)       (struct pw_context *context);
   struct pw_core       *(*pw_context_connect)       (struct pw_context *context, struct pw_properties *properties, size_t user_data_size);
   int                   (*pw_core_disconnect)       (struct pw_core *core);
   int                   (*pw_core_add_listener)     (struct pw_core *core, struct spa_hook *listener, const struct pw_core_events *events, void *data);
   int                   (*pw_core_sync)             (struct pw_core *core, uint32_t id, int seq);
   struct pw_properties *(*pw_properties_new)        (const char *key, ...) SPA_SENTINEL;
   struct pw_stream     *(*pw_stream_new)            (struct pw_core *core, const char *name, struct pw_properties *props);
   void                  (*pw_stream_destroy)        (struct pw_stream *stream);
   void                  (*pw_stream_add_listener)   (struct pw_stream *stream, struct spa_hook *listener, const struct pw_stream_events *events, void *data);
   int                   (*pw_stream_connect)        (struct pw_stream *stream, enum pw_direction direction, uint32_t target_id, enum pw_stream_flags flags, const struct spa_pod **params, uint32_t n_params);
   int                   (*pw_stream_update_params)  (struct pw_stream *stream, const struct spa_pod **params, uint32_t n_params);
   struct pw_buffer     *(*pw_stream_dequeue_buffer) (struct pw_stream *stream);
   int                   (*pw_stream_queue_buffer)   (struct pw_stream *stream, struct pw_buffer *buffer);
   int                   (*pw_stream_set_active)     (struct pw_stream *stream, bool active);
   int                   (*pw_stream_flush)          (struct pw_stream *stream, bool drain);
   void                  (*spa_hook_remove)          (struct spa_hook *hook);
   struct spa_pod       *(*spa_format_audio_raw_build)(struct spa_pod_builder *builder, uint32_t id, const struct spa_audio_info_raw *info);
};

#define EPW_CALL(x) ecore_audio_pipewire_lib->x
#define EPW_LOAD() ecore_audio_pipewire_lib_load()

extern Ecore_Audio_Lib_Pipewire *ecore_audio_pipewire_lib;

Eina_Bool ecore_audio_pipewire_lib_load(void);
void      ecore_audio_pipewire_lib_unload(void);
#endif

//////////////////////////////////////////////////////////////////////////
#ifdef HAVE_SNDFILE
/* ecore_audio_sndfile */
typedef struct _Ecore_Audio_Lib_Sndfile Ecore_Audio_Lib_Sndfile;

struct _Ecore_Audio_Lib_Sndfile
{
   Eina_Module  *mod;

   SNDFILE    *(*sf_open)         (const char *path, int mode, SF_INFO *sfinfo);
   SNDFILE    *(*sf_open_virtual) (SF_VIRTUAL_IO *sfvirtual, int mode, SF_INFO *sfinfo, void *user_data);
   int         (*sf_close)        (SNDFILE *sndfile);
   sf_count_t  (*sf_read_float)   (SNDFILE *sndfile, float *ptr, sf_count_t items);
   sf_count_t  (*sf_write_float)  (SNDFILE *sndfile, const float *ptr, sf_count_t items);
   void        (*sf_write_sync)   (SNDFILE *sndfile);
   sf_count_t  (*sf_seek)         (SNDFILE *sndfile, sf_count_t frames, int whence);
   const char *(*sf_strerror)     (SNDFILE *sndfile);
};

#define ESF_CALL(x) ecore_audio_sndfile_lib->x
#define ESF_LOAD() ecore_audio_sndfile_lib_load()

extern Ecore_Audio_Lib_Sndfile *ecore_audio_sndfile_lib;

Eina_Bool ecore_audio_sndfile_lib_load(void);
void      ecore_audio_sndfile_lib_unload(void);
#endif /* HAVE_SNDFILE */

//////////////////////////////////////////////////////////////////////////
Ecore_Audio_Module *ecore_audio_custom_init(void);
void                ecore_audio_custom_shutdown(void);

/**
 * @}
 */
#endif
