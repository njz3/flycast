#pragma once
#include <SDL.h>
#include "types.h"

void input_sdl_init();
void input_sdl_handle();
void input_sdl_quit();
bool sdl_queue_open_file(const char *path);
void sdl_window_create();
void sdl_window_destroy();
bool sdl_recreate_window(u32 flags);
// The window covering the screen without borders, at the desktop's resolution (Alt+Enter or F11)
bool sdl_is_fullscreen();
void sdl_set_fullscreen(bool fullscreen);
bool sdl_update_display_metrics(SDL_Window *window, u32 windowFlags);
void sdl_fix_steamdeck_dpi(SDL_Window *window);
