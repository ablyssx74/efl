#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdlib.h>
#include <string.h>

#include <Ecore.h>
#include <Efl_Core.h>
#include "ecore_private.h"
#include <Ecore_Input.h>
#include <Ecore_Input_Evas.h>
#include <Evas_Engine_Buffer.h>

#include <Ecore_Evas.h>
#include "ecore_evas_private.h"

#include "haiku_window.h"

#ifdef EMODAPI
# undef EMODAPI
#endif
#ifdef _WIN32
# define EMODAPI __declspec(dllexport)
#else
# ifdef __GNUC__
#  if __GNUC__ >= 4
#   define EMODAPI __attribute__ ((visibility("default")))
#  else
#   define EMODAPI
#  endif
# else
#  define EMODAPI
# endif
#endif

/* Native Haiku engine: the canvas is rendered into a buffer of ours by the
 * evas buffer engine, and each frame is copied into a window of the app
 * server. There is no OpenGL (nor SDL) anywhere in it. */

typedef struct _Ecore_Evas_Haiku_Data Ecore_Evas_Haiku_Data;
struct _Ecore_Evas_Haiku_Data
{
   Haiku_Window *win;
   Ecore_Fd_Handler *fd_handler;

   /* what evas renders into; it outlives every frame because evas only
    * redraws the damaged part of a frame */
   void *pixels;
   int pitch;
   int w, h;
};

static int _ecore_evas_init_count = 0;
static int _ecore_evas_haiku_count = 0;
static Ecore_Job *_clipboard_job = NULL;

static unsigned int
_timestamp_get(void)
{
   return (unsigned int)((unsigned long long)(ecore_time_get() * 1000.0) & 0xffffffff);
}

static unsigned int
_modifiers_get(unsigned int haiku_modifiers)
{
   unsigned int ret = 0;

   if (haiku_modifiers & HAIKU_MOD_SHIFT) ret |= ECORE_EVENT_MODIFIER_SHIFT;
   if (haiku_modifiers & HAIKU_MOD_CONTROL) ret |= ECORE_EVENT_MODIFIER_CTRL;
   if (haiku_modifiers & HAIKU_MOD_ALT) ret |= ECORE_EVENT_MODIFIER_ALT;
   if (haiku_modifiers & HAIKU_MOD_SUPER) ret |= ECORE_EVENT_MODIFIER_WIN;
   if (haiku_modifiers & HAIKU_MOD_CAPS) ret |= ECORE_EVENT_LOCK_CAPS;
   if (haiku_modifiers & HAIKU_MOD_NUM) ret |= ECORE_EVENT_LOCK_NUM;
   if (haiku_modifiers & HAIKU_MOD_SCROLL) ret |= ECORE_EVENT_LOCK_SCROLL;
   return ret;
}

/* ------------------------------------------------------------------ */
/* the buffer evas renders into                                        */

static void *
_ecore_evas_haiku_switch_buffer(void *data, void *dest EINA_UNUSED)
{
   Ecore_Evas *ee = data;
   Ecore_Evas_Haiku_Data *hd = (Ecore_Evas_Haiku_Data *)(ee + 1);

   haiku_window_present(hd->win, hd->pixels, hd->w, hd->h, hd->pitch);
   return hd->pixels;
}

/* (Re)allocate the buffer for a w x h canvas and tell the buffer engine. */
static Eina_Bool
_ecore_evas_haiku_buffer_reset(Ecore_Evas *ee, int w, int h)
{
   Ecore_Evas_Haiku_Data *hd = (Ecore_Evas_Haiku_Data *)(ee + 1);
   Evas_Engine_Info_Buffer *einfo;
   void *pixels;

   einfo = (Evas_Engine_Info_Buffer *)evas_engine_info_get(ee->evas);
   if (!einfo) return EINA_FALSE;

   pixels = calloc(h, w * 4);
   if (!pixels) return EINA_FALSE;
   free(hd->pixels);
   hd->pixels = pixels;
   hd->pitch = w * 4;
   hd->w = w;
   hd->h = h;

   einfo->info.depth_type = EVAS_ENGINE_BUFFER_DEPTH_RGB32;
   einfo->info.switch_data = ee;
   einfo->info.dest_buffer = hd->pixels;
   einfo->info.dest_buffer_row_bytes = hd->pitch;
   einfo->info.use_color_key = 0;
   einfo->info.alpha_threshold = 0;
   einfo->info.func.new_update_region = NULL;
   einfo->info.func.free_update_region = NULL;
   einfo->info.func.switch_buffer = _ecore_evas_haiku_switch_buffer;
   return evas_engine_info_set(ee->evas, (Evas_Engine_Info *)einfo);
}

/* the canvas got the size w x h: the window already has it */
static void
_ecore_evas_haiku_canvas_resize(Ecore_Evas *ee, int w, int h)
{
   if ((w == ee->w) && (h == ee->h)) return;

   ee->req.w = w;
   ee->req.h = h;
   ee->w = w;
   ee->h = h;
   if (!_ecore_evas_haiku_buffer_reset(ee, w, h)) return;

   evas_output_size_set(ee->evas, w, h);
   evas_output_viewport_set(ee->evas, 0, 0, w, h);
   evas_damage_rectangle_add(ee->evas, 0, 0, w, h);
   if (ee->func.fn_resize) ee->func.fn_resize(ee);
}

/* ------------------------------------------------------------------ */
/* events                                                              */

static void
_mouse_fill(Ecore_Evas *ee, Ecore_Event_Mouse_Button *ev, const Haiku_Event *he)
{
   ev->timestamp = _timestamp_get();
   ev->window = ev->event_window = ee->prop.window;
   ev->modifiers = _modifiers_get(he->modifiers);
   ev->x = he->x;
   ev->y = he->y;
   ev->root.x = he->x + ee->x;
   ev->root.y = he->y + ee->y;
   /* the multi touch device must be 0 or the event is ignored */
   ev->multi.device = 0;
}

static void
_mouse_button_send(Ecore_Evas *ee, const Haiku_Event *he, Eina_Bool down)
{
   Ecore_Event_Mouse_Button *ev = calloc(1, sizeof(Ecore_Event_Mouse_Button));

   if (!ev) return;
   _mouse_fill(ee, ev, he);
   ev->buttons = he->button;
   ev->double_click = down && (he->clicks == 2);
   ev->triple_click = down && (he->clicks >= 3);
   ecore_event_add(down ? ECORE_EVENT_MOUSE_BUTTON_DOWN : ECORE_EVENT_MOUSE_BUTTON_UP,
                   ev, NULL, NULL);
}

static void
_mouse_move_send(Ecore_Evas *ee, const Haiku_Event *he)
{
   Ecore_Event_Mouse_Move *ev = calloc(1, sizeof(Ecore_Event_Mouse_Move));

   if (!ev) return;
   ev->timestamp = _timestamp_get();
   ev->window = ev->event_window = ee->prop.window;
   ev->modifiers = _modifiers_get(he->modifiers);
   ev->x = he->x;
   ev->y = he->y;
   ev->root.x = he->x + ee->x;
   ev->root.y = he->y + ee->y;
   ev->multi.device = 0;
   ecore_event_add(ECORE_EVENT_MOUSE_MOVE, ev, NULL, NULL);
}

static void
_mouse_inout_send(Ecore_Evas *ee, const Haiku_Event *he, Eina_Bool in)
{
   Ecore_Event_Mouse_IO *ev = calloc(1, sizeof(Ecore_Event_Mouse_IO));

   if (!ev) return;
   ev->timestamp = _timestamp_get();
   ev->window = ev->event_window = ee->prop.window;
   ev->modifiers = _modifiers_get(he->modifiers);
   ev->x = he->x;
   ev->y = he->y;
   ecore_event_add(in ? ECORE_EVENT_MOUSE_IN : ECORE_EVENT_MOUSE_OUT, ev, NULL, NULL);
}

static void
_wheel_send(Ecore_Evas *ee, const Haiku_Event *he)
{
   Ecore_Event_Mouse_Wheel *ev = calloc(1, sizeof(Ecore_Event_Mouse_Wheel));
   float amount;

   if (!ev) return;
   ev->timestamp = _timestamp_get();
   ev->window = ev->event_window = ee->prop.window;
   ev->modifiers = _modifiers_get(he->modifiers);
   ev->x = he->x;
   ev->y = he->y;
   ev->root.x = he->x + ee->x;
   ev->root.y = he->y + ee->y;
   /* direction: 0 vertical, 1 horizontal; positive is down and right, like
    * Haiku's wheel deltas. Always move by at least one step. */
   ev->direction = (he->dy == 0.0f) && (he->dx != 0.0f);
   amount = ev->direction ? he->dx : he->dy;
   ev->z = (amount > 0) ? (int)(amount + 0.999f) : -(int)(-amount + 0.999f);
   ecore_event_add(ECORE_EVENT_MOUSE_WHEEL, ev, NULL, NULL);
}

static void
_key_send(Ecore_Evas *ee, const Haiku_Event *he, Eina_Bool down)
{
   Ecore_Event_Key *ev;
   size_t name_len = strlen(he->keyname), text_len = strlen(he->text);
   char *buf;

   /* keys without a name are not keys applications know */
   if (!name_len) return;

   /* the name and the text live right behind the event, so that a plain
    * free() releases all of it */
   ev = calloc(1, sizeof(Ecore_Event_Key) + name_len + 1 + text_len + 1);
   if (!ev) return;

   buf = (char *)(ev + 1);
   memcpy(buf, he->keyname, name_len + 1);
   ev->keyname = buf;
   ev->key = buf;
   if (text_len)
     {
        memcpy(buf + name_len + 1, he->text, text_len + 1);
        ev->string = buf + name_len + 1;
     }
   ev->compose = NULL;
   ev->keycode = he->keycode;
   ev->timestamp = _timestamp_get();
   ev->window = ev->event_window = ee->prop.window;
   ev->modifiers = _modifiers_get(he->modifiers);
   ecore_event_add(down ? ECORE_EVENT_KEY_DOWN : ECORE_EVENT_KEY_UP, ev, NULL, NULL);
}

static Eina_Bool
_ecore_evas_haiku_events(void *data, Ecore_Fd_Handler *fd_handler EINA_UNUSED)
{
   Ecore_Evas *ee = data;
   Ecore_Evas_Haiku_Data *hd = (Ecore_Evas_Haiku_Data *)(ee + 1);
   Haiku_Event he;
   Eina_Bool resized = EINA_FALSE;
   int new_w = 0, new_h = 0;

   while (haiku_window_event_get(hd->win, &he))
     {
        switch (he.type)
          {
           case HAIKU_EVENT_RESIZE:
             /* a drag produces many: only the last size matters */
             resized = EINA_TRUE;
             new_w = he.w;
             new_h = he.h;
             break;
           case HAIKU_EVENT_MOVE:
             if ((ee->x != he.x) || (ee->y != he.y))
               {
                  ee->req.x = ee->x = he.x;
                  ee->req.y = ee->y = he.y;
                  if (ee->func.fn_move) ee->func.fn_move(ee);
               }
             break;
           case HAIKU_EVENT_MOUSE_DOWN: _mouse_button_send(ee, &he, EINA_TRUE); break;
           case HAIKU_EVENT_MOUSE_UP: _mouse_button_send(ee, &he, EINA_FALSE); break;
           case HAIKU_EVENT_MOUSE_MOVE: _mouse_move_send(ee, &he); break;
           case HAIKU_EVENT_MOUSE_IN: _mouse_inout_send(ee, &he, EINA_TRUE); break;
           case HAIKU_EVENT_MOUSE_OUT: _mouse_inout_send(ee, &he, EINA_FALSE); break;
           case HAIKU_EVENT_WHEEL: _wheel_send(ee, &he); break;
           case HAIKU_EVENT_KEY_DOWN: _key_send(ee, &he, EINA_TRUE); break;
           case HAIKU_EVENT_KEY_UP: _key_send(ee, &he, EINA_FALSE); break;
           case HAIKU_EVENT_FOCUS:
             _ecore_evas_focus_device_set(ee, NULL, he.flag ? EINA_TRUE : EINA_FALSE);
             break;
           case HAIKU_EVENT_CLOSE:
             if (ee->func.fn_delete_request) ee->func.fn_delete_request(ee);
             break;
          }
     }

   if (resized) _ecore_evas_haiku_canvas_resize(ee, new_w, new_h);
   return ECORE_CALLBACK_RENEW;
}

/* ------------------------------------------------------------------ */
/* the clipboard                                                       */

/* The copy and paste buffer is the clipboard of the system, for text. The
 * primary selection stays inside the process. */
static Ecore_Evas_Selection_Callbacks _sel_cbs[ECORE_EVAS_SELECTION_BUFFER_LAST];
static unsigned int _sel_seat = 0;

static Eina_Bool
_sel_is_text(const char *type)
{
   return type && (!strcmp(type, "text/plain;charset=utf-8") ||
                   !strcmp(type, "text/plain"));
}

/* Hand the content to the clipboard. This cannot be done from inside the
 * claim: ecore_evas only stores the content once the claim returned, so the
 * delivery callback would find nothing. */
static void
_clipboard_push(void *data)
{
   Ecore_Evas *ee = data;
   Ecore_Evas_Selection_Callbacks *cbs =
     &_sel_cbs[ECORE_EVAS_SELECTION_BUFFER_COPY_AND_PASTE_BUFFER];
   const char *type = NULL;
   Eina_Rw_Slice slice;
   unsigned int i;

   _clipboard_job = NULL;
   if (!cbs->delivery || !cbs->available_types) return;

   for (i = 0; !type && (i < eina_array_count_get(cbs->available_types)); i++)
     {
        const char *t = eina_array_data_get(cbs->available_types, i);

        if (_sel_is_text(t)) type = t;
     }
   if (!type) return;

   if (cbs->delivery(ee, _sel_seat,
                     ECORE_EVAS_SELECTION_BUFFER_COPY_AND_PASTE_BUFFER,
                     type, &slice) && slice.mem)
     {
        /* the slice includes the terminating NUL of the text */
        size_t len = slice.len;

        if (len && (((char *)slice.mem)[len - 1] == '\0')) len--;
        haiku_clipboard_text_set(slice.mem, len);
        free(slice.mem);
     }
}

static Eina_Bool
_ecore_evas_haiku_selection_claim(Ecore_Evas *ee, unsigned int seat, Ecore_Evas_Selection_Buffer selection, Eina_Array *available_types, Ecore_Evas_Selection_Internal_Delivery delivery, Ecore_Evas_Selection_Internal_Cancel cancel)
{
   Ecore_Evas_Selection_Callbacks *cbs = &_sel_cbs[selection];

   if (cbs->cancel) cbs->cancel(ee, _sel_seat, selection);
   if (cbs->available_types) eina_array_free(cbs->available_types);

   cbs->delivery = delivery;
   cbs->cancel = cancel;
   cbs->available_types = available_types;
   _sel_seat = seat;

   if ((selection == ECORE_EVAS_SELECTION_BUFFER_COPY_AND_PASTE_BUFFER) && delivery)
     {
        if (_clipboard_job) ecore_job_del(_clipboard_job);
        _clipboard_job = ecore_job_add(_clipboard_push, ee);
     }

   if (ee->func.fn_selection_changed)
     ee->func.fn_selection_changed(ee, seat, selection);
   return EINA_TRUE;
}

static Eina_Bool
_ecore_evas_haiku_selection_has_owner(Ecore_Evas *ee EINA_UNUSED, unsigned int seat EINA_UNUSED, Ecore_Evas_Selection_Buffer selection)
{
   if (selection == ECORE_EVAS_SELECTION_BUFFER_COPY_AND_PASTE_BUFFER)
     return haiku_clipboard_has_text();
   return EINA_FALSE;
}

static Eina_Future *
_ecore_evas_haiku_selection_request(Ecore_Evas *ee, unsigned int seat, Ecore_Evas_Selection_Buffer selection, Eina_Array *acceptable_types)
{
   Ecore_Evas_Selection_Callbacks *cbs = &_sel_cbs[selection];
   Eina_Content *content = NULL;
   const char *type = NULL;
   Eina_Value value;
   unsigned int i, j;

   if (selection == ECORE_EVAS_SELECTION_BUFFER_COPY_AND_PASTE_BUFFER)
     {
        for (i = 0; !type && (i < eina_array_count_get(acceptable_types)); i++)
          {
             const char *t = eina_array_data_get(acceptable_types, i);

             if (_sel_is_text(t)) type = t;
          }
        if (type && haiku_clipboard_has_text())
          {
             size_t len;
             char *text = haiku_clipboard_text_get(&len);

             if (text)
               {
                  Eina_Slice slice = { .len = len + 1, .mem = text };

                  content = eina_content_new(slice, type);
                  free(text);
               }
          }
     }

   /* in-process buffer: the primary selection, or an empty clipboard */
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

/* ------------------------------------------------------------------ */
/* the engine                                                          */

static int
_ecore_evas_haiku_init(void)
{
   _ecore_evas_init_count++;
   if (_ecore_evas_init_count == 1) ecore_event_evas_init();
   return _ecore_evas_init_count;
}

static int
_ecore_evas_haiku_shutdown(void)
{
   _ecore_evas_init_count--;
   if (_ecore_evas_init_count == 0) ecore_event_evas_shutdown();
   if (_ecore_evas_init_count < 0) _ecore_evas_init_count = 0;
   return _ecore_evas_init_count;
}

static void
_ecore_evas_haiku_free(Ecore_Evas *ee)
{
   Ecore_Evas_Haiku_Data *hd = (Ecore_Evas_Haiku_Data *)(ee + 1);

   ecore_event_window_unregister(ee->prop.window);
   if (_clipboard_job)
     {
        ecore_job_del(_clipboard_job);
        _clipboard_job = NULL;
     }
   if (hd->fd_handler) ecore_main_fd_handler_del(hd->fd_handler);
   haiku_window_free(hd->win);
   free(hd->pixels);

   _ecore_evas_haiku_shutdown();
   _ecore_evas_haiku_count--;
}

static void
_ecore_evas_haiku_move(Ecore_Evas *ee, int x, int y)
{
   Ecore_Evas_Haiku_Data *hd = (Ecore_Evas_Haiku_Data *)(ee + 1);

   if ((ee->x == x) && (ee->y == y)) return;
   ee->req.x = ee->x = x;
   ee->req.y = ee->y = y;
   haiku_window_move(hd->win, x, y);
   if (ee->func.fn_move) ee->func.fn_move(ee);
}

static void
_ecore_evas_haiku_resize(Ecore_Evas *ee, int w, int h)
{
   Ecore_Evas_Haiku_Data *hd = (Ecore_Evas_Haiku_Data *)(ee + 1);

   if (w < 1) w = 1;
   if (h < 1) h = 1;
   if ((w == ee->w) && (h == ee->h)) return;
   haiku_window_resize(hd->win, w, h);
   _ecore_evas_haiku_canvas_resize(ee, w, h);
}

static void
_ecore_evas_haiku_move_resize(Ecore_Evas *ee, int x, int y, int w, int h)
{
   _ecore_evas_haiku_move(ee, x, y);
   _ecore_evas_haiku_resize(ee, w, h);
}

static void
_ecore_evas_haiku_show(Ecore_Evas *ee)
{
   Ecore_Evas_Haiku_Data *hd = (Ecore_Evas_Haiku_Data *)(ee + 1);

   haiku_window_show(hd->win);
   evas_damage_rectangle_add(ee->evas, 0, 0, ee->w, ee->h);
   ee->prop.withdrawn = EINA_FALSE;
   if (ee->func.fn_state_change) ee->func.fn_state_change(ee);
}

static void
_ecore_evas_haiku_hide(Ecore_Evas *ee)
{
   Ecore_Evas_Haiku_Data *hd = (Ecore_Evas_Haiku_Data *)(ee + 1);

   haiku_window_hide(hd->win);
   ee->prop.withdrawn = EINA_TRUE;
   if (ee->func.fn_state_change) ee->func.fn_state_change(ee);
}

static void
_ecore_evas_haiku_raise(Ecore_Evas *ee)
{
   haiku_window_raise(((Ecore_Evas_Haiku_Data *)(ee + 1))->win);
}

static void
_ecore_evas_haiku_lower(Ecore_Evas *ee)
{
   haiku_window_lower(((Ecore_Evas_Haiku_Data *)(ee + 1))->win);
}

static void
_ecore_evas_haiku_activate(Ecore_Evas *ee)
{
   haiku_window_activate(((Ecore_Evas_Haiku_Data *)(ee + 1))->win);
}

static void
_ecore_evas_haiku_title_set(Ecore_Evas *ee, const char *title)
{
   Ecore_Evas_Haiku_Data *hd = (Ecore_Evas_Haiku_Data *)(ee + 1);

   if (eina_streq(ee->prop.title, title)) return;
   free(ee->prop.title);
   ee->prop.title = eina_strdup(title);
   haiku_window_title_set(hd->win, title);
}

static void
_ecore_evas_haiku_size_limits_apply(Ecore_Evas *ee)
{
   Ecore_Evas_Haiku_Data *hd = (Ecore_Evas_Haiku_Data *)(ee + 1);

   haiku_window_size_limits_set(hd->win, ee->prop.min.w, ee->prop.min.h,
                                ee->prop.max.w, ee->prop.max.h);
}

static void
_ecore_evas_haiku_size_min_set(Ecore_Evas *ee, int w, int h)
{
   if (w < 0) w = 0;
   if (h < 0) h = 0;
   if ((ee->prop.min.w == w) && (ee->prop.min.h == h)) return;
   ee->prop.min.w = w;
   ee->prop.min.h = h;
   _ecore_evas_haiku_size_limits_apply(ee);
}

static void
_ecore_evas_haiku_size_max_set(Ecore_Evas *ee, int w, int h)
{
   if (w < 0) w = 0;
   if (h < 0) h = 0;
   if ((ee->prop.max.w == w) && (ee->prop.max.h == h)) return;
   ee->prop.max.w = w;
   ee->prop.max.h = h;
   _ecore_evas_haiku_size_limits_apply(ee);
}

static Ecore_Evas_Engine_Func _ecore_haiku_engine_func =
{
   .fn_free = _ecore_evas_haiku_free,
   .fn_move = _ecore_evas_haiku_move,
   .fn_resize = _ecore_evas_haiku_resize,
   .fn_move_resize = _ecore_evas_haiku_move_resize,
   .fn_show = _ecore_evas_haiku_show,
   .fn_hide = _ecore_evas_haiku_hide,
   .fn_raise = _ecore_evas_haiku_raise,
   .fn_lower = _ecore_evas_haiku_lower,
   .fn_activate = _ecore_evas_haiku_activate,
   .fn_title_set = _ecore_evas_haiku_title_set,
   .fn_size_min_set = _ecore_evas_haiku_size_min_set,
   .fn_size_max_set = _ecore_evas_haiku_size_max_set,
   .fn_selection_claim = _ecore_evas_haiku_selection_claim,
   .fn_selection_has_owner = _ecore_evas_haiku_selection_has_owner,
   .fn_selection_request = _ecore_evas_haiku_selection_request,
};

EMODAPI Ecore_Evas *
ecore_evas_haiku_new_internal(const char *name, int w, int h)
{
   Ecore_Evas_Haiku_Data *hd;
   Ecore_Evas *ee;
   int rmethod;

   rmethod = evas_render_method_lookup("buffer");
   if (!rmethod) return NULL;

   if (w < 1) w = 1;
   if (h < 1) h = 1;

   ee = calloc(1, sizeof(Ecore_Evas) + sizeof(Ecore_Evas_Haiku_Data));
   if (!ee) return NULL;
   hd = (Ecore_Evas_Haiku_Data *)(ee + 1);

   ECORE_MAGIC_SET(ee, ECORE_MAGIC_EVAS);
   _ecore_evas_haiku_init();
   _ecore_evas_haiku_count++;

   ee->engine.func = (Ecore_Evas_Engine_Func *)&_ecore_haiku_engine_func;
   ee->driver = "haiku";
   if (name) ee->name = strdup(name);

   ee->visible = 1;
   ee->req.w = w;
   ee->req.h = h;
   ee->w = w;
   ee->h = h;
   ee->prop.layer = 0;
   ee->prop.borderless = EINA_FALSE;
   ee->prop.withdrawn = EINA_TRUE;
   ee->prop.sticky = EINA_FALSE;
   ee->alpha = EINA_FALSE;

   /* the native window is only used from the main thread */
   ee->can_async_render = EINA_FALSE;

   if (!ecore_evas_evas_new(ee, w, h))
     {
        ERR("Can not create Canvas.");
        goto on_error;
     }
   evas_output_method_set(ee->evas, rmethod);

   /* cascade the windows a little, the window manager does not do it for
    * windows that are placed explicitly */
   hd->win = haiku_window_new(name ? name : "EFL",
                              100 + 28 * (_ecore_evas_haiku_count - 1),
                              100 + 28 * (_ecore_evas_haiku_count - 1), w, h);
   if (!hd->win)
     {
        ERR("Can not create the Haiku window.");
        goto on_error;
     }
   ee->x = ee->req.x = 100 + 28 * (_ecore_evas_haiku_count - 1);
   ee->y = ee->req.y = ee->x;

   if (!_ecore_evas_haiku_buffer_reset(ee, w, h))
     {
        ERR("evas_engine_info_set() for engine '%s' failed.", ee->driver);
        goto on_error;
     }

   hd->fd_handler = ecore_main_fd_handler_add(haiku_window_fd_get(hd->win),
                                              ECORE_FD_READ,
                                              _ecore_evas_haiku_events, ee,
                                              NULL, NULL);
   ee->prop.window = (Ecore_Window)(uintptr_t)hd->win;

   ecore_evas_done(ee, EINA_FALSE);
   return ee;

 on_error:
   ecore_evas_free(ee);
   return NULL;
}
