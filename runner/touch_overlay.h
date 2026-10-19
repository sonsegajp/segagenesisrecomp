#ifndef TOUCH_OVERLAY_H
#define TOUCH_OVERLAY_H

/*
 * touch_overlay — an on-screen Genesis pad for touch screens. Opt-in: a build
 * defines GENESIS_TOUCH_OVERLAY and compiles touch_overlay.c (the Android
 * build does). Left: an 8-way d-pad; right: A, B and C; top centre: Start.
 * A finger that lands on the d-pad steers until it lifts; a finger on the
 * buttons slides between them. Shown while the screen is in use; a gamepad
 * button or key press hides it until the next touch. Android's Back button
 * presses Start (SDL_HINT_ANDROID_TRAP_BACK_BUTTON must be set).
 */
#include <stdint.h>
#include <SDL2/SDL.h>

void     touch_overlay_event(const SDL_Event *e);
uint16_t touch_overlay_mask(void);           /* Genesis button bits (input_map.c) */
void     touch_overlay_draw(SDL_Renderer *r);  /* once a frame, before the present */
/* the player's choice: shown or not, and its size (1 = the standard) */
void     touch_overlay_configure(int enabled, float scale);

#endif
