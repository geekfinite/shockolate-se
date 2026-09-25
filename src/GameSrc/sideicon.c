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
/*
 * $Source: r:/prj/cit/src/RCS/sideicon.c $
 * $Revision: 1.53 $
 * $Author: mahk $
 * $Date: 1994/11/23 04:31:51 $
 *
 */

#include <stdlib.h>
#include <string.h>

#include "sideicon.h"
#include "sideart.h"
#include "popups.h"
#include "cybstrng.h"
#include "tools.h"
#include "fullscrn.h"
#include "newmfd.h"
#include "tools.h"
#include "wares.h"
#include "objsim.h"
#include "objclass.h"
#include "otrip.h"
#include "player.h"
#include "faketime.h"
#include "mfdext.h"
#include "objapp.h"
#include "musicai.h"
#include "sfxlist.h"
#include "gr2ss.h"
#include "canvchek.h"

// ---------
// Constants
// ---------

#define NUM_SIDE_ICONS      10

#define SIDE_ICONS_TOP_Y    24
#define SIDE_ICONS_LEFT_X   4
#define SIDE_ICONS_RIGHT_X  300
#define SIDE_ICONS_HEIGHT   15
#define SIDE_ICONS_WIDTH    15
#define SIDE_ICONS_VSPACE   8

#define ICON_ART_ITEMS      2
#define ICON_ART_OFF        0
#define ICON_ART_ON         1
#define ICON_ART_BACKGROUND 255

#define FLASH_RATE          256
#define MAX_FLASH_COUNT     12

// The two halves of the icon column. These are created once in
// screen_init_side_icons() and then kept in sync with side_icons[i].r by
// side_icon_rebuild_regions() (called on every fullscreen entry and on every
// HUD-scale change). The regions must be moved/resized, not just their
// underlying rects recomputed, because uiInstallRegionHandler snapshots the
// rect at install time.
static LGRegion *side_left_region  = NULL;
static LGRegion *side_right_region = NULL;

// Real-pixel rects for the fullscreen draw (the draw maps block-logical -> real
// by *k, so these are passed as real/k). Kept separate from side_icons[].r so
// the 320x200 logical round-trip (needed for the regions/mouse) cannot distort
// the drawn icon size -- side icons must stay exactly square. Filled by
// init_all_side_icons(); only meaningful in fullscreen.
static short side_icon_real[NUM_SIDE_ICONS][4]; // ul.x, ul.y, lr.x, lr.y


// ----------------
// Local Prototypes
// ----------------

uchar side_icon_mouse_callback(uiEvent *e, LGRegion *r, intptr_t udata);
void zoom_side_icon_to_mfd(int icon, int waretype, int wnum);
uchar side_icon_hotkey_func(ushort keycode, uint32_t context, intptr_t i);
void side_icon_draw_bm(LGRect *r, ubyte icon, ubyte art);

// ----------
// Structures
// ----------

typedef struct _side_icon {
    LGRect r;
    uchar flashstate;
    ubyte flashcount;
    ubyte state;
} SIDE_ICON;

typedef struct _icon_data {
    byte waretype;
    long waretrip;
    int flashfx;
} ICON_DATA;

// -------
// Globals
// -------

SIDE_ICON side_icons[NUM_SIDE_ICONS];
#ifdef PRELOAD_BITMAPS
grs_bitmap side_icon_bms[NUM_SIDE_ICONS][ICON_ART_ITEMS];
grs_bitmap side_icon_background;
#else
#define side_icon_bmid(icon, art) (MKREF(RES_SideIconArt, (ICON_ART_ITEMS * icon) + art + 1))
#define side_icon_backid (MKREF(RES_SideIconArt, 0))
#endif

#ifdef PROGRAM_SIDEICON
static char shiftnums[] = ")!@#$%^&*(";
static uchar programmed_sideicon = 0;
#endif

// this is in wares.c
extern long ware_base_triples[NUM_WARE_TYPES];

#define IDX_OF_TYPE(type, trip) (OPTRIP(trip) - OPTRIP(ware_base_triples[type]))

static ICON_DATA icon_data[NUM_SIDE_ICONS] = {
    {WARE_HARD, BIOSCAN_HARD_TRIPLE},
    {WARE_HARD, FULLSCR_HARD_TRIPLE},
    {WARE_HARD, SENS_HARD_TRIPLE},
    {WARE_HARD, LANTERN_HARD_TRIPLE},
    {WARE_HARD, SHIELD_HARD_TRIPLE},
    {WARE_HARD, INFRA_GOG_TRIPLE},
    {WARE_HARD, NAV_HARD_TRIPLE},
    {WARE_HARD, VIDTEX_HARD_TRIPLE, SFX_EMAIL},
    {WARE_HARD, MOTION_HARD_TRIPLE},
    {WARE_HARD, JET_HARD_TRIPLE},
};

grs_bitmap icon_cursor_bm[2];
LGCursor icon_cursor[2];
static char *cursor_strings[NUM_SIDE_ICONS];
static char cursor_strbuf[128];

// ============
// INITIALIZERS
// ============

// ---------------------------------------------------------------------------
// init_all_side_icons()
//
// Initialize all side icons to "unset" settings (should be called before
// wares_init()!).  Also sets up their on-screen locations.
// And, as of 7/22, loads in all the bitmaps from memory.

void side_icon_language_change(void) {
    load_string_array(REF_STR_IconCursor, cursor_strings, cursor_strbuf, sizeof(cursor_strbuf), NUM_SIDE_ICONS);
}

void init_all_side_icons() {
    int i;
    extern uchar full_game_3d;
    extern float hud_scale_factor(void); // newmfd.c

    // Non-fullscreen: the classic 320x200 layout, verbatim (k == 1), so the
    // in-game screen is pixel-identical to the original.
    if (!full_game_3d) {
        for (i = 0; i < (NUM_SIDE_ICONS / 2); i++) {
            side_icons[i].r.ul.x = SIDE_ICONS_LEFT_X;
            side_icons[i].r.ul.y = (short)(SIDE_ICONS_TOP_Y + i * (SIDE_ICONS_HEIGHT + SIDE_ICONS_VSPACE));
            side_icons[i].r.lr.x = (short)(side_icons[i].r.ul.x + SIDE_ICONS_WIDTH);
            side_icons[i].r.lr.y = (short)(side_icons[i].r.ul.y + SIDE_ICONS_HEIGHT);
        }
        for (i = (NUM_SIDE_ICONS / 2); i < NUM_SIDE_ICONS; i++) {
            side_icons[i].r.lr.x = (short)(SIDE_ICONS_RIGHT_X + SIDE_ICONS_WIDTH);
            side_icons[i].r.ul.x = (short)(side_icons[i].r.lr.x - SIDE_ICONS_WIDTH);
            side_icons[i].r.ul.y = side_icons[i - (NUM_SIDE_ICONS / 2)].r.ul.y;
            side_icons[i].r.lr.y = (short)(side_icons[i].r.ul.y + SIDE_ICONS_HEIGHT);
        }
        return;
    }

    // Fullscreen: lay the icons out in REAL pixels from the framebuffer size
    // and the HUD tier k -- a SINGLE isotropic scale on both axes, so the icons
    // stay SQUARE at any aspect ratio (a per-axis k/sconv made them widen past
    // 16:9, because SCONV_X/SCONV_Y diverge). The rects are then stored back in
    // the engine's 320x200 logical space -- the space the region system and the
    // mouse events use (see fscrn_rect in screen.c) -- so input keeps lining up;
    // side_icon_draw_bm maps them to real px under the shared isotropic override.
    {
        const int   real_w = grd_cap->w;
        const int   real_h = grd_cap->h;
        const float k      = hud_scale_factor();
        extern void hud_bounds_insets(int *left, int *right); // newmfd.c
        int bound_l = 0, bound_r = 0;
        int kw, kh, kv, lft, rgt, top, lx, rx;

        if (real_w <= 0 || real_h <= 0)
            return;

        // Real-pixel geometry: square icon (WIDTH*k), row pitch, edge insets.
        kw  = (int)(SIDE_ICONS_WIDTH  * k + 0.5f);
        kh  = (int)(SIDE_ICONS_HEIGHT * k + 0.5f);
        kv  = (int)((SIDE_ICONS_HEIGHT + SIDE_ICONS_VSPACE) * k + 0.5f);
        lft = (int)(SIDE_ICONS_LEFT_X * k + 0.5f);
        rgt = (int)((320 - SIDE_ICONS_RIGHT_X - SIDE_ICONS_WIDTH) * k + 0.5f);
        top = (int)(SIDE_ICONS_TOP_Y * k + 0.5f);

        // HUD bounds: pull each column in from its screen edge (real px).
        hud_bounds_insets(&bound_l, &bound_r);
        lx = lft + bound_l;             // left column left edge (real px)
        rx = real_w - rgt - bound_r;    // right column right edge (real px)

#define SICON_R2LX(px) ((short)((px) * 320.0f / (float)real_w + 0.5f))
#define SICON_R2LY(py) ((short)((py) * 200.0f / (float)real_h + 0.5f))

        for (i = 0; i < (NUM_SIDE_ICONS / 2); i++) {
            const int ry = top + i * kv;
            side_icon_real[i][0] = (short)lx;
            side_icon_real[i][1] = (short)ry;
            side_icon_real[i][2] = (short)(lx + kw);
            side_icon_real[i][3] = (short)(ry + kh);
            side_icons[i].r.ul.x = SICON_R2LX(lx);
            side_icons[i].r.ul.y = SICON_R2LY(ry);
            side_icons[i].r.lr.x = SICON_R2LX(lx + kw);
            side_icons[i].r.lr.y = SICON_R2LY(ry + kh);
        }
        for (i = (NUM_SIDE_ICONS / 2); i < NUM_SIDE_ICONS; i++) {
            const int ry = top + (i - (NUM_SIDE_ICONS / 2)) * kv;
            side_icon_real[i][0] = (short)(rx - kw);
            side_icon_real[i][1] = (short)ry;
            side_icon_real[i][2] = (short)rx;
            side_icon_real[i][3] = (short)(ry + kh);
            side_icons[i].r.lr.x = SICON_R2LX(rx);
            side_icons[i].r.ul.x = SICON_R2LX(rx - kw);
            side_icons[i].r.ul.y = SICON_R2LY(ry);
            side_icons[i].r.lr.y = SICON_R2LY(ry + kh);
        }
#undef SICON_R2LX
#undef SICON_R2LY
    }
}

void init_side_icon_popups(void) {
    side_icon_language_change();
    for (int i = 0; i < 2; i++) {
        LGPoint offset = {0, -1};
        LGCursor *c = &icon_cursor[i];
        grs_bitmap *bm = &icon_cursor_bm[i];
        make_popup_cursor(c, bm, cursor_strings[i * NUM_SIDE_ICONS / 2], i + POPUP_ICON_LEFT, TRUE, offset);
    }
}

#ifdef DUMMY // not yet, bucko

void init_side_icon_hotkeys(void) {
    uchar side_icon_hotkey_func(ushort key, uint32_t context, intptr_t i);
    uchar side_icon_progset_hotkey_func(ushort key, uint32_t context, intptr_t i);
    uchar lantern_change_setting_hkey(ushort key, uint32_t context, intptr_t i);
    uchar shield_change_setting_hkey(ushort key, uint32_t context, intptr_t i);
    uchar side_icon_prog_hotkey_func(ushort key, uint32_t context, intptr_t notused);
    int i;

    hotkey_add(KB_FLAG_ALT | KB_FLAG_DOWN | '4', DEMO_CONTEXT, lantern_change_setting_hkey, 0);
    hotkey_add(KB_FLAG_ALT | KB_FLAG_DOWN | '5', DEMO_CONTEXT, shield_change_setting_hkey, 0);

    hotkey_add(KB_FLAG_DOWN | '0', DEMO_CONTEXT, side_icon_hotkey_func, NUM_SIDE_ICONS - 1);
#ifdef PROGRAM_SIDEICON
    hotkey_add('`', DEMO_CONTEXT, ide_icon_prog_hotkey_func, 0);
    hotkey_add(KB_FLAG_DOWN | shiftnums[0], DEMO_CONTEXT, side_icon_progset_hotkey_func,
               NUM_SIDE_ICONS - 1);
#endif
    for (i = 0; i < NUM_SIDE_ICONS - 1; i++) {
        hotkey_add(KB_FLAG_DOWN | ('1' + i), DEMO_CONTEXT, side_icon_hotkey_func, i);
#ifdef PROGRAM_SIDEICON
        hotkey_add(KB_FLAG_DOWN | shiftnums[1 + i], DEMO_CONTEXT, side_icon_progset_hotkey_func,
                   i);
#endif
    }
}

#endif // DUMMY

// ---------------------------------------------------------------------------
// init_side_icon()
//

// ---------------------------------------------------------------------------
// screen_init_side_icons();
//
// Declare the appropriate regions for the side icons
// (called from screen_start() in screen.c)

void screen_init_side_icons(LGRegion *root) {
    int id;
    LGRect r;
    side_left_region  = (LGRegion *)malloc(sizeof(LGRegion));
    side_right_region = (LGRegion *)malloc(sizeof(LGRegion));

    // Wow, having a LGRegion for each of the side icons is totally uncool
    // Let's just have two regions, and figure out from there.

    r.ul = side_icons[0].r.ul;
    r.lr = side_icons[(NUM_SIDE_ICONS - 1) / 2].r.lr;
    macro_region_create_with_autodestroy(root, side_left_region, &r);
    uiInstallRegionHandler(side_left_region, UI_EVENT_MOUSE | UI_EVENT_MOUSE_MOVE, &side_icon_mouse_callback, 0,
                           &id);
    uiSetRegionDefaultCursor(side_left_region, NULL);

    r.ul = side_icons[(NUM_SIDE_ICONS + 1) / 2].r.ul;
    r.lr = side_icons[(NUM_SIDE_ICONS - 1)].r.lr;
    macro_region_create_with_autodestroy(root, side_right_region, &r);
    uiInstallRegionHandler(side_right_region, UI_EVENT_MOUSE | UI_EVENT_MOUSE_MOVE, &side_icon_mouse_callback,
                           ((NUM_SIDE_ICONS + 1) / 2), &id);
    uiSetRegionDefaultCursor(side_right_region, NULL);

    return;
}

void side_icon_rebuild_regions(void) {
    if (!side_left_region || !side_right_region)
        return;

    LGRect r;

    // Left column: top of the first icon to bottom of the last icon in the
    // first half.
    r.ul = side_icons[0].r.ul;
    r.lr = side_icons[(NUM_SIDE_ICONS - 1) / 2].r.lr;
    region_move  (side_left_region, r.ul.x, r.ul.y, 2);
    region_resize(side_left_region, r.lr.x - r.ul.x, r.lr.y - r.ul.y);

    // Right column: same, second half.
    r.ul = side_icons[(NUM_SIDE_ICONS + 1) / 2].r.ul;
    r.lr = side_icons[NUM_SIDE_ICONS - 1].r.lr;
    region_move  (side_right_region, r.ul.x, r.ul.y, 2);
    region_resize(side_right_region, r.lr.x - r.ul.x, r.lr.y - r.ul.y);
}

// =========
// SELECTION
// =========

// ----------------------------------------------------------------
// select_side_icon() selects a given side icon.

void zoom_side_icon_to_mfd(int icon, int waretype, int wnum) {
    extern ubyte waretype2invtype[];

    int mfd;

    mfd = mfd_grab_func(MFD_EMPTY_FUNC, MFD_ITEM_SLOT);
    mfd_zoom_rect(&side_icons[icon].r, mfd);
    set_inventory_mfd(waretype2invtype[waretype], wnum, TRUE);
    mfd_change_slot(mfd, MFD_ITEM_SLOT);
}

// ---------------------------------------------------------------------------
// side_icon_mouse_callback()
//
// Callback function for mouse clicks inside the side icons.

extern LGCursor globcursor;

int last_side_icon = -1;
uchar side_icon_mouse_callback(uiEvent *e, LGRegion *r, intptr_t udata) {
    extern uchar fullscrn_icons;
    uchar retval = FALSE;
    int i, type, num;

    if (!global_fullmap->cyber && !(full_game_3d && !fullscrn_icons)) {
        int ver;
        extern float hud_scale_factor(void); // newmfd.c

        // Find which icon was clicked by testing the actual drawn rects. This
        // avoids any assumption about what coordinate space e->pos is in and
        // which row pitch the current HUD scale implies -- if the click is
        // inside side_icons[j].r, then j is the icon.
	int j, start = (int)udata, end = start + (NUM_SIDE_ICONS / 2);
        i = -1;
        for (j = start; j < end; j++) {
            if (RECT_TEST_PT(&side_icons[j].r, e->pos)) {
                i = j;
                break;
            }
        }
        if (i < 0) {
            uiSetRegionDefaultCursor(r, &globcursor);
            last_side_icon = -1;
            return FALSE;
        }

        type = icon_data[i].waretype;
        num  = IDX_OF_TYPE(type, icon_data[i].waretrip);
        ver  = get_player_ware_version(type, num);

        if (!RECT_TEST_PT(&side_icons[i].r, e->pos) || ver == 0) {
            uiSetRegionDefaultCursor(r, &globcursor);
            last_side_icon = -1;
            return FALSE;
        }

        if (popup_cursors) {
            if (last_side_icon != i) {
                uchar side = i * 2 / NUM_SIDE_ICONS;
                LGCursor *c = &icon_cursor[side];
                grs_bitmap *bm = &icon_cursor_bm[side];
                // Offset is in the popup's NATIVE units -- make_popup_cursor
                // converts it with the same scale as the art, so it must not be
                // pre-scaled by k (doing that pushed the label up by ~k^2 px).
                LGPoint offset = {0, -1};

                free(bm->bits);
                make_popup_cursor(c, bm, cursor_strings[i], side + POPUP_ICON_LEFT, TRUE, offset);
                uiSetRegionDefaultCursor(r, c);
                last_side_icon = i;
            }
        }
        /*
              if (m->action & MOUSE_RDOWN)
              {
                 zoom_side_icon_to_mfd(i,type,num);
                 retval = TRUE;
              }
        */

        if (!(e->mouse_data.action & MOUSE_LDOWN))
            return retval; // ignore click releases
                           //   mprintf("  Side Icon %d: CYBER(%d,%d) [%x] REAL(%d,%d) [%x]\n",
                           //      i, side_icons[i].cyber_type, side_icons[i].cyber_num,
                           //      side_icons[i].cyber_set, side_icons[i].real_type,
                           //      side_icons[i].real_num, side_icons[i].real_set);

        if (type >= 0)
            use_ware(type, num);
        retval = TRUE;
    }
    if (global_fullmap->cyber || (full_game_3d && !fullscrn_icons) || !popup_cursors) {
        last_side_icon = -1;
        uiSetRegionDefaultCursor(r, NULL);
    }

    return retval;
}

uchar side_icon_hotkey_func(ushort keycode, uint32_t context, intptr_t i) {
    int type = icon_data[i].waretype;
    int num = IDX_OF_TYPE(type, icon_data[i].waretrip);
    if ((!global_fullmap->cyber) || (i == 1)) {
        if (type >= 0)
            use_ware(type, num);
    }
    return TRUE;
}

#ifdef PROGRAM_SIDEICON
uchar side_icon_progset_hotkey_func(ushort keycode, uint32_t context, intptr_t i) {
    char mess[80];
    int l;
    programmed_sideicon = i;
    get_string(REF_STR_PresetSideicon, mess, 80);
    l = strlen(mess);
    get_object_short_name(icon_data[i].waretrip, mess + l, 80 - l);
    message_info(mess);
    return TRUE;
}

uchar side_icon_prog_hotkey_func(ushort keycode, uint32_t context, intptr_t notused) {
    return (side_icon_hotkey_func(keycode, context, programmed_sideicon));
}
#endif

// ========
// GRAPHICS
// ========

// ---------------------------------------------------------------------------
// side_icon_expose_all()
//
// Sort of an initial-draw-everything type of routine

void side_icon_expose_all() {
    for (uint8_t i = 0; i < NUM_SIDE_ICONS; i++)
        side_icon_expose(i);
}

// ----------------------------------------------------
// zoom_to_side_icon(Point from, int icon)
// zooms a LGRect to a side icon and then exposes it.

void zoom_to_side_icon(LGPoint from, int icon) {
    LGRect start = {{-3, -3}, {3, 3}};
    LGRect dest;
    RECT_MOVE(&start, from);
    dest = side_icons[icon].r;
    zoom_rect(&start, &dest);
    side_icon_expose(icon);
}

// ---------------------------------------------------------------------------
// side_icon_draw_bm()
//
// Draws a side icon of the specified ware, version, and status.

void side_icon_draw_bm(LGRect *r, ubyte icon, ubyte art) {
#ifdef SVGA_SUPPORT
    uchar old_over = gr2ss_override;
    gr2ss_override = OVERRIDE_ALL;
#endif
    if (is_onscreen())
        uiHideMouse(r);

    if (full_game_3d) {
        Ref ref = (art == ICON_ART_BACKGROUND) ? side_icon_backid : side_icon_bmid(icon, art);
        FrameDesc *f = RefLock(ref);
        if (f) {
            // Draw under the shared isotropic fullscreen override (the convert
            // maps block-logical -> real by a uniform *k). Use the real-pixel
            // rect from init_all_side_icons() and pass it as real/k, so the
            // drawn icon is exactly square regardless of the 320x200 logical
            // rounding the regions need. Non-fullscreen never reaches here.
            extern void inventory_block_scale_begin(void);
            extern void inventory_block_scale_end(void);
            extern float hud_scale_factor(void);
            const float k = hud_scale_factor();
            const int rex = side_icon_real[icon][0];
            const int rey = side_icon_real[icon][1];
            const int rew = side_icon_real[icon][2] - side_icon_real[icon][0];
            const int reh = side_icon_real[icon][3] - side_icon_real[icon][1];
            const int bx = (int)(rex / k + 0.5f);
            const int by = (int)(rey / k + 0.5f);
            const int bw = (int)(rew / k + 0.5f);
            const int bh = (int)(reh / k + 0.5f);
	    f->bm.bits = (uchar *)(f+1);
            inventory_block_scale_begin();
            ss_scale_bitmap(&f->bm, bx, by, bw, bh);
            inventory_block_scale_end();
            RefUnlock(ref);
        }
    } else {
        if (art == ICON_ART_BACKGROUND)
            draw_raw_resource_bm(side_icon_backid, r->ul.x, r->ul.y);
        else
            draw_raw_resource_bm(side_icon_bmid(icon, art), r->ul.x, r->ul.y);
    }

    if (is_onscreen())
        uiShowMouse(r);
#ifdef SVGA_SUPPORT
    gr2ss_override = old_over;
#endif
}
// ---------------------------------------------------------------------------
// side_icon_expose()
//
// Draw a side icon appropriately, depending on the ware it points at,
// and its state.

void side_icon_expose(ubyte icon_num) {
    ubyte *player_wares, *player_status;
    WARE *wares;
    int type, num, n;
    LGRect *r;
    extern uchar fullscrn_icons;

    if (full_game_3d && (global_fullmap->cyber || !(fullscrn_icons)))
        return;
    r = &(side_icons[icon_num].r);

    type = icon_data[icon_num].waretype;
    num = IDX_OF_TYPE(type, icon_data[icon_num].waretrip);

    if (type < 0)
        return;
    get_ware_pointers(type, &player_wares, &player_status, &wares, &n);

    // Possible expose cases:
    //
    // 1) We don't have the appropriate ware for that side icon.
    if (player_wares[num] == 0) {

        // Expose screen background
        //      Spew(DSRC_TESTING_Test9,("icon number %d is empty\n",icon_num));
        if (!full_game_3d)
            side_icon_draw_bm(r, icon_num, ICON_ART_BACKGROUND);
    }

    // 3) We have the ware, and it's trying to get our attention.
    //
    else if (player_status[num] & WARE_FLASH) {
        uchar fs = ((*tmd_ticks / FLASH_RATE) % 2);
        if (fs == side_icons[icon_num].flashstate && !full_game_3d)
            return;

        if (fs) {
            side_icon_draw_bm(r, icon_num, ICON_ART_ON);
            if (fs != side_icons[icon_num].flashstate)
                if (icon_data[icon_num].flashfx != 0)
                    if (QUESTBIT_GET(0x12c))
                        play_digi_fx(icon_data[icon_num].flashfx, 1);
        } else
            side_icon_draw_bm(r, icon_num, ICON_ART_OFF);
        if (fs != side_icons[icon_num].flashstate) {
            side_icons[icon_num].flashcount++;
            if (side_icons[icon_num].flashcount >= MAX_FLASH_COUNT) {
                side_icons[icon_num].flashcount = 0;
                player_status[num] &= ~WARE_FLASH;
            }
        }
        side_icons[icon_num].flashstate = fs;
    }

    // 2) We have the ware, but we're turning it off.
    //
    else if (!(player_status[num] & WARE_ON)) {

        // Expose darkened bitmap of current version
        //      Spew(DSRC_TESTING_Test9,("icon number %d is off\n",icon_num));
        side_icon_draw_bm(r, icon_num, ICON_ART_OFF);
    } else {

        // Expose normal (active) bitmap of current ware version
        side_icon_draw_bm(r, icon_num, ICON_ART_ON);
    }

    return;
}

// ---------------------------------------------------------------------------
// side_icon_load_bitmaps()
//
// Load the bitmaps for all side icons and states from the resource system.

errtype side_icon_load_bitmaps() {
#ifdef PRELOAD_BITMAPS
    RefTable *side_icon_rft;
    int i, j, index /*, file_handle */;

    //  file_handle = ResOpenFile("sideart.res");
    //   if (file_handle < 0) critical_error(CRITERR_RES|6);

    side_icon_rft = ResLock(RES_SideIconArt);
    load_bitmap_from_res(&side_icon_background, RES_SideIconArt, 0, side_icon_rft, FALSE, NULL, NULL);

    for (i = 0; i < NUM_SIDE_ICONS; i++) {

        for (j = 0; j < ICON_ART_ITEMS; j++) {

            index = (ICON_ART_ITEMS * i) + j + 1;
            load_bitmap_from_res(&(side_icon_bms[i][j]), RES_SideIconArt, index, side_icon_rft, FALSE, NULL, NULL);
        }
    }
    ResUnlock(RES_SideIconArt);
//   ResCloseFile(file_handle);
#endif

    return (OK);
}

errtype side_icon_free_bitmaps() {
#ifdef PRELOAD_BITMAPS
    int i, j, index;
    Free(side_icon_background.bits);
    for (i = 0; i < NUM_SIDE_ICONS; i++) {

        for (j = 0; j < ICON_ART_ITEMS; j++) {

            index = (ICON_ART_ITEMS * i) + j;
            Free(side_icon_bms[i][j].bits);
        }
    }
#endif
    return (OK);
}
