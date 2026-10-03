#ifndef HAIKU_WINDOW_H
#define HAIKU_WINDOW_H

/* A plain C interface to the native Haiku window used by the ecore_evas
 * engine. The Haiku API is C++, so it lives in haiku_window.cpp.
 *
 * Threads: a BApplication runs in a thread of its own and every window has
 * its own thread (the BWindow looper). What happens to a window is turned
 * into events that are queued and signalled through a pipe, so that the
 * ecore main loop only has to poll a file descriptor. All the functions
 * below are to be called from the ecore main thread. */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _Haiku_Window Haiku_Window;

typedef enum
{
   HAIKU_EVENT_RESIZE,
   HAIKU_EVENT_MOVE,
   HAIKU_EVENT_MOUSE_DOWN,
   HAIKU_EVENT_MOUSE_UP,
   HAIKU_EVENT_MOUSE_MOVE,
   HAIKU_EVENT_MOUSE_IN,
   HAIKU_EVENT_MOUSE_OUT,
   HAIKU_EVENT_WHEEL,
   HAIKU_EVENT_KEY_DOWN,
   HAIKU_EVENT_KEY_UP,
   HAIKU_EVENT_FOCUS,
   HAIKU_EVENT_DND_ENTER,
   HAIKU_EVENT_DND_MOVE,
   HAIKU_EVENT_DND_LEAVE,
   HAIKU_EVENT_DND_DROP,
   HAIKU_EVENT_CLOSE
} Haiku_Event_Type;

/* keyboard modifiers and locks, as reported in Haiku_Event.modifiers */
#define HAIKU_MOD_SHIFT   (1 << 0)
#define HAIKU_MOD_CONTROL (1 << 1)
#define HAIKU_MOD_ALT     (1 << 2) /* the keys printed Alt */
#define HAIKU_MOD_SUPER   (1 << 3) /* the logo keys */
#define HAIKU_MOD_CAPS    (1 << 4)
#define HAIKU_MOD_NUM     (1 << 5)
#define HAIKU_MOD_SCROLL  (1 << 6)

/* mouse buttons, numbered like ecore does: 1 left, 2 middle, 3 right */
#define HAIKU_BUTTON(n)   (1 << ((n) - 1))

typedef struct
{
   Haiku_Event_Type type;
   int x, y;              /* position (mouse), or position of the window (move) */
   int w, h;              /* size (resize) */
   unsigned int buttons;  /* buttons that are down (mouse) */
   int button;            /* button that changed (mouse down and up), 1..3 */
   int clicks;            /* click count (mouse down) */
   unsigned int modifiers;
   int keycode;
   char keyname[32];      /* "a", "Return", "BackSpace", ... */
   char text[16];         /* what the key types, UTF-8, empty if nothing */
   int repeat;            /* the key down is an auto repeat */
   float dx, dy;          /* wheel, positive is down and right */
   int flag;              /* focus gained (1) or lost (0) */
   char *data;            /* drop: the text, UTF-8, which the receiver frees */
} Haiku_Event;

/* The window is created hidden, w and h are the size of its content. */
Haiku_Window *haiku_window_new(const char *title, int x, int y, int w, int h);
void haiku_window_free(Haiku_Window *win);

/* file descriptor to poll for input, and the events behind it */
int haiku_window_fd_get(Haiku_Window *win);
int haiku_window_event_get(Haiku_Window *win, Haiku_Event *event);

void haiku_window_title_set(Haiku_Window *win, const char *title);
void haiku_window_show(Haiku_Window *win);
void haiku_window_hide(Haiku_Window *win);
void haiku_window_move(Haiku_Window *win, int x, int y);
void haiku_window_resize(Haiku_Window *win, int w, int h);
void haiku_window_raise(Haiku_Window *win);
void haiku_window_lower(Haiku_Window *win);
void haiku_window_activate(Haiku_Window *win);
void haiku_window_size_limits_set(Haiku_Window *win, int min_w, int min_h, int max_w, int max_h);

/* The window size is base + n * step on each axis (terminals resize in whole
 * cells); 0 or 1 for a step means any size. */
void haiku_window_size_step_set(Haiku_Window *win, int base_w, int base_h, int step_w, int step_h);
void haiku_window_fullscreen_set(Haiku_Window *win, int on);
void haiku_window_maximized_set(Haiku_Window *win, int on);
void haiku_window_iconified_set(Haiku_Window *win, int on);
void haiku_window_borderless_set(Haiku_Window *win, int on);

/* Hide the pointer of the system over the window, when the canvas draws one
 * of its own. */
void haiku_window_cursor_visible_set(Haiku_Window *win, int visible);

/* size of the screen the window is on */
void haiku_window_screen_size_get(Haiku_Window *win, int *w, int *h);

/* Show a frame: copies w x h pixels (32 bits, pitch bytes per row) into the
 * window and redraws it. */
void haiku_window_present(Haiku_Window *win, const void *pixels, int w, int h, int pitch);

/* clipboard, text only; haiku_clipboard_text_get() returns malloc()ed,
 * NUL terminated text, or NULL */
void haiku_clipboard_text_set(const char *text, size_t len);
char *haiku_clipboard_text_get(size_t *len);
int haiku_clipboard_has_text(void);

#ifdef __cplusplus
}
#endif

#endif
