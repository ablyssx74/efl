# EFL for Haiku: changelog

One entry per published package revision (`efl` and `efl_devel`, version
1.28.99), newest first. The tag of a revision is `haiku-1.28.99-<revision>`,
and the link next to it lists the code that changed since the revision before.
The recipe is `efl-1.28.99.recipe` in this directory.

Revisions 1 and 2 were early builds, and revision 4 a test build, that were
never published.

## Known issues

* `ELM_NEBULA=1` (`--nebula` in Terminology) uses OpenGL with the Nebula NVIDIA
  driver, and with that driver the window freezes and ending the program can
  hang the system. The fault is in the driver, not in EFL. This is why OpenGL is
  off there by default.

## Revision 10 (2026-10-03)

Tag `haiku-1.28.99-10`, source `1cb25f8d8b`.
[Changes since revision 9](https://github.com/ablyssx74/efl/compare/haiku-1.28.99-9...haiku-1.28.99-10)

* drag and drop onto the native window: files dragged from Tracker, and text
  dragged from other applications, reach the application as text, for files
  their paths one per line. Applications that accept drops, like Terminology,
  paste the paths or show the media file, with nothing to change.
* dragging out of an EFL window is not implemented.

## Revision 9 (2026-10-02)

Tag `haiku-1.28.99-9`, source `5c033d5734`.
[Changes since revision 8](https://github.com/ablyssx74/efl/compare/haiku-1.28.99-8...haiku-1.28.99-9)

OpenGL is an opt-in again, and only where it is safe.

* the SDL engines, which make SDL create an OpenGL context for every window, are
  not used when the Nebula NVIDIA driver is installed (the Mesa EGL vendor
  library `/boot/system/add-ons/opengl/egl_vendor.d/libEGL_mesa.so` is there).
  Neither `ELM_ENGINE=sdl` nor a saved engine brings them back. `ELM_NEBULA=1`
  opts in. When OpenGL was asked for and is not used, an error message says how
  to opt in.
* without the Nebula driver the SDL engines are allowed. The default stays the
  native engine, without OpenGL. Asking for OpenGL (the acceleration
  preference, or `ELM_ACCEL=gl`) selects the SDL engines.
* `elementary_config`: the renderer list and the override check box work again.
  They are greyed out only with the Nebula driver installed and without
  `ELM_NEBULA`.

## Revision 8 (2026-10-02)

Tag `haiku-1.28.99-8`, source `503c49191f`.
[Changes since revision 7](https://github.com/ablyssx74/efl/compare/haiku-1.28.99-7...haiku-1.28.99-8)

* Ctrl and Alt follow the keys printed Ctrl and Alt, whatever the keymap calls
  them. With the Linux layout of the Keymap preferences, where Command sits on
  the Ctrl key and Control on the Alt key, applications saw the two swapped, and
  Ctrl+Shift+C and Ctrl+Shift+V did not work in Terminology. The default layout
  behaves as before.
* key presses with Command are given to the application. BWindow kept them for
  its own shortcuts, so Ctrl+C, Ctrl+V and every Ctrl shortcut never arrived
  when Command was on the Ctrl key.
* the character sent for Ctrl+key and Alt+key is the same in both layouts:
  Ctrl+C sends the control character, Alt+C sends Escape and the letter.

## Revision 7 (2026-09-30)

Tag `haiku-1.28.99-7`, source `d28d6d98e8`.
[Changes since revision 6](https://github.com/ablyssx74/efl/compare/haiku-1.28.99-6...haiku-1.28.99-7)

* fixes a freeze of the native window engine: clicking into some text fields of
  Terminology's settings made the clipboard code call itself until the heap was
  corrupted. A change of the selection is now reported from the main loop, as
  the X11 and Wayland engines do. The SDL engine had the same flaw and has the
  same fix.

## Revision 6 (2026-09-30)

Tag `haiku-1.28.99-6`, source `eeeb48f2a0`.
[Changes since revision 5](https://github.com/ablyssx74/efl/compare/haiku-1.28.99-5...haiku-1.28.99-6)

* sound goes through the Haiku media kit (`BSoundPlayer`) instead of SDL, so a
  process that plays sounds no longer loads SDL or OpenGL. The SDL audio output
  is no longer built on Haiku, and `libecore_audio` needs `libmedia`.
  `ECORE_AUDIO_BACKEND=haiku` forces it.

## Revision 5 (2026-09-30)

Tag `haiku-1.28.99-5`, source `d2c132a4a5`.
[Changes since revision 3](https://github.com/ablyssx74/efl/compare/haiku-1.28.99-3...haiku-1.28.99-5)

* a native Haiku window engine, picked automatically: it draws into a `BWindow`
  with a `BBitmap`, without OpenGL. This avoids the freeze some GPU driver
  stacks caused with the SDL engine, where SDL creates an OpenGL context for
  every window. Keyboard, mouse and wheel, clipboard, key repeat, window title,
  resize grip and size steps, fullscreen, maximise and borderless windows.
* `elementary_config` offers no renderer choice on Haiku, until OpenGL is an
  opt-in.
* the SDL engine is still built but is no longer the default.
* fixes the declaration of the SDL audio probe that was hidden inside the
  PipeWire block.

## Revision 3 (2026-09-29)

Tag `haiku-1.28.99-3`, source `7f1a8438a3`. First published revision.

* the SDL window engine, picked automatically.
* sound through SDL audio (edje sounds, e.g. the terminal bell).
* media playback through GStreamer (the packages require the base, good and
  libav plug-ins).
* clipboard, key repeat, mouse and window fixes for the SDL backend.
* fixes for the poll file monitor and emotion on Haiku.
* no OpenGL, X11 or Wayland engines are built.
