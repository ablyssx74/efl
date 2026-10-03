/* Native Haiku window for the ecore_evas "haiku" engine. See haiku_window.h.
 *
 * Nothing here uses OpenGL or SDL: frames are copied into a BBitmap and drawn
 * by the app server. */

#include <AppFileInfo.h>
#include <Application.h>
#include <Bitmap.h>
#include <Clipboard.h>
#include <Cursor.h>
#include <File.h>
#include <InterfaceDefs.h>
#include <Entry.h>
#include <Message.h>
#include <OS.h>
#include <Path.h>
#include <Screen.h>
#include <View.h>
#include <Window.h>
#include <image.h>

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <deque>
#include <string>

#include "haiku_window.h"

struct _Haiku_Window;
class HaikuView;
class HaikuWindow;

struct _Haiku_Window
{
   HaikuWindow *window;
   HaikuView *view;

   BBitmap *bitmap;
   sem_id bitmap_lock;

   std::deque<Haiku_Event> queue;
   sem_id queue_lock;
   int pipe_fds[2];

   /* fullscreen and maximized: what to go back to */
   bool fullscreen;
   bool zoomed;
   BRect saved_frame;
   BRect saved_zoom_frame;
   window_look saved_look;
   window_feel saved_feel;
};

/* ------------------------------------------------------------------ */
/* the application                                                     */

static sem_id _app_ready = -1;

static void
_app_signature_get(char *signature)
{
   /* the signature of the executable, so that the team is the application
    * its resources describe */
   int32 cookie = 0;
   image_info info;

   strcpy(signature, "application/x-vnd.EFL-application");
   while (get_next_image_info(0, &cookie, &info) == B_OK)
     {
        if (info.type != B_APP_IMAGE) continue;
        BFile file(info.name, B_READ_ONLY);
        BAppFileInfo app_info(&file);
        char found[B_MIME_TYPE_LENGTH];
        if ((file.InitCheck() == B_OK) &&
            (app_info.GetSignature(found) == B_OK) && found[0])
          strcpy(signature, found);
        break;
     }
}

static int32
_app_thread(void *)
{
   char signature[B_MIME_TYPE_LENGTH];

   _app_signature_get(signature);
   BApplication app(signature);
   release_sem(_app_ready);
   app.Run();
   return 0;
}

static bool
_app_ensure(void)
{
   if (be_app) return true;

   _app_ready = create_sem(0, "efl app ready");
   thread_id tid = spawn_thread(_app_thread, "efl app", B_NORMAL_PRIORITY, NULL);
   if (tid < 0) return false;
   resume_thread(tid);
   acquire_sem(_app_ready);
   delete_sem(_app_ready);
   return be_app != NULL;
}

/* ------------------------------------------------------------------ */
/* events                                                              */

static void
_post(Haiku_Window *win, const Haiku_Event &event)
{
   acquire_sem(win->queue_lock);
   /* a burst of pointer moves only needs its last position */
   if ((event.type == HAIKU_EVENT_MOUSE_MOVE) && !win->queue.empty() &&
       (win->queue.back().type == HAIKU_EVENT_MOUSE_MOVE) &&
       (win->queue.back().buttons == event.buttons))
     win->queue.back() = event;
   else if ((event.type == HAIKU_EVENT_DND_MOVE) && !win->queue.empty() &&
            (win->queue.back().type == HAIKU_EVENT_DND_MOVE))
     win->queue.back() = event;
   else
     win->queue.push_back(event);
   release_sem(win->queue_lock);

   char c = 1;
   write(win->pipe_fds[1], &c, 1);
}

/* The keymap decides which key is Command, Control and Option: with the
 * Linux layout of the Keymap preferences Command is on the key printed Ctrl
 * and Control on the one printed Alt. EFL applications expect Ctrl and Alt
 * where they are printed, so go by the key a modifier is assigned to. The
 * codes are those of the keys of a PC keyboard. */
static unsigned int
_modifier_of_key(uint32 key, unsigned int fallback)
{
   switch (key)
     {
      case 0x5c: case 0x60: return HAIKU_MOD_CONTROL;
      case 0x5d: case 0x5f: return HAIKU_MOD_ALT;
      case 0x66: case 0x67: return HAIKU_MOD_SUPER;
      default: return fallback;
     }
}

/* what the keys of one side of a modifier stand for, for a second at most:
 * the keymap can be changed while the application runs */
static unsigned int
_modifier_of_side(uint32 side, unsigned int fallback)
{
   static const uint32 sides[] = {
      B_LEFT_COMMAND_KEY, B_RIGHT_COMMAND_KEY, B_LEFT_CONTROL_KEY,
      B_RIGHT_CONTROL_KEY, B_LEFT_OPTION_KEY, B_RIGHT_OPTION_KEY
   };
   static unsigned int cache[sizeof(sides) / sizeof(sides[0])];
   static bigtime_t stamp = 0;
   unsigned int i;
   bigtime_t now = system_time();

   if ((now - stamp) > 1000000)
     {
        for (i = 0; i < sizeof(sides) / sizeof(sides[0]); i++)
          {
             uint32 key = 0;

             cache[i] = (get_modifier_key(sides[i], &key) == B_OK) ?
               _modifier_of_key(key, 0) : 0;
          }
        stamp = now;
     }
   for (i = 0; i < sizeof(sides) / sizeof(sides[0]); i++)
     if ((sides[i] == side) && cache[i]) return cache[i];
   return fallback;
}

static unsigned int
_modifiers_get(void)
{
   uint32 m = modifiers();
   unsigned int ret = 0;

   if (m & B_SHIFT_KEY) ret |= HAIKU_MOD_SHIFT;
   if (m & B_COMMAND_KEY)
     {
        if (m & B_LEFT_COMMAND_KEY)
          ret |= _modifier_of_side(B_LEFT_COMMAND_KEY, HAIKU_MOD_ALT);
        if (m & B_RIGHT_COMMAND_KEY)
          ret |= _modifier_of_side(B_RIGHT_COMMAND_KEY, HAIKU_MOD_ALT);
        if (!(m & (B_LEFT_COMMAND_KEY | B_RIGHT_COMMAND_KEY)))
          ret |= HAIKU_MOD_ALT;
     }
   if (m & B_CONTROL_KEY)
     {
        if (m & B_LEFT_CONTROL_KEY)
          ret |= _modifier_of_side(B_LEFT_CONTROL_KEY, HAIKU_MOD_CONTROL);
        if (m & B_RIGHT_CONTROL_KEY)
          ret |= _modifier_of_side(B_RIGHT_CONTROL_KEY, HAIKU_MOD_CONTROL);
        if (!(m & (B_LEFT_CONTROL_KEY | B_RIGHT_CONTROL_KEY)))
          ret |= HAIKU_MOD_CONTROL;
     }
   if (m & B_OPTION_KEY)
     {
        if (m & B_LEFT_OPTION_KEY)
          ret |= _modifier_of_side(B_LEFT_OPTION_KEY, HAIKU_MOD_SUPER);
        if (m & B_RIGHT_OPTION_KEY)
          ret |= _modifier_of_side(B_RIGHT_OPTION_KEY, HAIKU_MOD_SUPER);
        if (!(m & (B_LEFT_OPTION_KEY | B_RIGHT_OPTION_KEY)))
          ret |= HAIKU_MOD_SUPER;
     }
   if (m & B_CAPS_LOCK) ret |= HAIKU_MOD_CAPS;
   if (m & B_NUM_LOCK) ret |= HAIKU_MOD_NUM;
   if (m & B_SCROLL_LOCK) ret |= HAIKU_MOD_SCROLL;
   return ret;
}

/* A drag that can be dropped on the window: files, or text. */
static bool
_drag_accepted(const BMessage *drag)
{
   return drag && (drag->HasRef("refs") || drag->HasData("text/plain", B_MIME_TYPE));
}

/* What was dropped as text: the paths of the files one per line, or the
 * text itself. NULL if there is nothing the application can use. */
static char *
_dropped_text_get(const BMessage *message)
{
   std::string text;
   entry_ref ref;
   int32 i;

   for (i = 0; message->FindRef("refs", i, &ref) == B_OK; i++)
     {
        BPath path(&ref);

        if (path.InitCheck() != B_OK) continue;
        if (!text.empty()) text += '\n';
        text += path.Path();
     }
   if (text.empty())
     {
        const void *data;
        ssize_t len;

        if ((message->FindData("text/plain", B_MIME_TYPE, &data, &len) == B_OK) &&
            (len > 0))
          {
             text.assign(static_cast<const char *>(data), len);
             while (!text.empty() && (text[text.size() - 1] == '\0'))
               text.erase(text.size() - 1);
          }
     }
   if (text.empty()) return NULL;
   return strdup(text.c_str());
}

/* Haiku numbers the buttons primary, secondary, tertiary; ecore left,
 * middle, right */
static unsigned int
_buttons_get(uint32 haiku_buttons)
{
   unsigned int ret = 0;

   if (haiku_buttons & B_PRIMARY_MOUSE_BUTTON) ret |= HAIKU_BUTTON(1);
   if (haiku_buttons & B_TERTIARY_MOUSE_BUTTON) ret |= HAIKU_BUTTON(2);
   if (haiku_buttons & B_SECONDARY_MOUSE_BUTTON) ret |= HAIKU_BUTTON(3);
   return ret;
}

static const struct { char c; const char *name; } _punctuation[] = {
   { ' ', "space" }, { '!', "exclam" }, { '"', "quotedbl" },
   { '#', "numbersign" }, { '$', "dollar" }, { '%', "percent" },
   { '&', "ampersand" }, { '\'', "apostrophe" }, { '(', "parenleft" },
   { ')', "parenright" }, { '*', "asterisk" }, { '+', "plus" },
   { ',', "comma" }, { '-', "minus" }, { '.', "period" }, { '/', "slash" },
   { ':', "colon" }, { ';', "semicolon" }, { '<', "less" },
   { '=', "equal" }, { '>', "greater" }, { '?', "question" }, { '@', "at" },
   { '[', "bracketleft" }, { '\\', "backslash" }, { ']', "bracketright" },
   { '^', "asciicircum" }, { '_', "underscore" }, { '`', "grave" },
   { '{', "braceleft" }, { '|', "bar" }, { '}', "braceright" },
   { '~', "asciitilde" }
};

/* the name of a key, as X names its keysyms, which is what applications
 * look for */
static void
_key_name_get(int32 raw_char, int32 key, char *name, size_t size)
{
   const char *special = NULL;

   switch (raw_char)
     {
      case B_ENTER: special = "Return"; break;
      case B_BACKSPACE: special = "BackSpace"; break;
      case B_TAB: special = "Tab"; break;
      case B_ESCAPE: special = "Escape"; break;
      case B_DELETE: special = "Delete"; break;
      case B_LEFT_ARROW: special = "Left"; break;
      case B_RIGHT_ARROW: special = "Right"; break;
      case B_UP_ARROW: special = "Up"; break;
      case B_DOWN_ARROW: special = "Down"; break;
      case B_HOME: special = "Home"; break;
      case B_END: special = "End"; break;
      case B_PAGE_UP: special = "Prior"; break;
      case B_PAGE_DOWN: special = "Next"; break;
      case B_INSERT: special = "Insert"; break;
      default: break;
     }
   if (special)
     {
        strlcpy(name, special, size);
        return;
     }

   if (raw_char == B_FUNCTION_KEY)
     {
        if ((key >= B_F1_KEY) && (key <= B_F12_KEY))
          snprintf(name, size, "F%d", key - B_F1_KEY + 1);
        else if (key == B_PRINT_KEY) strlcpy(name, "Print", size);
        else if (key == B_SCROLL_KEY) strlcpy(name, "Scroll_Lock", size);
        else if (key == B_PAUSE_KEY) strlcpy(name, "Pause", size);
        else name[0] = '\0';
        return;
     }

   if ((raw_char > 0) && (raw_char < 0x80))
     {
        if (isalnum(raw_char))
          {
             name[0] = tolower(raw_char);
             name[1] = '\0';
             return;
          }
        for (size_t i = 0; i < sizeof(_punctuation) / sizeof(_punctuation[0]); i++)
          if (_punctuation[i].c == raw_char)
            {
               strlcpy(name, _punctuation[i].name, size);
               return;
            }
        name[0] = '\0';
        return;
     }

   name[0] = '\0';
}

/* ------------------------------------------------------------------ */
/* the view and the window                                             */

class HaikuView : public BView
{
public:
   HaikuView(BRect frame, Haiku_Window *win)
     : BView(frame, "canvas", B_FOLLOW_ALL,
             B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE),
       fWin(win), fButtons(0), fDragging(false)
   {
      SetViewColor(B_TRANSPARENT_COLOR);
      SetLowColor(32, 32, 32);
   }

   void AttachedToWindow() { MakeFocus(true); }

   void Draw(BRect)
   {
      acquire_sem(fWin->bitmap_lock);
      if (fWin->bitmap)
        {
           BRect bounds = fWin->bitmap->Bounds();
           DrawBitmap(fWin->bitmap, bounds, bounds);
           /* the window can be larger than the last frame for a moment */
           BRect view = Bounds();
           if (view.right > bounds.right)
             FillRect(BRect(bounds.right + 1, view.top, view.right, view.bottom), B_SOLID_LOW);
           if (view.bottom > bounds.bottom)
             FillRect(BRect(view.left, bounds.bottom + 1, bounds.right, view.bottom), B_SOLID_LOW);
        }
      else
        FillRect(Bounds(), B_SOLID_LOW);
      release_sem(fWin->bitmap_lock);
   }

   void FrameResized(float width, float height)
   {
      Haiku_Event e;
      memset(&e, 0, sizeof(e));
      e.type = HAIKU_EVENT_RESIZE;
      e.w = (int)width + 1;
      e.h = (int)height + 1;
      _post(fWin, e);
   }

   void MouseDown(BPoint where)
   {
      BMessage *message = Window()->CurrentMessage();
      int32 haiku_buttons = 0, clicks = 1;
      message->FindInt32("buttons", &haiku_buttons);
      message->FindInt32("clicks", &clicks);
      unsigned int now = _buttons_get(haiku_buttons);
      unsigned int pressed = now & ~fButtons;
      fButtons = now;
      /* keep the events while the button is down, also outside the view */
      SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);

      Haiku_Event e;
      memset(&e, 0, sizeof(e));
      e.type = HAIKU_EVENT_MOUSE_DOWN;
      e.x = (int)where.x;
      e.y = (int)where.y;
      e.buttons = now;
      e.button = pressed & HAIKU_BUTTON(1) ? 1 : (pressed & HAIKU_BUTTON(2) ? 2 : 3);
      e.clicks = clicks;
      e.modifiers = _modifiers_get();
      _post(fWin, e);
   }

   void MouseUp(BPoint where)
   {
      /* the message of a button up has no buttons, only what is left down */
      int32 haiku_buttons = 0;
      Window()->CurrentMessage()->FindInt32("buttons", &haiku_buttons);
      unsigned int now = _buttons_get(haiku_buttons);
      unsigned int released = fButtons & ~now;
      fButtons = now;

      Haiku_Event e;
      memset(&e, 0, sizeof(e));
      e.type = HAIKU_EVENT_MOUSE_UP;
      e.x = (int)where.x;
      e.y = (int)where.y;
      e.buttons = now;
      e.button = released & HAIKU_BUTTON(1) ? 1 : (released & HAIKU_BUTTON(2) ? 2 : 3);
      e.modifiers = _modifiers_get();
      _post(fWin, e);
   }

   void MouseMoved(BPoint where, uint32 transit, const BMessage *drag)
   {
      Haiku_Event e;
      memset(&e, 0, sizeof(e));
      e.x = (int)where.x;
      e.y = (int)where.y;
      e.buttons = fButtons;
      e.modifiers = _modifiers_get();
      if (_drag_accepted(drag))
        {
           /* something is being dragged over the window */
           if (transit == B_EXITED_VIEW)
             {
                if (fDragging) e.type = HAIKU_EVENT_DND_LEAVE;
                else return;
                fDragging = false;
                _post(fWin, e);
                return;
             }
           if (!fDragging)
             {
                Haiku_Event enter = e;

                enter.type = HAIKU_EVENT_DND_ENTER;
                fDragging = true;
                _post(fWin, enter);
             }
           e.type = HAIKU_EVENT_DND_MOVE;
           _post(fWin, e);
           return;
        }
      if (transit == B_ENTERED_VIEW) e.type = HAIKU_EVENT_MOUSE_IN;
      else if (transit == B_EXITED_VIEW) e.type = HAIKU_EVENT_MOUSE_OUT;
      else e.type = HAIKU_EVENT_MOUSE_MOVE;
      _post(fWin, e);
   }

   void KeyDown(const char *bytes, int32 count) { _key(true, bytes, count); }
   void KeyUp(const char *bytes, int32 count) { _key(false, bytes, count); }

   void MessageReceived(BMessage *message)
   {
      if (message->what == B_MOUSE_WHEEL_CHANGED)
        {
           Haiku_Event e;
           memset(&e, 0, sizeof(e));
           e.type = HAIKU_EVENT_WHEEL;
           message->FindFloat("be:wheel_delta_x", &e.dx);
           message->FindFloat("be:wheel_delta_y", &e.dy);
           BPoint where;
           uint32 haiku_buttons;
           GetMouse(&where, &haiku_buttons, false);
           e.x = (int)where.x;
           e.y = (int)where.y;
           e.modifiers = _modifiers_get();
           _post(fWin, e);
           return;
        }
      if (message->WasDropped())
        {
           char *text = _dropped_text_get(message);

           if (text)
             {
                BPoint offset, where = message->DropPoint(&offset);
                Haiku_Event e;

                ConvertFromScreen(&where);
                memset(&e, 0, sizeof(e));
                e.type = HAIKU_EVENT_DND_DROP;
                e.x = (int)where.x;
                e.y = (int)where.y;
                e.modifiers = _modifiers_get();
                e.data = text;
                fDragging = false;
                _post(fWin, e);
                return;
             }
        }
      BView::MessageReceived(message);
   }

private:
   void _key(bool down, const char *bytes, int32 count)
   {
      BMessage *message = Window()->CurrentMessage();
      int32 raw_char = 0, key = 0, repeat = 0;
      message->FindInt32("raw_char", &raw_char);
      message->FindInt32("key", &key);
      message->FindInt32("be:key_repeat", &repeat);

      Haiku_Event e;
      memset(&e, 0, sizeof(e));
      e.type = down ? HAIKU_EVENT_KEY_DOWN : HAIKU_EVENT_KEY_UP;
      e.modifiers = _modifiers_get();
      e.keycode = key;
      e.repeat = repeat > 0;
      _key_name_get(raw_char, key, e.keyname, sizeof(e.keyname));

      /* text: what is printable, and the control character of ctrl + key
       * (like an X server sends), but nothing for the keys that only have a
       * name (Return, arrows, ...) */
      if ((count > 0) && (count < (int32)sizeof(e.text)))
        {
           unsigned char first = (unsigned char)bytes[0];
           bool named = (raw_char == B_ENTER) || (raw_char == B_BACKSPACE) ||
             (raw_char == B_TAB) || (raw_char == B_ESCAPE) ||
             (raw_char == B_DELETE) || (raw_char == B_FUNCTION_KEY) ||
             ((raw_char >= B_HOME) && (raw_char <= B_DOWN_ARROW) &&
              (raw_char != B_ENTER) && (raw_char != B_BACKSPACE) &&
              (raw_char != B_TAB) && (raw_char != B_ESCAPE));
           char text[sizeof(e.text)];
           int len = 0;

           if (!named)
             {
                if ((first < 0x20) && (modifiers() & B_CONTROL_KEY) &&
                    (raw_char >= 0x20) && (raw_char < 0x7f))
                  {
                     /* the keymap has Control where Alt is printed: the
                      * bytes are a control character, the key's own is
                      * what is wanted */
                     char c = (char)raw_char;

                     if (isalpha((unsigned char)c) &&
                         (((e.modifiers & HAIKU_MOD_SHIFT) != 0) !=
                          ((e.modifiers & HAIKU_MOD_CAPS) != 0)))
                       c = toupper((unsigned char)c);
                     text[len++] = c;
                  }
                else if ((first >= 0x20) && (first != 0x7f))
                  {
                     memcpy(text, bytes, count);
                     len = count;
                  }
             }
           /* ctrl + key gives the control character, as on an X server */
           if ((len == 1) && (e.modifiers & HAIKU_MOD_CONTROL))
             {
                unsigned char c = (unsigned char)text[0];

                if (((c >= '@') && (c <= '_')) || ((c >= 'a') && (c <= 'z')))
                  text[0] = c & 0x1f;
             }
           if (len > 0) memcpy(e.text, text, len);
        }
      _post(fWin, e);
   }

   Haiku_Window *fWin;
   unsigned int fButtons;
   bool fDragging;
};

class HaikuWindow : public BWindow
{
public:
   HaikuWindow(BRect frame, const char *title, Haiku_Window *win)
     : BWindow(frame, title, B_DOCUMENT_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL, 0),
       fWin(win)
   {
   }

   bool QuitRequested()
   {
      Haiku_Event e;
      memset(&e, 0, sizeof(e));
      e.type = HAIKU_EVENT_CLOSE;
      _post(fWin, e);
      return false; /* the application decides */
   }

   /* BWindow keeps the key combinations with Command for its shortcuts and
    * does not give them to the view. The keymap can put Command on the key
    * printed Ctrl, and an application wants Ctrl+C like any other key. */
   void DispatchMessage(BMessage *message, BHandler *handler)
   {
      int32 mods = 0;
      const char *bytes = NULL;
      BView *focus = CurrentFocus();

      if ((message->what == B_KEY_DOWN) && focus &&
          (message->FindInt32("modifiers", &mods) == B_OK) &&
          (mods & B_COMMAND_KEY) &&
          (message->FindString("bytes", &bytes) == B_OK))
        {
           focus->KeyDown(bytes, strlen(bytes));
           return;
        }
      BWindow::DispatchMessage(message, handler);
   }

   void WindowActivated(bool active)
   {
      Haiku_Event e;
      memset(&e, 0, sizeof(e));
      e.type = HAIKU_EVENT_FOCUS;
      e.flag = active;
      _post(fWin, e);
   }

   void FrameMoved(BPoint origin)
   {
      Haiku_Event e;
      memset(&e, 0, sizeof(e));
      e.type = HAIKU_EVENT_MOVE;
      e.x = (int)origin.x;
      e.y = (int)origin.y;
      _post(fWin, e);
   }

private:
   Haiku_Window *fWin;
};

/* ------------------------------------------------------------------ */
/* the C interface                                                     */

extern "C" Haiku_Window *
haiku_window_new(const char *title, int x, int y, int w, int h)
{
   if (!_app_ensure()) return NULL;

   Haiku_Window *win = new Haiku_Window();
   win->bitmap = NULL;
   win->fullscreen = false;
   win->zoomed = false;
   win->bitmap_lock = create_sem(1, "efl bitmap");
   win->queue_lock = create_sem(1, "efl event queue");
   if (pipe(win->pipe_fds) != 0)
     {
        delete win;
        return NULL;
     }
   for (int i = 0; i < 2; i++)
     {
        fcntl(win->pipe_fds[i], F_SETFL, fcntl(win->pipe_fds[i], F_GETFL) | O_NONBLOCK);
        fcntl(win->pipe_fds[i], F_SETFD, FD_CLOEXEC);
     }

   if (w < 1) w = 1;
   if (h < 1) h = 1;
   win->window = new HaikuWindow(BRect(x, y, x + w - 1, y + h - 1), title ? title : "", win);
   win->view = new HaikuView(win->window->Bounds(), win);
   win->window->AddChild(win->view);
   return win;
}

extern "C" void
haiku_window_free(Haiku_Window *win)
{
   if (!win) return;
   if (win->window->Lock()) win->window->Quit(); /* deletes the window */
   for (size_t i = 0; i < win->queue.size(); i++) free(win->queue[i].data);
   close(win->pipe_fds[0]);
   close(win->pipe_fds[1]);
   delete_sem(win->queue_lock);
   delete_sem(win->bitmap_lock);
   delete win->bitmap;
   delete win;
}

extern "C" int
haiku_window_fd_get(Haiku_Window *win)
{
   return win->pipe_fds[0];
}

extern "C" int
haiku_window_event_get(Haiku_Window *win, Haiku_Event *event)
{
   int got = 0;

   acquire_sem(win->queue_lock);
   if (!win->queue.empty())
     {
        *event = win->queue.front();
        win->queue.pop_front();
        got = 1;
     }
   else
     {
        /* nothing left: forget the wake-ups, the events come first so one
         * posted right after is not missed */
        char buf[256];
        while (read(win->pipe_fds[0], buf, sizeof(buf)) > 0) { }
     }
   release_sem(win->queue_lock);
   return got;
}

extern "C" void
haiku_window_title_set(Haiku_Window *win, const char *title)
{
   if (win->window->Lock())
     {
        win->window->SetTitle(title ? title : "");
        win->window->Unlock();
     }
}

extern "C" void
haiku_window_show(Haiku_Window *win)
{
   if (win->window->IsHidden()) win->window->Show();
}

extern "C" void
haiku_window_hide(Haiku_Window *win)
{
   if (!win->window->IsHidden()) win->window->Hide();
}

extern "C" void
haiku_window_move(Haiku_Window *win, int x, int y)
{
   if (win->window->Lock())
     {
        win->window->MoveTo(x, y);
        win->window->Unlock();
     }
}

extern "C" void
haiku_window_resize(Haiku_Window *win, int w, int h)
{
   if (win->window->Lock())
     {
        win->window->ResizeTo(w - 1, h - 1);
        win->window->Unlock();
     }
}

extern "C" void
haiku_window_raise(Haiku_Window *win)
{
   if (win->window->Lock())
     {
        win->window->Activate(true);
        win->window->Unlock();
     }
}

extern "C" void
haiku_window_lower(Haiku_Window *win)
{
   if (win->window->Lock())
     {
        win->window->SendBehind(NULL);
        win->window->Unlock();
     }
}

extern "C" void
haiku_window_activate(Haiku_Window *win)
{
   haiku_window_raise(win);
}

extern "C" void
haiku_window_size_limits_set(Haiku_Window *win, int min_w, int min_h, int max_w, int max_h)
{
   if (win->window->Lock())
     {
        win->window->SetSizeLimits(min_w > 0 ? min_w - 1 : 0,
                                   max_w > 0 ? max_w - 1 : B_SIZE_UNLIMITED,
                                   min_h > 0 ? min_h - 1 : 0,
                                   max_h > 0 ? max_h - 1 : B_SIZE_UNLIMITED);
        win->window->Unlock();
     }
}

extern "C" void
haiku_window_size_step_set(Haiku_Window *win, int base_w, int base_h, int step_w, int step_h)
{
   if (!win->window->Lock()) return;
   if ((step_w <= 1) && (step_h <= 1))
     win->window->SetWindowAlignment(B_PIXEL_ALIGNMENT, 1, 0, 1, 0, 1, 0, 1, 0);
   else
     {
        /* Haiku sizes are coordinates: a content of n pixels is n - 1 */
        if (step_w < 1) step_w = 1;
        if (step_h < 1) step_h = 1;
        int off_w = (((base_w - 1) % step_w) + step_w) % step_w;
        int off_h = (((base_h - 1) % step_h) + step_h) % step_h;
        win->window->SetWindowAlignment(B_PIXEL_ALIGNMENT, 1, 0, step_w, off_w,
                                        1, 0, step_h, off_h);
     }
   win->window->Unlock();
}

extern "C" void
haiku_window_fullscreen_set(Haiku_Window *win, int on)
{
   if (!win->window->Lock()) return;
   if (on && !win->fullscreen)
     {
        win->saved_frame = win->window->Frame();
        win->saved_look = win->window->Look();
        win->saved_feel = win->window->Feel();
        BRect screen = BScreen(win->window).Frame();
        win->window->SetLook(B_NO_BORDER_WINDOW_LOOK);
        win->window->MoveTo(screen.left, screen.top);
        win->window->ResizeTo(screen.Width(), screen.Height());
        win->fullscreen = true;
     }
   else if (!on && win->fullscreen)
     {
        win->window->SetLook(win->saved_look);
        win->window->SetFeel(win->saved_feel);
        win->window->MoveTo(win->saved_frame.left, win->saved_frame.top);
        win->window->ResizeTo(win->saved_frame.Width(), win->saved_frame.Height());
        win->fullscreen = false;
     }
   win->window->Unlock();
}

extern "C" void
haiku_window_maximized_set(Haiku_Window *win, int on)
{
   if (!win->window->Lock()) return;
   if (on && !win->zoomed)
     {
        /* Zoom() only moves a window that has no size limits, so fill the
         * screen by hand, leaving room for the border and the title tab */
        win->saved_zoom_frame = win->window->Frame();
        BRect screen = BScreen(win->window).Frame();
        BRect frame = win->window->Frame();
        BRect decorator = win->window->DecoratorFrame();
        float left = frame.left - decorator.left;
        float top = frame.top - decorator.top;
        float right = decorator.right - frame.right;
        float bottom = decorator.bottom - frame.bottom;
        win->window->MoveTo(screen.left + left, screen.top + top);
        win->window->ResizeTo(screen.Width() - left - right,
                              screen.Height() - top - bottom);
        win->zoomed = true;
     }
   else if (!on && win->zoomed)
     {
        win->window->MoveTo(win->saved_zoom_frame.left, win->saved_zoom_frame.top);
        win->window->ResizeTo(win->saved_zoom_frame.Width(), win->saved_zoom_frame.Height());
        win->zoomed = false;
     }
   win->window->Unlock();
}

extern "C" void
haiku_window_iconified_set(Haiku_Window *win, int on)
{
   if (win->window->Lock())
     {
        win->window->Minimize(on != 0);
        win->window->Unlock();
     }
}

extern "C" void
haiku_window_borderless_set(Haiku_Window *win, int on)
{
   if (win->window->Lock())
     {
        win->window->SetLook(on ? B_NO_BORDER_WINDOW_LOOK : B_DOCUMENT_WINDOW_LOOK);
        win->window->Unlock();
     }
}

extern "C" void
haiku_window_cursor_visible_set(Haiku_Window *win, int visible)
{
   if (win->window->LockLooper())
     {
        BCursor cursor(visible ? B_CURSOR_ID_SYSTEM_DEFAULT : B_CURSOR_ID_NO_CURSOR);
        win->view->SetViewCursor(&cursor);
        win->window->UnlockLooper();
     }
}

extern "C" void
haiku_window_screen_size_get(Haiku_Window *win, int *w, int *h)
{
   BRect frame = BScreen(win->window).Frame();

   if (w) *w = (int)frame.Width() + 1;
   if (h) *h = (int)frame.Height() + 1;
}

extern "C" void
haiku_window_present(Haiku_Window *win, const void *pixels, int w, int h, int pitch)
{
   acquire_sem(win->bitmap_lock);
   if (!win->bitmap ||
       ((int)win->bitmap->Bounds().Width() + 1 != w) ||
       ((int)win->bitmap->Bounds().Height() + 1 != h))
     {
        delete win->bitmap;
        win->bitmap = new BBitmap(BRect(0, 0, w - 1, h - 1), B_RGB32);
     }
   const uint8 *src = (const uint8 *)pixels;
   uint8 *dst = (uint8 *)win->bitmap->Bits();
   int dst_pitch = win->bitmap->BytesPerRow();
   int row_bytes = w * 4;
   for (int y = 0; y < h; y++)
     memcpy(dst + y * dst_pitch, src + y * pitch, row_bytes);
   release_sem(win->bitmap_lock);

   if (win->window->LockLooper())
     {
        win->view->Invalidate();
        win->window->UnlockLooper();
     }
}

extern "C" void
haiku_clipboard_text_set(const char *text, size_t len)
{
   if (!be_clipboard || !be_clipboard->Lock()) return;
   be_clipboard->Clear();
   BMessage *data = be_clipboard->Data();
   if (data)
     {
        data->AddData("text/plain", B_MIME_TYPE, text, len);
        be_clipboard->Commit();
     }
   be_clipboard->Unlock();
}

extern "C" char *
haiku_clipboard_text_get(size_t *len)
{
   char *ret = NULL;

   if (len) *len = 0;
   if (!be_clipboard || !be_clipboard->Lock()) return NULL;
   BMessage *data = be_clipboard->Data();
   const char *text;
   ssize_t size;
   if (data && (data->FindData("text/plain", B_MIME_TYPE,
                               (const void **)&text, &size) == B_OK))
     {
        ret = (char *)malloc(size + 1);
        if (ret)
          {
             memcpy(ret, text, size);
             ret[size] = '\0';
             if (len) *len = size;
          }
     }
   be_clipboard->Unlock();
   return ret;
}

extern "C" int
haiku_clipboard_has_text(void)
{
   int has = 0;

   if (!be_clipboard || !be_clipboard->Lock()) return 0;
   BMessage *data = be_clipboard->Data();
   const char *text;
   ssize_t size;
   if (data && (data->FindData("text/plain", B_MIME_TYPE,
                               (const void **)&text, &size) == B_OK) && size > 0)
     has = 1;
   be_clipboard->Unlock();
   return has;
}
