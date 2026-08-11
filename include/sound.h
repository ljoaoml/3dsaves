#pragma once

// Simple synthesized UI feedback sounds via ndsp -- no external audio
// assets needed, both tones are plain sine waves generated once at init
// into fixed PCM buffers. Hooked into ui_print_success()/ui_print_error()
// (see ui.c) so every existing success/error message in the app gets
// sound for free, without touching each of their call sites individually.
//
// Best-effort like every other optional-hardware-feature in this app:
// if ndspInit() fails for any reason, every function here silently
// becomes a no-op instead of the caller needing to check anything.
void sound_init(void);
void sound_exit(void);

void sound_play_success(void);
void sound_play_error(void);
