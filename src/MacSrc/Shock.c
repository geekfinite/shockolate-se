/*

Copyright (C) 2015-2018 Night Dive Studios, LLC.

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/
//====================================================================================
//
//		System Shock - ©1994-1995 Looking Glass Technologies, Inc.
//
//		Shock.c	-	Mac-specific initialization and main event loop.
//
//====================================================================================

//--------------------
//  Includes
//--------------------
#include <math.h>
#include <stdlib.h>
#include <SDL.h>


#include "InitMac.h"
#include "Modding.h"
#ifdef USE_OPENGL
#include "OpenGL.h"
#endif
#include "Prefs.h"
#include "Shock.h"
#include "ShockBitmap.h"

#include "amaploop.h"
#include "gr2ss.h"
#include "hkeyfunc.h"
#include "mainloop.h"
#include "setup.h"
#include "shockolate_version.h"
#include "status.h"
#include "version.h"

#include "fullscrntogg.h"

//--------------------
//  Globals
//--------------------
bool gPlayingGame;
bool gFullscreenArg;
bool gWindowedArg;

grs_screen *cit_screen;
SDL_Window *window;
SDL_Palette *sdlPalette;
SDL_Renderer *renderer;

SDL_AudioDeviceID device;

int num_args;
char **arg_values;

extern grs_screen *svga_screen;
extern frc *svga_render_context;

//--------------------
//  Prototypes
//--------------------
extern void init_all(void);
extern void inv_change_fullscreen(uchar on);
extern void object_data_flush(void);
extern errtype load_da_palette(void);

// see Prefs.c
extern void CreateDefaultKeybindsFile(void);
extern void LoadHotkeyKeybinds(void);
extern void LoadMoveKeybinds(void);

int GetIntArgument(char *arg);
void ApplyCustomResolutionArg(void);
void ApplyHudBoundArg(void);

//------------------------------------------------------------------------------------
//		Main function.
//------------------------------------------------------------------------------------
int main(int argc, char **argv) {
    // CRT debug heap removed: _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF |
    // _CRTDBG_CHECK_ALWAYS_DF) made every allocation walk and validate the
    // whole heap, which is a large startup/per-frame cost in this build.
    // Save the arguments for later

    num_args = argc;
    arg_values = argv;

    // FIXME externalize this
    log_set_quiet(0);
    log_set_level(LOG_INFO);

    INFO("Logger initialized");

    // init mac managers

    InitMac();

    // Initialize the preferences file.

    SetDefaultPrefs();
    LoadPrefs();

    // Apply the previously-selected screen resolution (persisted in prefs as
    // doVideoMode). A custom -width/-height on the command line still wins --
    // ApplyCustomResolutionArg() runs below and overrides this.
    {
        extern short mode_id;
        if (gShockPrefs.doVideoMode >= 0 && gShockPrefs.doVideoMode <= 14 &&
            gShockPrefs.doVideoMode != 5) // 5 is the reserved stereo sentinel
            mode_id = gShockPrefs.doVideoMode;
    }

    // see Prefs.c
    CreateDefaultKeybindsFile(); // only if it doesn't already exist
    // even if keybinds file still doesn't exist, defaults will be set here
    LoadHotkeyKeybinds();
    LoadMoveKeybinds();

	if (gShockPrefs.doFullscreen)
		enterFullscreen(false);

    // Process some startup arguments

    bool show_splash = !CheckArgument("-nosplash");

	if (CheckArgument("-fullscreen"))
		enterFullscreen(false);
	if (CheckArgument("-windowed"))
		exitFullscreen(false);

    // -width/-height <pixels>: use a custom (e.g. widescreen) resolution
    // instead of one of the 5 built-in screen-mode menu options. Must run
    // before init_all() -- see ApplyCustomResolutionArg().
    ApplyCustomResolutionArg();

    // -hudbound <off|4:3|16:9|W:H|ratio>: bound the fullscreen HUD to a centred
    // rect of that aspect on widescreen. Overrides the prefs file for this run.
    ApplyHudBoundArg();

    // CC: Modding support! This is so exciting.

    ProcessModArgs(argc, argv);

    // Initialize

    init_all();
    setup_init();

    gPlayingGame = true;

    load_da_palette();
    gr_clear(0xFF);

    // Draw the splash screen

    //INFO("Showing splash screen");
    splash_draw(show_splash);

    // Start in the Main Menu loop

    _new_mode = _current_loop = SETUP_LOOP;
    loopmode_enter(SETUP_LOOP);

    // Start the main loop
	
    INFO("Showing main menu, starting game loop");
    mainloop(argc, argv);

    status_bio_end();
    stop_music();

    return 0;
}

bool CheckArgument(char *arg) {
    if (arg == NULL)
        return false;

    for (int i = 1; i < num_args; i++) {
        if (strcmp(arg_values[i], arg) == 0) {
            return true;
        }
    }

    return false;
}

// Returns the integer value following `arg` on the command line (e.g.
// "-width 1920" -> 1920 when arg is "-width"), or -1 if the argument
// wasn't given or has no numeric value after it.
int GetIntArgument(char *arg) {
    if (arg == NULL)
        return -1;

    for (int i = 1; i < num_args - 1; i++) {
        if (strcmp(arg_values[i], arg) == 0) {
            int value = atoi(arg_values[i + 1]);
            if (value > 0)
                return value;
            return -1;
        }
    }

    return -1;
}

// Applies a -width/-height override for the "1920x1080" screen-mode menu
// slot (svga_mode_data[14], see fullscrn.c/wrapper.c -- that slot is also
// the last entry of the redone screen-mode menu, so overriding it means
// the menu's "1920x1080" button will actually read/produce whatever
// custom size was requested here instead). Must run before init_all()
// (specifically before InitSDL() and screen_init()) so the overridden
// dimensions are what actually get used to create the window and register
// the SCONV_X/Y scale tables. Both -width and -height must be given
// together; if either is missing this is a no-op and existing behavior
// (whatever the prefs file / menu selected) is unchanged.
void ApplyCustomResolutionArg(void) {
    int width = GetIntArgument("-width");
    int height = GetIntArgument("-height");

    if (width <= 0 || height <= 0)
        return;

    // Sanity-clamp to something the fixed-point SCONV math and the
    // original 4:3 logical layout can reasonably scale to.
    if (width < 320)
        width = 320;
    if (height < 200)
        height = 200;

    // Round down to a multiple of 4. gScreenRowbytes now correctly uses
    // the real SDL surface pitch (see ShockBitmap.c), so this isn't
    // strictly required anymore -- but keeping widths 4-aligned avoids
    // relying on every other piece of old row-stepping code having the
    // same guarantee, for comparatively little cost (at most 3px lost).
    width &= ~3;
    height &= ~3;

    INFO("Custom resolution requested: %d x %d", width, height);

    grd_mode_info[GRM_1920x1080x8].w = width;
    grd_mode_info[GRM_1920x1080x8].h = height;

    gShockPrefs.doVideoMode = 14;

    extern short mode_id;
    mode_id = 14;
}

// Applies a "-hudbound <value>" override for the fullscreen HUD bounds, e.g.
// "-hudbound 4:3", "-hudbound 16:9", "-hudbound 21:9", "-hudbound 2.39" or
// "-hudbound off". Parsing is shared with the prefs file (hud_bounds_parse()
// in newmfd.c). Unparseable values are ignored.
void ApplyHudBoundArg(void) {
    extern int hud_bounds_parse(const char *s, short *mode, short *cw, short *ch);

    for (int i = 1; i < num_args - 1; i++) {
        if (strcmp(arg_values[i], "-hudbound") == 0) {
            short mode = 0, cw = 0, ch = 0;
            int kind = hud_bounds_parse(arg_values[i + 1], &mode, &cw, &ch);

            if (kind == 0) {
                INFO("-hudbound: can't parse \"%s\" (expected off, 4:3, 16:9, W:H or a decimal ratio)",
                     arg_values[i + 1]);
                return;
            }
            gShockPrefs.hudBoundMode = mode;
            if (kind == 2 && mode == 3) {
                gShockPrefs.hudBoundCustomW = cw;
                gShockPrefs.hudBoundCustomH = ch;
            }
            INFO("HUD bounds set from command line: mode %d", (int)mode);
            return;
        }
    }
}

void InitSDL() {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_AUDIO) < 0) {
        DEBUG("%s: Init failed", __FUNCTION__);
    }

    // TODO: figure out some universal set of settings that work...
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BUFFER_SIZE, 32);

    gr_init();

    extern short svga_mode_data[];
    gr_set_mode(svga_mode_data[gShockPrefs.doVideoMode], TRUE);

    INFO("Setting up screen and render contexts");

    // Create a canvas to draw to

    SetupOffscreenBitmaps(grd_cap->w, grd_cap->h);

    // Open our window!
    char window_title[128];
    //sprintf(window_title, "System Shock - %s", SHOCKOLATE_VERSION);
	sprintf(window_title, "System Shock - Enhanced");

    window = SDL_CreateWindow(window_title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, grd_cap->w, grd_cap->h,
                              SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_OPENGL);

	SDL_Surface* appicon = SDL_LoadBMP("shock.bmp");
	if (appicon)
		SDL_SetWindowIcon(window, appicon);

    // Create the palette

    sdlPalette = SDL_AllocPalette(256);

    // Setup the screen

    svga_screen = cit_screen = gr_alloc_screen(grd_cap->w, grd_cap->h);
    gr_set_screen(svga_screen);

    gr_alloc_ipal();

    SDL_ShowCursor(SDL_DISABLE);

    atexit(SDL_Quit);

    SDL_RaiseWindow(window);

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(renderer, grd_cap->w, grd_cap->h);

    // Startup OpenGL
#ifdef USE_OPENGL
    init_opengl();
#endif

    SDLDraw();

    SDL_ShowWindow(window);
}

SDL_Color gamePalette[256];
bool UseCutscenePalette = FALSE; // see cutsloop.c
void SetSDLPalette(int index, int count, uchar *pal) {
    static bool gammalut_init = 0;
    static uchar gammalut[100 - 10 + 1][256];
    if (!gammalut_init) {
	double factor = 2.2;// (can_use_opengl() ? 1.0 : 2.2); // OpenGL uses 2.2
        int i, j;
        for (i = 10; i <= 100; i++) {
            double gamma = (double)i * 1.0 / 100;
            gamma = 1 - gamma;
            gamma *= gamma;
            gamma = 1 - gamma;
            gamma = 1 / (gamma * factor);
            for (j = 0; j < 256; j++)
                gammalut[i - 10][j] = (uchar)(pow((double)j / 255, gamma) * 255);
        }
        gammalut_init = 1;
        INFO("Gamma LUT init\'ed");
    }

    int gam = gShockPrefs.doGamma;
    if (gam < 10)
        gam = 10;
    if (gam > 100)
        gam = 100;
    gam -= 10;

    for (int i = index; i < index + count; i++) {
        gamePalette[i].r = gammalut[gam][*pal++];
        gamePalette[i].g = gammalut[gam][*pal++];
        gamePalette[i].b = gammalut[gam][*pal++];
        gamePalette[i].a = 0xff;
    }

    if (!UseCutscenePalette) {
        // Hack black!
		gamePalette[255].r = 0x0;
		gamePalette[255].g = 0x0;
		gamePalette[255].b = 0x0;
        gamePalette[255].a = 0xff;
    }

    SDL_SetPaletteColors(sdlPalette, gamePalette, 0, 256);
    SDL_SetSurfacePalette(drawSurface, sdlPalette);
    SDL_SetSurfacePalette(offscreenDrawSurface, sdlPalette);
#ifdef USE_OPENGL
    if (should_opengl_swap())
        opengl_change_palette();
#endif
}

void SDLDraw() {
#ifdef USE_OPENGL
    if (should_opengl_swap()) {
        sdlPalette->colors[255].a = 0x00;
    }
#endif

    SDL_Texture *texture = SDL_CreateTextureFromSurface(renderer, drawSurface);

#ifdef USE_OPENGL
    if (should_opengl_swap()) {
        sdlPalette->colors[255].a = 0xff;
		SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    }
#endif

    SDL_Rect srcRect = {0, 0, gScreenWide, gScreenHigh};
    SDL_RenderCopy(renderer, texture, &srcRect, NULL);
    SDL_DestroyTexture(texture);

#ifdef USE_OPENGL
    if (should_opengl_swap()) {
        opengl_swap_and_restore();
    } else {
        SDL_RenderPresent(renderer);
        SDL_RenderClear(renderer);
    }
#else
	SDL_RenderPresent(renderer);
	SDL_RenderClear(renderer);
#endif
}

bool MouseCaptured = FALSE;

extern int mlook_enabled;

void CaptureMouse(bool capture) {
    MouseCaptured = (capture && gShockPrefs.goCaptureMouse);

    if (!MouseCaptured && mlook_enabled && SDL_GetRelativeMouseMode() == SDL_TRUE) {
        SDL_SetRelativeMouseMode(SDL_FALSE);

        int w, h;
        SDL_GetWindowSize(window, &w, &h);
        SDL_WarpMouseInWindow(window, w / 2, h / 2);
    } else
        SDL_SetRelativeMouseMode(MouseCaptured ? SDL_TRUE : SDL_FALSE);
}
