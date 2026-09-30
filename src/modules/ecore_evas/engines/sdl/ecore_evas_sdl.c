#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include <SDL2/SDL.h>

#include <Ecore.h>
#include <Efl_Core.h>
#include "ecore_private.h"
#include <Ecore_Input.h>
#include <Ecore_Input_Evas.h>
#include <Ecore_Sdl.h>
#include <Evas_Engine_Buffer.h>
#ifdef BUILD_ECORE_EVAS_OPENGL_SDL
# include <Evas_Engine_GL_SDL.h>
#endif

#include <Ecore_Evas.h>
#include "ecore_evas_private.h"

#ifdef _WIN32
# ifndef EFL_MODULE_STATIC
#  define EMODAPI __declspec(dllexport)
# else
#  define EMODAPI
# endif
#else
# ifdef __GNUC__
#  if __GNUC__ >= 4
#   define EMODAPI __attribute__ ((visibility("default")))
#  endif
# endif
#endif /* ! _WIN32 */

#ifndef EMODAPI
# define EMODAPI
#endif

/*
 * SDL only handle one window at a time. That's by definition, there is nothing wrong here.
 *
 */

/* static char *ecore_evas_default_display = "0"; */
/* static Ecore_List *ecore_evas_input_devices = NULL; */

typedef struct _Ecore_Evas_SDL_Switch_Data Ecore_Evas_SDL_Switch_Data;
struct _Ecore_Evas_SDL_Switch_Data
{
   SDL_Texture *page;
   SDL_Renderer *r;
   SDL_Window *w;

   /* Evas renders into this buffer, which outlives every frame: only the
    * damaged part of a frame is redrawn, and the pixels of a locked SDL
    * texture are not guaranteed to be preserved between two locks. */
   void *pixels;
   int pitch;

   Ecore_Timer *redraw_timer;
};

static int                      _ecore_evas_init_count = 0;
static Ecore_Job                *_sdl_clipboard_job = NULL;

static Ecore_Event_Handler      *ecore_evas_event_handlers[4] = {
   NULL, NULL, NULL, NULL
};

static const char               *ecore_evas_sdl_default = "EFL SDL";
static Ecore_Poller             *ecore_evas_event;
static int                      _ecore_evas_fps_debug = 0;
static int                       ecore_evas_sdl_count = 0;

static Ecore_Evas *
_ecore_evas_sdl_match(unsigned int windowID)
{
   return SDL_GetWindowData(SDL_GetWindowFromID(windowID), "_Ecore_Evas");
}

static void *
_ecore_evas_sdl_switch_buffer(void *data, void *dest EINA_UNUSED)
{
   Ecore_Evas_SDL_Switch_Data *swd = data;

   SDL_UpdateTexture(swd->page, NULL, swd->pixels, swd->pitch);
   SDL_RenderCopy(swd->r, swd->page, NULL, NULL);
   SDL_RenderPresent(swd->r);

   return swd->pixels;
}

/* (Re)create the texture and the pixel buffer for a w x h canvas and tell the
 * buffer engine about them. */
static Eina_Bool
_ecore_evas_sdl_buffer_reset(Ecore_Evas *ee, int w, int h)
{
   Ecore_Evas_SDL_Switch_Data *swd = (Ecore_Evas_SDL_Switch_Data*)(ee + 1);
   Evas_Engine_Info_Buffer *einfo;

   einfo = (Evas_Engine_Info_Buffer *) evas_engine_info_get(ee->evas);
   if (!einfo) return EINA_FALSE;

   if (swd->page) SDL_DestroyTexture(swd->page);
   free(swd->pixels);

   SDL_RenderClear(swd->r);
   swd->pitch = w * 4;
   swd->page = SDL_CreateTexture(swd->r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
   swd->pixels = calloc(h, swd->pitch);
   if (!swd->page || !swd->pixels) return EINA_FALSE;

   einfo->info.depth_type = EVAS_ENGINE_BUFFER_DEPTH_RGB32;
   einfo->info.switch_data = swd;
   einfo->info.dest_buffer = swd->pixels;
   einfo->info.dest_buffer_row_bytes = swd->pitch;
   einfo->info.use_color_key = 0;
   einfo->info.alpha_threshold = 0;
   einfo->info.func.new_update_region = NULL;
   einfo->info.func.free_update_region = NULL;
   einfo->info.func.switch_buffer = _ecore_evas_sdl_switch_buffer;
   return evas_engine_info_set(ee->evas, (Evas_Engine_Info *) einfo);
}

static Eina_Bool
_ecore_evas_sdl_event_got_focus(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   Ecore_Sdl_Event_Window *ev = event;
   Ecore_Evas *ee;

   ee = _ecore_evas_sdl_match(ev->windowID);
   /* pass on event */
   if (!ee) return ECORE_CALLBACK_PASS_ON;
   _ecore_evas_focus_device_set(ee, NULL, EINA_TRUE);
   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_ecore_evas_sdl_event_lost_focus(void *data EINA_UNUSED, int type EINA_UNUSED, void *event EINA_UNUSED)
{
   Ecore_Sdl_Event_Window *ev = event;
   Ecore_Evas *ee;

   ee = _ecore_evas_sdl_match(ev->windowID);

   if (!ee) return ECORE_CALLBACK_PASS_ON;
   /* pass on event */
   _ecore_evas_focus_device_set(ee, NULL, EINA_FALSE);
   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_ecore_evas_sdl_redraw_cb(void *data)
{
   Ecore_Evas *ee = data;
   Ecore_Evas_SDL_Switch_Data *swd = (Ecore_Evas_SDL_Switch_Data*)(ee + 1);

   swd->redraw_timer = NULL;
   evas_damage_rectangle_add(ee->evas, 0, 0, ee->w, ee->h);
   return ECORE_CALLBACK_CANCEL;
}

/* Frames presented while the native window is still being created and sized
 * can be dropped (seen on Haiku, where the window stays black until the next
 * frame), so present the whole canvas once more shortly after. */
static void
_ecore_evas_sdl_redraw_later(Ecore_Evas *ee)
{
   Ecore_Evas_SDL_Switch_Data *swd = (Ecore_Evas_SDL_Switch_Data*)(ee + 1);

   if (swd->redraw_timer) ecore_timer_del(swd->redraw_timer);
   if (_sdl_clipboard_job)
     {
        ecore_job_del(_sdl_clipboard_job);
        _sdl_clipboard_job = NULL;
     }
   swd->redraw_timer = ecore_timer_add(0.25, _ecore_evas_sdl_redraw_cb, ee);
}

static Eina_Bool
_ecore_evas_sdl_event_video_resize(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   Ecore_Sdl_Event_Video_Resize *e;
   Ecore_Evas *ee;

   e = event;
   ee = _ecore_evas_sdl_match(e->windowID);

   if (!ee) return ECORE_CALLBACK_PASS_ON; /* pass on event */

   /* Already at that size, e.g. we resized the window ourselves: the frame
    * presented before the window manager applied the size may have been
    * lost, so just redraw. */
   if ((ee->w == e->w) && (ee->h == e->h))
     {
        evas_damage_rectangle_add(ee->evas, 0, 0, e->w, e->h);
        _ecore_evas_sdl_redraw_later(ee);
        return ECORE_CALLBACK_PASS_ON;
     }

   if (evas_output_method_get(ee->evas) == evas_render_method_lookup("buffer"))
     {
        if (!_ecore_evas_sdl_buffer_reset(ee, e->w, e->h))
          return EINA_FALSE;
     }

   ee->w = e->w;
   ee->h = e->h;
   ee->req.w = e->w;
   ee->req.h = e->h;

   evas_output_size_set(ee->evas, e->w, e->h);
   evas_output_viewport_set(ee->evas, 0, 0, e->w, e->h);
   evas_damage_rectangle_add(ee->evas, 0, 0, e->w, e->h);
   _ecore_evas_sdl_redraw_later(ee);

   /* let the application (elm_win) know about the new size */
   if (ee->func.fn_resize) ee->func.fn_resize(ee);

   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_ecore_evas_sdl_event_video_expose(void *data EINA_UNUSED, int type EINA_UNUSED, void *event)
{
   Ecore_Sdl_Event_Window *ev = event;
   Ecore_Evas *ee;
   int w;
   int h;

   ee = _ecore_evas_sdl_match(ev->windowID);

   if (!ee) return ECORE_CALLBACK_PASS_ON;
   evas_output_size_get(ee->evas, &w, &h);
   evas_damage_rectangle_add(ee->evas, 0, 0, w, h);

   return ECORE_CALLBACK_PASS_ON;
}

static Eina_Bool
_ecore_evas_sdl_event(void *data EINA_UNUSED)
{
   ecore_sdl_feed_events();
   return ECORE_CALLBACK_RENEW;
}

static int
_ecore_evas_sdl_init(int w EINA_UNUSED, int h EINA_UNUSED)
{
   _ecore_evas_init_count++;
   if (_ecore_evas_init_count > 1) return _ecore_evas_init_count;

#ifndef _WIN32
   if (getenv("ECORE_EVAS_FPS_DEBUG")) _ecore_evas_fps_debug = 1;
#endif /* _WIN32 */
   // this is pretty bad: poller? and set poll time? pol time is meant to be
   // adjustable for things like polling battery state, or amoutn of spare
   // memory etc.
   //
   ecore_evas_event = ecore_poller_add(ECORE_POLLER_CORE, 1, _ecore_evas_sdl_event, NULL);
   ecore_poller_poll_interval_set(ECORE_POLLER_CORE, 0.006);
#ifndef _WIN32
   if (_ecore_evas_fps_debug) _ecore_evas_fps_debug_init();
#endif /* _WIN32 */

   ecore_event_evas_init();

   ecore_evas_event_handlers[0] = ecore_event_handler_add(ECORE_SDL_EVENT_GOT_FOCUS, _ecore_evas_sdl_event_got_focus, NULL);
   ecore_evas_event_handlers[1] = ecore_event_handler_add(ECORE_SDL_EVENT_LOST_FOCUS, _ecore_evas_sdl_event_lost_focus, NULL);
   ecore_evas_event_handlers[2] = ecore_event_handler_add(ECORE_SDL_EVENT_RESIZE, _ecore_evas_sdl_event_video_resize, NULL);
   ecore_evas_event_handlers[3] = ecore_event_handler_add(ECORE_SDL_EVENT_EXPOSE, _ecore_evas_sdl_event_video_expose, NULL);

   return _ecore_evas_init_count;
}

static int
_ecore_evas_sdl_shutdown(void)
{
   _ecore_evas_init_count--;
   if (_ecore_evas_init_count == 0)
     {
        unsigned int i;

        for (i = 0; i < sizeof (ecore_evas_event_handlers) / sizeof (Ecore_Event_Handler*); i++)
          ecore_event_handler_del(ecore_evas_event_handlers[i]);
        ecore_event_evas_shutdown();
        ecore_poller_del(ecore_evas_event);
        ecore_evas_event = NULL;
#ifndef _WIN32
        if (_ecore_evas_fps_debug) _ecore_evas_fps_debug_shutdown();
#endif /* _WIN32 */
     }
   if (_ecore_evas_init_count < 0) _ecore_evas_init_count = 0;
   return _ecore_evas_init_count;
}

static void
_ecore_evas_sdl_free(Ecore_Evas *ee)
{
   Ecore_Evas_SDL_Switch_Data *swd = (Ecore_Evas_SDL_Switch_Data*) (ee + 1);

   ecore_event_window_unregister(SDL_GetWindowID(swd->w));

   if (swd->redraw_timer) ecore_timer_del(swd->redraw_timer);
   if (swd->page)
     SDL_DestroyTexture(swd->page);
   free(swd->pixels);
   if (swd->r)
     SDL_DestroyRenderer(swd->r);
   if (swd->w)
     SDL_DestroyWindow(swd->w);

   _ecore_evas_sdl_shutdown();
   ecore_sdl_shutdown();
   ecore_evas_sdl_count--;

   SDL_VideoQuit();
}

static void
_ecore_evas_resize(Ecore_Evas *ee, int w, int h)
{

   if ((w == ee->w) && (h == ee->h)) return;
   ee->req.w = w;
   ee->req.h = h;
   ee->w = w;
   ee->h = h;

   /* Also resize the native window, otherwise a window created with a
    * placeholder size never grows to what the application asked for. */
   {
      Ecore_Evas_SDL_Switch_Data *swd = (Ecore_Evas_SDL_Switch_Data*)(ee + 1);
      int cw = 0, ch = 0;

      SDL_GetWindowSize(swd->w, &cw, &ch);
      if ((cw != w) || (ch != h)) SDL_SetWindowSize(swd->w, w, h);
   }

   if (evas_output_method_get(ee->evas) == evas_render_method_lookup("buffer"))
     {
        if (!_ecore_evas_sdl_buffer_reset(ee, w, h))
          return;
     }

   evas_output_size_set(ee->evas, ee->w, ee->h);
   evas_output_viewport_set(ee->evas, 0, 0, ee->w, ee->h);
   evas_damage_rectangle_add(ee->evas, 0, 0, ee->w, ee->h);
   _ecore_evas_sdl_redraw_later(ee);

   if (ee->func.fn_resize) ee->func.fn_resize(ee);
}

static void
_ecore_evas_move_resize(Ecore_Evas *ee, int x, int y, int w, int h)
{
   if ((ee->x != x) || (ee->y != y))
     {
        ee->req.x = x;
        ee->req.y = y;
        ee->x = x;
        ee->y = y;
        if (ee->func.fn_move) ee->func.fn_move(ee);
     }
   _ecore_evas_resize(ee, w, h);
}

static void
_ecore_evas_show(Ecore_Evas *ee)
{
   ee->prop.withdrawn = EINA_FALSE;
   if (ee->func.fn_state_change) ee->func.fn_state_change(ee);
   if (ecore_evas_focus_device_get(ee, NULL)) return;
   _ecore_evas_focus_device_set(ee, NULL, EINA_TRUE);
   evas_event_feed_mouse_in(ee->evas, (unsigned int)((unsigned long long)(ecore_time_get() * 1000.0) & 0xffffffff), NULL);
}

/* Clipboard: the copy and paste buffer is mapped to the native clipboard
 * through SDL (BClipboard on Haiku). The primary selection stays inside the
 * process. */
static Ecore_Evas_Selection_Callbacks _sdl_sel_cbs[ECORE_EVAS_SELECTION_BUFFER_LAST];
static unsigned int _sdl_sel_seat = 0;

static Eina_Bool
_sdl_sel_is_text(const char *type)
{
   return type && (!strcmp(type, "text/plain;charset=utf-8") ||
                   !strcmp(type, "text/plain"));
}

/* Copy the clipboard content to the native clipboard. This cannot be done
 * from inside the claim: ecore_evas only stores the content once the claim
 * returned, so the delivery callback would find nothing. */
static void
_ecore_evas_sdl_clipboard_push(void *data)
{
   Ecore_Evas *ee = data;
   Ecore_Evas_Selection_Callbacks *cbs =
     &_sdl_sel_cbs[ECORE_EVAS_SELECTION_BUFFER_COPY_AND_PASTE_BUFFER];
   const char *type = NULL;
   Eina_Rw_Slice slice;
   unsigned int i;

   _sdl_clipboard_job = NULL;
   if (!cbs->delivery || !cbs->available_types) return;

   for (i = 0; !type && (i < eina_array_count_get(cbs->available_types)); i++)
     {
        const char *t = eina_array_data_get(cbs->available_types, i);

        if (_sdl_sel_is_text(t)) type = t;
     }
   if (!type) return;

   if (cbs->delivery(ee, _sdl_sel_seat,
                     ECORE_EVAS_SELECTION_BUFFER_COPY_AND_PASTE_BUFFER,
                     type, &slice) && slice.mem)
     {
        char *txt = strndup(slice.mem, slice.len);

        if (txt)
          {
             SDL_SetClipboardText(txt);
             free(txt);
          }
        free(slice.mem);
     }
}

static Eina_Bool
_ecore_evas_sdl_selection_claim(Ecore_Evas *ee, unsigned int seat, Ecore_Evas_Selection_Buffer selection, Eina_Array *available_types, Ecore_Evas_Selection_Internal_Delivery delivery, Ecore_Evas_Selection_Internal_Cancel cancel)
{
   Ecore_Evas_Selection_Callbacks *cbs = &_sdl_sel_cbs[selection];

   if (cbs->cancel) cbs->cancel(ee, _sdl_sel_seat, selection);
   if (cbs->available_types) eina_array_free(cbs->available_types);

   cbs->delivery = delivery;
   cbs->cancel = cancel;
   cbs->available_types = available_types;
   _sdl_sel_seat = seat;

   if ((selection == ECORE_EVAS_SELECTION_BUFFER_COPY_AND_PASTE_BUFFER) && delivery)
     {
        if (_sdl_clipboard_job) ecore_job_del(_sdl_clipboard_job);
        _sdl_clipboard_job = ecore_job_add(_ecore_evas_sdl_clipboard_push, ee);
     }

   if (ee->func.fn_selection_changed)
     ee->func.fn_selection_changed(ee, seat, selection);

   return EINA_TRUE;
}

static Eina_Bool
_ecore_evas_sdl_selection_has_owner(Ecore_Evas *ee EINA_UNUSED, unsigned int seat EINA_UNUSED, Ecore_Evas_Selection_Buffer selection)
{
   if (selection == ECORE_EVAS_SELECTION_BUFFER_COPY_AND_PASTE_BUFFER)
     return SDL_HasClipboardText();
   return EINA_FALSE;
}

static Eina_Future *
_ecore_evas_sdl_selection_request(Ecore_Evas *ee, unsigned int seat, Ecore_Evas_Selection_Buffer selection, Eina_Array *acceptable_types)
{
   Ecore_Evas_Selection_Callbacks *cbs = &_sdl_sel_cbs[selection];
   Eina_Content *content = NULL;
   const char *type = NULL;
   Eina_Value value;
   unsigned int i, j;

   if (selection == ECORE_EVAS_SELECTION_BUFFER_COPY_AND_PASTE_BUFFER)
     {
        for (i = 0; !type && (i < eina_array_count_get(acceptable_types)); i++)
          {
             const char *t = eina_array_data_get(acceptable_types, i);

             if (_sdl_sel_is_text(t)) type = t;
          }
        if (type && SDL_HasClipboardText())
          {
             char *txt = SDL_GetClipboardText();

             if (txt)
               {
                  Eina_Slice slice = { strlen(txt) + 1, txt };

                  content = eina_content_new(slice, type);
                  SDL_free(txt);
               }
          }
     }

   /* in-process buffer (primary selection, or nothing on the clipboard) */
   if (!content && cbs->delivery && cbs->available_types)
     {
        type = NULL;
        for (i = 0; !type && (i < eina_array_count_get(cbs->available_types)); i++)
          {
             const char *a = eina_array_data_get(cbs->available_types, i);

             for (j = 0; j < eina_array_count_get(acceptable_types); j++)
               if (!strcmp(a, eina_array_data_get(acceptable_types, j)))
                 {
                    type = a;
                    break;
                 }
          }
        if (type)
          {
             Eina_Rw_Slice slice;

             if (cbs->delivery(ee, seat, selection, type, &slice) && slice.mem)
               {
                  content = eina_content_new(eina_rw_slice_slice_get(slice), type);
                  free(slice.mem);
               }
          }
     }

   for (i = 0; i < eina_array_count_get(acceptable_types); i++)
     eina_stringshare_del(eina_array_data_get(acceptable_types, i));
   eina_array_free(acceptable_types);

   if (!content)
     return eina_future_resolved(efl_loop_future_scheduler_get(efl_main_loop_get()),
                                 eina_value_int_init(0));

   value = eina_value_content_init(content);
   eina_content_free(content);
   return eina_future_resolved(efl_loop_future_scheduler_get(efl_main_loop_get()), value);
}

static void
_ecore_evas_title_set(Ecore_Evas *ee, const char *title)
{
   Ecore_Evas_SDL_Switch_Data *swd = (Ecore_Evas_SDL_Switch_Data*)(ee + 1);

   if (eina_streq(ee->prop.title, title)) return;
   free(ee->prop.title);
   ee->prop.title = eina_strdup(title);
   SDL_SetWindowTitle(swd->w, title ? title : "");
}

static Ecore_Evas_Engine_Func _ecore_sdl_engine_func =
{
   _ecore_evas_sdl_free,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   _ecore_evas_resize,
   _ecore_evas_move_resize,
   NULL,
   NULL,
   _ecore_evas_show,
   NULL,
   NULL,
   NULL,
   NULL,
   _ecore_evas_title_set,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL, //transparent
   NULL, // profiles_set
   NULL, // profile_set

   NULL,
   NULL,
   NULL,
   NULL,
   NULL,
   NULL,

   NULL, // render
   NULL, // screen_geometry_get
   NULL, // screen_dpi_get
   NULL,
   NULL,  // msg_send

   NULL, // pointer_xy_get
   NULL, // pointer_warp

   NULL, // wm_rot_preferred_rotation_set
   NULL, // wm_rot_available_rotations_set
   NULL, // wm_rot_manual_rotation_done_set
   NULL, // wm_rot_manual_rotation_done

   NULL, // aux_hints_set

   NULL, // fn_animator_register
   NULL, // fn_animator_unregister

   NULL, // fn_evas_changed
   NULL, //fn_focus_device_set
   NULL, //fn_callback_focus_device_in_set
   NULL, //fn_callback_focus_device_out_set
   NULL, //fn_callback_device_mouse_in_set
   NULL, //fn_callback_device_mouse_out_set
   NULL, //fn_pointer_device_xy_get
   NULL, //fn_prepare
   NULL, //fn_last_tick_get
   _ecore_evas_sdl_selection_claim,
   _ecore_evas_sdl_selection_has_owner,
   _ecore_evas_sdl_selection_request,
};

static Ecore_Evas*
_ecore_evas_internal_sdl_new(int rmethod, const char* name, int w, int h, int fullscreen, int hwsurface, int noframe EINA_UNUSED, int alpha)
{
   Ecore_Evas_SDL_Switch_Data *swd;
   Ecore_Evas *ee;
   Eina_Bool gl = EINA_FALSE;

   if (ecore_evas_sdl_count > 0) return NULL;
   if (!name)
     name = ecore_evas_sdl_default;

   if (!ecore_sdl_init(name)) return NULL;

   if (SDL_VideoInit(NULL) != 0)
     {
        ERR("SDL Video initialization failed !");
        return NULL;
     }

   ee = calloc(1, sizeof(Ecore_Evas) + sizeof (Ecore_Evas_SDL_Switch_Data));
   if (!ee) return NULL;

   swd = (Ecore_Evas_SDL_Switch_Data*)(ee + 1);

   ECORE_MAGIC_SET(ee, ECORE_MAGIC_EVAS);

   ee->engine.func = (Ecore_Evas_Engine_Func *)&_ecore_sdl_engine_func;

   ee->driver = "sdl";
   if (name) ee->name = strdup(name);

   if (w < 1) w = 1;
   if (h < 1) h = 1;
   ee->visible = 1;
   ee->req.w = w;
   ee->req.h = h;
   ee->w = w;
   ee->h = h;

   ee->prop.max.w = 0;
   ee->prop.max.h = 0;
   ee->prop.layer = 0;
   ee->prop.borderless = EINA_TRUE;
   ee->prop.override = EINA_TRUE;
   ee->prop.maximized = EINA_TRUE;
   ee->prop.fullscreen = fullscreen;
   ee->prop.withdrawn = EINA_TRUE;
   ee->prop.sticky = EINA_FALSE;
   ee->prop.window = 0;
   ee->alpha = alpha;
   ee->prop.hwsurface = hwsurface;

   /* init evas here */
   if (!ecore_evas_evas_new(ee, w, h))
     {
        ERR("Can not create Canvas.");
        goto on_error;
     }

   evas_output_method_set(ee->evas, rmethod);

   gl = !(rmethod == evas_render_method_lookup("buffer"));
   /* The buffer flush ends up in SDL_UnlockTexture()/SDL_RenderPresent(),
    * and SDL's renderer must only be driven from the thread that owns the
    * window. Rendering asynchronously calls it from the evas render thread,
    * which aborts on some platforms (e.g. Haiku's BGLView::UnlockGL()). */
   ee->can_async_render = EINA_FALSE;

   swd->w = SDL_CreateWindow(name,
                             SDL_WINDOWPOS_UNDEFINED,
                             SDL_WINDOWPOS_UNDEFINED,
                             w, h,
                             SDL_WINDOW_RESIZABLE | (gl ? SDL_WINDOW_OPENGL : 0));
   if (!swd->w)
     {
        ERR("SDL_CreateWindow failed.");
        goto on_error;
     }

   SDL_StartTextInput();

   if (!gl)
     {
        swd->r = SDL_CreateRenderer(swd->w, -1, 0);
        if (!swd->r)
          {
             ERR("SDL_CreateRenderer failed.");
             goto on_error;
          }

        if (!_ecore_evas_sdl_buffer_reset(ee, w, h))
          {
             ERR("evas_engine_info_set() for engine '%s' failed.", ee->driver);
             ecore_evas_free(ee);
             return NULL;
          }
     }
   else
     {
        /* FIXME */
#ifdef BUILD_ECORE_EVAS_OPENGL_SDL
        Evas_Engine_Info_GL_SDL *einfo;

        einfo = (Evas_Engine_Info_GL_SDL *) evas_engine_info_get(ee->evas);
        if (einfo)
          {
             einfo->flags.fullscreen = fullscreen;
             einfo->flags.noframe = noframe;
             einfo->window = swd->w;
             if (!evas_engine_info_set(ee->evas, (Evas_Engine_Info *)einfo))
               {
                  ERR("evas_engine_info_set() for engine '%s' failed.", ee->driver);
                  ecore_evas_free(ee);
                  return NULL;
               }
          }
        else
          {
             ERR("evas_engine_info_set() init engine '%s' failed.", ee->driver);
             ecore_evas_free(ee);
             return NULL;
          }
#endif
     }

   _ecore_evas_sdl_init(w, h);
   ee->prop.window = SDL_GetWindowID(swd->w);

   ecore_evas_done(ee, EINA_FALSE);

   SDL_SetWindowData(swd->w, "_Ecore_Evas", ee);
   SDL_ShowCursor(SDL_ENABLE);

   _ecore_evas_focus_device_set(ee, NULL, EINA_TRUE);
   ecore_evas_sdl_count++;
   return ee;

 on_error:
   ecore_evas_free(ee);
   return NULL;
}

EMODAPI Ecore_Evas *
ecore_evas_sdl_new_internal(const char* name, int w, int h, int fullscreen,
                            int hwsurface, int noframe, int alpha)
{
   Ecore_Evas          *ee;
   int                  rmethod;

   rmethod = evas_render_method_lookup("buffer");
   if (!rmethod) return NULL;

   ee = _ecore_evas_internal_sdl_new(rmethod, name, w, h, fullscreen, hwsurface, noframe, alpha);
   return ee;
}

EMODAPI Ecore_Evas*
ecore_evas_sdl16_new_internal(const char* name EINA_UNUSED, int w EINA_UNUSED, int h EINA_UNUSED, int fullscreen EINA_UNUSED, int hwsurface EINA_UNUSED, int noframe EINA_UNUSED, int alpha EINA_UNUSED)
{
   ERR("OUCH !");
   return NULL;
}

#ifdef BUILD_ECORE_EVAS_OPENGL_SDL
EMODAPI Ecore_Evas *
ecore_evas_gl_sdl_new_internal(const char* name, int w, int h, int fullscreen, int noframe)
{
   Ecore_Evas          *ee;
   int                  rmethod;

   rmethod = evas_render_method_lookup("gl_sdl");
   if (!rmethod) return NULL;

   ee = _ecore_evas_internal_sdl_new(rmethod, name, w, h, fullscreen, 0, noframe, 0);
   if (ee) ee->driver = "gl_sdl";
   return ee;
}
#endif
