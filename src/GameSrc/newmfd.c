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

// NEWMFD.C

/*
 * $Source: r:/prj/cit/src/RCS/newmfd.c $
 * $Revision: 1.153 $
 * $Author: xemu $
 * $Date: 1994/11/21 22:03:39 $
 *
 */

// Source code for controlling the multi-function displays (MFDs)
// All MFD infrastructure belongs here, all expose/handler callbacks
// belong in mfdfunc.c

#include <stdlib.h>
#include <string.h>

#include "game_screen.h" // for the root region
#include "fullscrn.h"
#include "invent.h"
#include "mfdint.h"
#include "mfdext.h"
#include "mfdfunc.h"
#include "mfddims.h"
#include "input.h"
#include "player.h"
#include "tools.h"
#include "mainloop.h"
#include "gameloop.h"
#include "gamescr.h"
#include "musicai.h" // for digital FX
#include "sfxlist.h" // same
#include "citres.h"
#include "weapons.h"
#include "cit2d.h"
#include "popups.h"
#include "statics.h"
#include "gr2ss.h"
#include "Prefs.h"
#include "cybstrng.h"

#include "leanmetr.h"
#include "sideicon.h"
#include "status.h"

// -----------------------
// Player_Struct Accessors
// -----------------------

#define mfd_empty_func(mfd_num)            player_struct.mfd_empty_funcs[(mfd_num)]
#define mfd_index(mfd_num)                 player_struct.mfd_current_slots[(mfd_num)]
#define set_mfd_to_slot(mfd_id, vs, as)    player_struct.mfd_virtual_slots[(mfd_id)][(vs)] = (as)
#define set_default_to_func(mfd_num, fnum) player_struct.mfd_empty_funcs[(mfd_num)] = (fnum)
#define mfd_get_active_func(mfd_id)        mfd_get_func((mfd_id), (mfd_index((mfd_id))))

// -------
// Globals
// -------

#define MFD_NUM_BTTNS MFD_NUM_VIRTUAL_SLOTS

MFD mfd[2];         // Our actual MFD's
uchar Flash = TRUE; // State of blinking buttons
LGCursor mfd_bttn_cursors[NUM_MFDS];
grs_bitmap mfd_bttn_bitmaps[NUM_MFDS];

grs_canvas _offscreen_mfd, _fullscreen_mfd;

#define mfdL mfd[MFD_LEFT]
#define mfdR mfd[MFD_RIGHT]

grs_bitmap mfd_background;
grs_canvas *pmfd_canvas;


// =========================================================================
// HUD overlay module
//
// Elements register a native canvas, a corner/edge anchor, and a draw
// callback. At layout time, each element is given a real-pixel destination
// rect computed from its anchor and the global HUD scale. Drawing pushes
// the element's canvas, calls its draw callback, pops, and then blits the
// canvas to the framebuffer with gr_scale_bitmap — bypassing the outer SS
// scaler entirely, which is what makes the HUD aspect-correct.
// =========================================================================

#define HUD_ANCHOR_TL 0
#define HUD_ANCHOR_TR 1
#define HUD_ANCHOR_BL 2
#define HUD_ANCHOR_BR 3
#define HUD_ANCHOR_T  4  // top-center
#define HUD_ANCHOR_B  5  // bottom-center
#define HUD_ANCHOR_L  6  // left-middle
#define HUD_ANCHOR_R  7  // right-middle

typedef struct {
    const char *name;
    grs_canvas *canvas;       // native-size canvas to draw into
    int src_w, src_h;         // native size (pixels of the canvas)
    int anchor;
    int margin_x, margin_y;   // px of margin from anchored edge, in *real* pixels
    // Filled at layout time:
    int dst_x, dst_y, dst_w, dst_h;
    LGRegion *region;         // real-pixel region; may be NULL if no input
} hud_element;

#define HUD_MAX_ELEMENTS 16
static hud_element hud_elements[HUD_MAX_ELEMENTS];
static int hud_num_elements = 0;

// Global HUD scale (percent). 100 == native pixel size. This is the single
// user preference that sizes the whole fullscreen HUD -- MFD panels, vitals,
// inventory and the lean meter -- and is stored as gShockPrefs.hudScale.
// Only fullscreen code paths consult it (every caller sits inside a
// full_game_3d / fullscreen branch), so non-fullscreen rendering is untouched.
short hud_scale_pct = 100;

// Range of the user-facing setting. The floor sits above the smallest HUD
// font tier on purpose: below it the scaled-down fonts stop being legible, so
// 60..150 maps onto the "double" (floor) and "mega" tiers.
#define HUD_SCALE_MIN 100
#define HUD_SCALE_MAX 1000

// The HUD scales in INTEGER tiers only (1x .. 10x). A fractional factor makes
// the pixel art (HUD art, arrows and the weapon sprite) land on uneven pixel
// sizes, so snap the requested percentage to the nearest whole tier.
short hud_scale_clamp(short pct) {
    int tier = ((int)pct + 50) / 100; // nearest whole multiple of 100
    if (tier < HUD_SCALE_MIN / 100)
        tier = HUD_SCALE_MIN / 100;
    if (tier > HUD_SCALE_MAX / 100)
        tier = HUD_SCALE_MAX / 100;
    return (short)(tier * 100);
}

// Factor applied to every fullscreen HUD element's size. Callers multiply
// their existing fullscreen scale by this and then re-derive their position
// from their UNCHANGED anchor -- so an element that hugs a corner stays in
// that corner and grows about it, and centred elements stay centred.
float hud_scale_factor(void) {
    short p = gShockPrefs.hudScale ? gShockPrefs.hudScale : 100;
    return (float)hud_scale_clamp(p) / 100.0f;
}

// ---------------------------------------------------------------------------
// HUD bounds
//
// Optionally confines the *edge-anchored* fullscreen HUD elements (the two MFD
// units with their button strips, the side icon columns, the vitals readout)
// to a horizontally centred rect of a given aspect ratio instead of the full
// framebuffer width. E.g. on a 21:9 monitor, "4:3" pulls all of them back
// into the classic 4:3 layout in the middle of the screen while the 3D view
// still fills the whole window.
//
// Elements that are already horizontally centred (inventory, message line,
// lean meter) need no change: the bounded rect shares the screen's centre.
//
// Settings live in gShockPrefs.hudBoundMode / hudBoundCustomW / hudBoundCustomH.
// With mode OFF (the default) hud_bounds_insets() returns 0/0, so every caller
// lays out exactly as it did before this module existed.
// ---------------------------------------------------------------------------
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define HUD_BOUND_OFF    0
#define HUD_BOUND_4_3    1
#define HUD_BOUND_16_9   2
#define HUD_BOUND_CUSTOM 3

// Accepted range for a custom ratio (width / height).
#define HUD_BOUND_RATIO_MIN 1.0f
#define HUD_BOUND_RATIO_MAX 4.0f

extern short mode_id; // fullscrn.c: the selected screen-mode slot

// Target aspect (width / height) for the current setting; 0 when bounding is off.
static float hud_bound_ratio(void) {
    switch (gShockPrefs.hudBoundMode) {
    case HUD_BOUND_4_3:
        return 4.0f / 3.0f;
    case HUD_BOUND_16_9:
        return 16.0f / 9.0f;
    case HUD_BOUND_CUSTOM:
        if (gShockPrefs.hudBoundCustomW > 0 && gShockPrefs.hudBoundCustomH > 0) {
            float ratio = (float)gShockPrefs.hudBoundCustomW / (float)gShockPrefs.hudBoundCustomH;
            if (ratio < HUD_BOUND_RATIO_MIN)
                ratio = HUD_BOUND_RATIO_MIN;
            if (ratio > HUD_BOUND_RATIO_MAX)
                ratio = HUD_BOUND_RATIO_MAX;
            return ratio;
        }
        return 0.0f;
    default:
        return 0.0f;
    }
}

// Real-pixel distance from the left / right screen edge to the bounded rect.
// Both are 0 when bounding is off, when the screen is already no wider than
// the target aspect, and on the legacy 4:3 slots (mode_id < 8). mode_id is
// used rather than convert_use_mode because the latter is temporarily forced
// by ss_set_hack_mode() while the HUD layout is being recomputed.
void hud_bounds_insets(int *left, int *right) {
    int l = 0, r = 0;
    float ratio = hud_bound_ratio();

    if (ratio > 0.0f && mode_id >= 8 && grd_cap != NULL && grd_cap->h > 0) {
        int sw = grd_cap->w;
        int bw = (int)((float)grd_cap->h * ratio + 0.5f);
        // Ignore a 1px sliver from rounding (e.g. 854x480 vs 16:9).
        if (bw > 0 && sw - bw > 1) {
            l = (sw - bw) / 2;
            r = sw - bw - l;
        }
    }
    if (left)
        *left = l;
    if (right)
        *right = r;
}

static int hud_bound_ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++;
        b++;
    }
    return *a == *b;
}

// Parses a HUD-bounds value as used by the prefs file and the -hudbound
// launch option:
//   off | none | no | full | 0   -> mode OFF
//   4:3 / 16:9                   -> the matching preset
//   custom                       -> mode CUSTOM, keeps the stored custom ratio
//   W:H (e.g. 21:9, 16:10)       -> preset if it equals 4:3 / 16:9, else CUSTOM
//   decimal (e.g. 2.39)          -> CUSTOM, stored as (value * 100) : 100
// Returns 0 if the text can't be parsed (outputs untouched), 1 for a keyword,
// 2 for a ratio. On 2, *cw / *ch (if non-NULL) receive the parsed ratio even
// when it matched a preset, so callers can decide whether to keep it.
int hud_bounds_parse(const char *s, short *mode, short *cw, short *ch) {
    char buf[32];
    char *colon, *end;
    size_t n;
    long w, h;

    if (s == NULL || mode == NULL)
        return 0;
    while (*s == ' ' || *s == '\t')
        s++;
    n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        n--;
    if (n == 0 || n >= sizeof(buf))
        return 0;
    memcpy(buf, s, n);
    buf[n] = '\0';

    if (hud_bound_ieq(buf, "off") || hud_bound_ieq(buf, "none") || hud_bound_ieq(buf, "no") ||
        hud_bound_ieq(buf, "full") || hud_bound_ieq(buf, "0")) {
        *mode = HUD_BOUND_OFF;
        return 1;
    }
    if (hud_bound_ieq(buf, "custom")) {
        *mode = HUD_BOUND_CUSTOM;
        return 1;
    }

    colon = strchr(buf, ':');
    if (colon != NULL) {
        *colon = '\0';
        w = strtol(buf, &end, 10);
        if (end == buf || *end != '\0')
            return 0;
        h = strtol(colon + 1, &end, 10);
        if (end == colon + 1 || *end != '\0')
            return 0;
    } else {
        double v = strtod(buf, &end);
        if (end == buf || *end != '\0' || v <= 0.0 || v > 100.0)
            return 0;
        w = (long)(v * 100.0 + 0.5);
        h = 100;
    }

    if (w <= 0 || h <= 0 || w > 10000 || h > 10000)
        return 0;
    if ((float)w / (float)h < HUD_BOUND_RATIO_MIN || (float)w / (float)h > HUD_BOUND_RATIO_MAX)
        return 0;

    if (cw)
        *cw = (short)w;
    if (ch)
        *ch = (short)h;
    if (w * 3 == h * 4)
        *mode = HUD_BOUND_4_3;
    else if (w * 9 == h * 16)
        *mode = HUD_BOUND_16_9;
    else
        *mode = HUD_BOUND_CUSTOM;
    return 2;
}

// Register an element. Call before fullscreen entry.
int hud_register(grs_canvas *canvas, int src_w, int src_h, int anchor,
                 int margin_x, int margin_y, const char *name) {
    if (hud_num_elements >= HUD_MAX_ELEMENTS) return -1;
    hud_element *e = &hud_elements[hud_num_elements];
    e->name = name;
    e->canvas = canvas;
    e->src_w = src_w;
    e->src_h = src_h;
    e->anchor = anchor;
    e->margin_x = margin_x;
    e->margin_y = margin_y;
    e->dst_x = e->dst_y = e->dst_w = e->dst_h = 0;
    e->region = NULL;
    return hud_num_elements++;
}

// Compute destination rects for all elements based on the current
// screen size and HUD scale. Does not move regions or redraw.
void hud_layout(void) {
    int sw = grd_cap->w;
    int sh = grd_cap->h;
    for (int i = 0; i < hud_num_elements; i++) {
        hud_element *e = &hud_elements[i];
        e->dst_w = e->src_w * hud_scale_pct / 100;
        e->dst_h = e->src_h * hud_scale_pct / 100;
        switch (e->anchor) {
        case HUD_ANCHOR_TL:
            e->dst_x = e->margin_x;
            e->dst_y = e->margin_y;
            break;
        case HUD_ANCHOR_TR:
            e->dst_x = sw - e->dst_w - e->margin_x;
            e->dst_y = e->margin_y;
            break;
        case HUD_ANCHOR_BL:
            e->dst_x = e->margin_x;
            e->dst_y = sh - e->dst_h - e->margin_y;
            break;
        case HUD_ANCHOR_BR:
            e->dst_x = sw - e->dst_w - e->margin_x;
            e->dst_y = sh - e->dst_h - e->margin_y;
            break;
        case HUD_ANCHOR_T:
            e->dst_x = (sw - e->dst_w) / 2 + e->margin_x;
            e->dst_y = e->margin_y;
            break;
        case HUD_ANCHOR_B:
            e->dst_x = (sw - e->dst_w) / 2 + e->margin_x;
            e->dst_y = sh - e->dst_h - e->margin_y;
            break;
        case HUD_ANCHOR_L:
            e->dst_x = e->margin_x;
            e->dst_y = (sh - e->dst_h) / 2 + e->margin_y;
            break;
        case HUD_ANCHOR_R:
            e->dst_x = sw - e->dst_w - e->margin_x;
            e->dst_y = (sh - e->dst_h) / 2 + e->margin_y;
            break;
        }
        // HUD bounds: left/right-anchored elements hug the bounded rect.
        {
            int bound_l, bound_r;
            hud_bounds_insets(&bound_l, &bound_r);
            switch (e->anchor) {
            case HUD_ANCHOR_TL:
            case HUD_ANCHOR_BL:
            case HUD_ANCHOR_L:
                e->dst_x += bound_l;
                break;
            case HUD_ANCHOR_TR:
            case HUD_ANCHOR_BR:
            case HUD_ANCHOR_R:
                e->dst_x -= bound_r;
                break;
            }
        }
    }
}

// Blit all elements to the framebuffer. Called after the 3D view is
// rendered, and again whenever a HUD element changes.
//
// Uses gr_scale_bitmap (not ss_scale_bitmap) so the outer SS scaler does
// NOT touch the destination coordinates. That's what keeps the HUD
// aspect-correct.
void hud_redraw(void) {
    gr_push_canvas(grd_screen_canvas);
    uchar old_over = gr2ss_override;
    gr2ss_override = OVERRIDE_NONE;   // belt and suspenders
    for (int i = 0; i < hud_num_elements; i++) {
        hud_element *e = &hud_elements[i];
        gr_scale_bitmap(&e->canvas->bm, e->dst_x, e->dst_y, e->dst_w, e->dst_h);
    }
    gr2ss_override = old_over;
    gr_pop_canvas();
}

// Convert a screen-space mouse event to canvas space for an element.
// Handlers that live inside the element (like the MFD view callback)
// should call this before doing hit tests.
LGPoint hud_screen_to_canvas(hud_element *e, LGPoint screen) {
    LGPoint p;
    p.x = (screen.x - e->dst_x) * e->src_w / e->dst_w;
    p.y = (screen.y - e->dst_y) * e->src_h / e->dst_h;
    return p;
}


// ---------------------------------------------------------------------------
// Fullscreen MFD geometry
//
// In fullscreen mode, fullview_region (the parent region for all fullscreen
// overlays) is 320x200 logical (see fscrn_rect in screen.c).  Every fullscreen
// overlay -- MFDs, inventory, side icons, wrapper cursor region -- lives in
// that same 320x200 space, and the SS layer's SCONV_X/Y take the whole thing
// up to the real framebuffer resolution.
//
// So "making the HUD bigger" means: make the MFD's *destination rect* bigger
// within the 320x200 space, and stretch-blit the 74x58 MFD canvas into it.
// The view-window content (automap, weapon display, etc.) is unchanged; only
// the final blit is scaled.
//
// This struct holds the geometry for the fullscreen path.  The non-fullscreen
// path continues to use the MFD_VIEW_* / MFD_BTTN_* constants from mfddims.h
// verbatim, so the original 320x200 in-game HUD is pixel-for-pixel unchanged.
//
// The numbers here are in *logical* (320x200) space.  They are chosen so that
// each MFD unit (view + button strip) occupies roughly 1/3 of the logical
// width, with the two units hugging the left and right edges and a wide gap
// between them -- which on a real widescreen resolution becomes a proportional
// scale-up of the classic HUD layout.
//
// These are the knobs to tweak if the HUD feels too small/large on your
// display.  Adjust and rebuild; nothing else in this file depends on the
// specific values.

typedef struct {
    short view_lx, view_rx, view_y, view_w, view_h;   // view window rects
    short bttn_lx, bttn_rx, bttn_y, bttn_w, bttn_h;   // button strip rects
    short btn_sz, btn_blnk, btn_wid;                  // per-button geometry
    // Exact real-pixel rects for the fullscreen draw. The logical rects above
    // round-trip through SCONV (real_w/320); at 5120-wide that turns a 6px
    // button strip into 0 (6*320/5120 == 0.3 -> 0) and the strip (and its
    // region) collapse. Drawing straight from these real values avoids it.
    int   view_x_r[2];   // [MFD_LEFT], [MFD_RIGHT]; view window left edge (real px)
    int   view_y_r;
    int   view_w_r, view_h_r;
    int   bttn_x_r[2];   // button strip left edge (real px)
    int   bttn_y_r;
    int   bttn_w_r, bttn_h_r;
    int   btn_sz_r, btn_blnk_r, btn_wid_r;  // per-button metrics (real px)
} mfd_full_geom;

static mfd_full_geom mfd_fg;

// Uniform horizontal scale for the MFD view content (its vertical scale * 5:6),
// swapped into convert_x while a view draws so the view's text/art keeps correct
// proportions. convert_use_mode is untouched, so per-mode fonts still apply.
static fix mfd_block_ux = FIX_UNIT, mfd_block_uy = FIX_UNIT;
static fix mfd_block_ux_saved = FIX_UNIT, mfd_block_uy_saved = FIX_UNIT;
static int mfd_view_w = 74, mfd_view_h = 58; // real-pixel size of the view canvas
static int mfd_block_depth = 0;

static void mfd_block_scale_begin(void) {
    if (!full_game_3d)
        return;
    // Re-entrant: only the outermost begin saves/overrides, so nested draws
    // (e.g. an MFD expose that re-enters MFD drawing) can't clobber the saved
    // value and leak the override into the rest of the HUD.
    if (mfd_block_depth++ == 0) {
        // Swap BOTH scales so the view content scales uniformly with the
        // (hudScale-sized) destination rect. Only X was overridden before,
        // which left the content's Y at the mode's convert_y -- visible as
        // "contents shrunk only horizontally" once hudScale moved the box.
        mfd_block_ux_saved = convert_x[convert_type][convert_use_mode];
        mfd_block_uy_saved = convert_y[convert_type][convert_use_mode];
        convert_x[convert_type][convert_use_mode] = mfd_block_ux;
        convert_y[convert_type][convert_use_mode] = mfd_block_uy;
    }
}

static void mfd_block_scale_end(void) {
    if (mfd_block_depth <= 0)
        return;
    if (--mfd_block_depth == 0) {
        convert_x[convert_type][convert_use_mode] = mfd_block_ux_saved;
        convert_y[convert_type][convert_use_mode] = mfd_block_uy_saved;
    }
}

// Fullscreen MFD scale: 100 == "native-ish" (roughly 1.4x the original
// logical size).  Driven by gShockPrefs.hudScale (see Prefs.h).
static short mfd_user_scale = 100;

static void mfd_compute_fullscreen_geometry(void) {
    // ---- Layout units ----------------------------------------------------
    // Every fullscreen overlay is drawn in the engine's 320x200 "logical"
    // space and mapped to real pixels through SCONV_X/SCONV_Y. Those two
    // scales are independent (SCONV_X = real_w/320, SCONV_Y = real_h/200),
    // which reproduces the classic 4:3 look exactly: 320x200 art has a 5:6
    // pixel aspect, and 640x480 (SCONV_X=2.0, SCONV_Y=2.4 -> 5:6) matches it
    // pixel-for-pixel. On a 16:9 mode the two ratios diverge (1920x1080 gives
    // 6.0/5.4 = 10:9), so anything laid out in raw logical space is stretched
    // horizontally by 4/3.
    //
    // So lay the MFD out here in *real* pixels, using a single uniform scale
    // (the vertical one) with the 5:6 pixel aspect folded into X only, then
    // convert the results back to logical before storing them. The region
    // system (input) and the SCONV-applied blits therefore still land on
    // exactly these real-pixel rects, but the MFD keeps 4:3 proportions at
    // every resolution instead of stretching.
    // ----------------------------------------------------------------------

    // hudScale drives this, and every other fullscreen HUD element.
    hud_scale_pct  = hud_scale_clamp(gShockPrefs.hudScale ? gShockPrefs.hudScale : 100);
    mfd_user_scale = hud_scale_pct;

    const float real_w_f = (float)grd_cap->w;
    const float real_h_f = (float)grd_cap->h;
    const float par      = 5.0f / 6.0f;                                        // 320x200 pixel aspect
//    const float sy       = (real_h_f / 200.0f) * ((float)mfd_user_scale / 100.0f); // real px per logical-y
    const float sy = (float)mfd_user_scale / 100.0f;
    const float sx       = sy;                                                // isotropic: no 5:6 fold (see inventory_recompute_layout)

    // Element sizes in real pixels (same 74x58 / 6-wide / 9-and-2 buttons as
    // the original HUD, just measured on the real framebuffer now).
    int view_w   = (int)(74.0f * sx + 0.5f);
    int view_h   = (int)(58.0f * sy + 0.5f);
    int btn_w    = (int)( 6.0f * sx + 0.5f);
    int btn_sz   = (int)( 9.0f * sy + 0.5f);
    int btn_blnk = (int)( 2.0f * sy + 0.5f);
    if (btn_blnk < 1) btn_blnk = 1;

    // Sanity floors (real px).
    if (view_w < (int)(40.0f * sx)) view_w = (int)(40.0f * sx);
    if (view_h < (int)(30.0f * sy)) view_h = (int)(30.0f * sy);
    if (btn_w  < 1) btn_w  = 1;
    if (btn_sz < 1) btn_sz = 1;

    int margin = (int)(2.0f * sx + 0.5f);
    if (margin < 1) margin = 1;
    int gap = (int)(1.0f * sx + 0.5f);
    if (gap < 1) gap = 1;

    // Left MFD: button strip on the outer (left) edge, view window inboard.
    int left_btn_x  = margin;
    int left_view_x = left_btn_x + btn_w + gap;
    int left_view_r = left_view_x + view_w;

    // Right MFD: mirror about the screen centre.
    int right_btn_r  = (int)real_w_f - margin;
    int right_btn_x  = right_btn_r - btn_w;
    int right_view_r = right_btn_x - gap;
    int right_view_x = right_view_r - view_w;

    // HUD bounds: pull both MFD units in from the screen edges so they hug the
    // bounded rect instead (no-op unless a bound is set).
    {
        int bound_l, bound_r;
        hud_bounds_insets(&bound_l, &bound_r);
        left_btn_x += bound_l;
        left_view_x += bound_l;
        left_view_r += bound_l;
        right_btn_r -= bound_r;
        right_btn_x -= bound_r;
        right_view_r -= bound_r;
        right_view_x -= bound_r;
    }

    // If the two view windows would overlap (very narrow screen), shrink them.
    if (left_view_r > right_view_x) {
        int overlap = left_view_r - right_view_x;
        view_w -= overlap / 2 + 1;
        if (view_w < (int)(40.0f * sx)) view_w = (int)(40.0f * sx);
        left_view_r  = left_view_x + view_w;
        right_view_x = right_view_r - view_w;
    }

    // Bottom-anchored, level with the inventory/message strip.
    int view_y = (int)real_h_f - margin - view_h;
    int btn_y  = view_y;

    // Back to logical space so the region rects (input) and the SCONV blits
    // land on exactly these real-pixel rectangles.
#define R2LX(px) ((short)((px) * 320.0f / real_w_f))
#define R2LY(py) ((short)((py) * 200.0f / real_h_f))
// Ceil variants for REGION SIZES: a region must fully cover its drawn area,
// so its logical width/height rounds UP (origins still truncate, so the
// region starts where the draw starts). Truncating sizes left slivers of
// drawn strip/view outside the input region at extreme resolutions.
#define R2LXC(px) ((short)(((px) * 320.0f / real_w_f) + 0.999f))
#define R2LYC(py) ((short)(((py) * 200.0f / real_h_f) + 0.999f))

    mfd_fg.view_lx = R2LX(left_view_x);
    mfd_fg.view_rx = R2LX(right_view_x);
    mfd_fg.view_y  = R2LY(view_y);
    mfd_fg.view_w  = R2LXC(view_w);
    mfd_fg.view_h  = R2LYC(view_h);

    mfd_fg.bttn_lx = R2LX(left_btn_x);
    mfd_fg.bttn_rx = R2LX(right_btn_x);
    mfd_fg.bttn_y  = R2LY(btn_y);
    mfd_fg.bttn_w  = R2LXC(btn_w);
    // The region rects live in 320x200 logical space, where a narrow strip
    // rounds to 0 at extreme widths (5120: 6px * 320/5120 == 0), leaving the
    // strip unclickable. Clamp to 1 logical unit so the input region stays live
    // (the draw uses the exact real rects above, so it is unaffected).
    if (mfd_fg.bttn_w < 1) mfd_fg.bttn_w = 1;

    // Per-button metrics first, then derive the strip height from those *same*
    // logical values.
    //
    // IMPORTANT: do not set bttn_h = R2LY(btn_h). Converting the real strip
    // height and the real button metrics back to logical independently can
    // desync them (e.g. R2LY(68) + R2LY(15) rounds down to 5*12+4*2 = 68
    // while R2LY(400) rounds to 74), leaving the strip region a couple of
    // logical px taller than the buttons inside it. mfd_button_callback then
    // computes which_button = rel_y / (btn_sz + btn_blnk), which for a click
    // in that slack can reach MFD_NUM_VIRTUAL_SLOTS -- out of range for
    // cursor_strings[], whose OOB read was the segfault in make_popup_cursor.
    mfd_fg.btn_sz   = R2LY(btn_sz);
    mfd_fg.btn_blnk = R2LY(btn_blnk);
    mfd_fg.btn_wid  = R2LX(btn_w) - 2;
    if (mfd_fg.btn_sz < 1) mfd_fg.btn_sz = 1;
    if (mfd_fg.btn_blnk < 1) mfd_fg.btn_blnk = 1;
    if (mfd_fg.btn_wid < 1) mfd_fg.btn_wid = 1;

    // Region/rect height: ceil the REAL strip height back to logical space so
    // the input region always covers the drawn strip. The old per-button sum
    // (N*R2LY(btn_sz) + (N-1)*R2LY(btn_blnk)) truncated up to one SCONV_Y per
    // term, leaving the region short of the strip bottom by up to ~9*SCONV_Y
    // real px, which swallowed the lowest buttons' hit zones. The +1 guards the
    // truncation of the region origin (bttn_y = R2LY(btn_y)). A slightly taller
    // region is safe: mfd_button_callback maps clicks in real pixels and
    // range-checks which_button against MFD_NUM_VIRTUAL_SLOTS.
    mfd_fg.bttn_h = R2LYC((float)(MFD_NUM_VIRTUAL_SLOTS * btn_sz +
                                  (MFD_NUM_VIRTUAL_SLOTS - 1) * btn_blnk)) + 1;

    // The view is drawn with this uniform scale and blitted 1:1, so the canvas
    // is exactly the view's real size (see mfd_update_screen_mode /
    // fullscreen_refresh_mfd).
    mfd_view_w = view_w;
    mfd_view_h = view_h;
    mfd_block_ux = fix_div(fix_make(view_w, 0), fix_make(74, 0));
    mfd_block_uy = fix_div(fix_make(view_h, 0), fix_make(58, 0));

    // Exact real-pixel geometry for the fullscreen draw (no logical round-trip).
    mfd_fg.view_x_r[MFD_LEFT]  = left_view_x;
    mfd_fg.view_x_r[MFD_RIGHT] = right_view_x;
    mfd_fg.view_y_r = view_y;
    mfd_fg.view_w_r = view_w;
    mfd_fg.view_h_r = view_h;
    mfd_fg.bttn_x_r[MFD_LEFT]  = left_btn_x;
    mfd_fg.bttn_x_r[MFD_RIGHT] = right_btn_x;
    mfd_fg.bttn_y_r = btn_y;
    mfd_fg.bttn_w_r = btn_w;
    mfd_fg.bttn_h_r = MFD_NUM_VIRTUAL_SLOTS * btn_sz + (MFD_NUM_VIRTUAL_SLOTS - 1) * btn_blnk;
    mfd_fg.btn_sz_r = btn_sz;
    mfd_fg.btn_blnk_r = btn_blnk;
    mfd_fg.btn_wid_r = btn_w;

#undef R2LX
#undef R2LY
#undef R2LXC
#undef R2LYC

//INFO("MFD geom: view_lx=%d view_rx=%d view_w=%d view_h=%d btn_lx=%d btn_rx=%d",
//     mfd_fg.view_lx, mfd_fg.view_rx, mfd_fg.view_w, mfd_fg.view_h,
//     mfd_fg.bttn_lx, mfd_fg.bttn_rx);
//INFO("grd_cap=%dx%d  fullview_region rect=%d,%d..%d,%d",
//     grd_cap->w, grd_cap->h,
//     fullview_region->r->ul.x, fullview_region->r->ul.y,
//     fullview_region->r->lr.x, fullview_region->r->lr.y);

}

// Applies the geometry in mfd_fg to the live MFD rects and to the four
// fullscreen regions. Safe to call repeatedly; region_move clips against the
// parent (fullview_region) automatically. Requires the fullscreen regions to
// already exist (created in screen_init_mfd(TRUE)).
// z-order for the fullscreen MFD regions (both view windows and both button
// strips). It must sit above the "raised" z (2) used by full_raise_region()
// and by the inventory / lean-meter / view-wrapper regions: with HUD bounds on,
// the MFD units move inboard and their logical region rects overlap the
// inventory panel region's (much wider) logical rect, so at equal z the
// inventory region swallowed the strips' clicks (symptom: bounds on -> right
// strip dead). The overlap is logical only -- the inventory panel is drawn far
// inboard of the strip -- so raising the MFD regions above it just restores the
// correct owner without affecting anything the user can actually see.
#define MFD_REGION_Z 3

static void mfd_apply_fullscreen_geometry(void) {
    // Region rects must fully contain the real drawn rects. Convert each real
    // span to logical by FLOORING the low edge and CEILING the high edge; a
    // plain width conversion can fall a logical unit short when the low edge's
    // fraction pushes the high edge past a logical boundary. That shortfall
    // left only part of the button strip inside the input region at some HUD
    // scales (5120x1000 at 200%: strip real x 3213..3223, region x 3200..3216
    // -> only ~3px clickable, i.e. the strip "turned thin").
    const float l2sx = 320.0f / (float)grd_cap->w;
    const float l2sy = 200.0f / (float)grd_cap->h;
#define RLX0(px) ((short)((px) * l2sx))
#define RLX1(px) ((short)((px) * l2sx + 0.999f))
#define RLY0(py) ((short)((py) * l2sy))
#define RLY1(py) ((short)((py) * l2sy + 0.999f))

    int p;
    for (p = 0; p < NUM_MFDS; p++) {
        LGRect vr, br;

        vr.ul.x = RLX0(mfd_fg.view_x_r[p]);
        vr.ul.y = RLY0(mfd_fg.view_y_r);
        vr.lr.x = RLX1(mfd_fg.view_x_r[p] + mfd_fg.view_w_r);
        vr.lr.y = RLY1(mfd_fg.view_y_r + mfd_fg.view_h_r);

        br.ul.x = RLX0(mfd_fg.bttn_x_r[p]);
        br.ul.y = RLY0(mfd_fg.bttn_y_r);
        br.lr.x = RLX1(mfd_fg.bttn_x_r[p] + mfd_fg.bttn_w_r);
        br.lr.y = RLY1(mfd_fg.bttn_y_r + mfd_fg.bttn_h_r);

        // Resize as well as move: the regions are created once (in
        // screen_init_mfd) but the layout is recomputed on every HUD-scale /
        // bounds change, so the size must follow too.
        region_move  (&(mfd[p].reg2), vr.ul.x, vr.ul.y, MFD_REGION_Z);
        region_resize(&(mfd[p].reg2), vr.lr.x - vr.ul.x, vr.lr.y - vr.ul.y);
        // Button strips are moved last so they win any one-unit overlap with
        // the view rect (same z).
        region_move  (&(mfd[p].bttn.reg2), br.ul.x, br.ul.y, MFD_REGION_Z);
        region_resize(&(mfd[p].bttn.reg2), br.lr.x - br.ul.x, br.lr.y - br.ul.y);
    }
#undef RLX0
#undef RLX1
#undef RLY0
#undef RLY1

    mfd[MFD_LEFT].rect.ul.x  = mfd_fg.view_lx;
    mfd[MFD_LEFT].rect.ul.y  = mfd_fg.view_y;
    mfd[MFD_LEFT].rect.lr.x  = mfd_fg.view_lx + mfd_fg.view_w;
    mfd[MFD_LEFT].rect.lr.y  = mfd_fg.view_y  + mfd_fg.view_h;
    mfd[MFD_RIGHT].rect.ul.x = mfd_fg.view_rx;
    mfd[MFD_RIGHT].rect.ul.y = mfd_fg.view_y;
    mfd[MFD_RIGHT].rect.lr.x = mfd_fg.view_rx + mfd_fg.view_w;
    mfd[MFD_RIGHT].rect.lr.y = mfd_fg.view_y  + mfd_fg.view_h;

    mfd[MFD_LEFT].bttn.rect.ul.x  = mfd_fg.bttn_lx;
    mfd[MFD_LEFT].bttn.rect.ul.y  = mfd_fg.bttn_y;
    mfd[MFD_LEFT].bttn.rect.lr.x  = mfd_fg.bttn_lx + mfd_fg.bttn_w;
    mfd[MFD_LEFT].bttn.rect.lr.y  = mfd_fg.bttn_y  + mfd_fg.bttn_h;
    mfd[MFD_RIGHT].bttn.rect.ul.x = mfd_fg.bttn_rx;
    mfd[MFD_RIGHT].bttn.rect.ul.y = mfd_fg.bttn_y;
    mfd[MFD_RIGHT].bttn.rect.lr.x = mfd_fg.bttn_rx + mfd_fg.bttn_w;
    mfd[MFD_RIGHT].bttn.rect.lr.y = mfd_fg.bttn_y  + mfd_fg.bttn_h;
}

// Called when the user changes the HUD scale. Clamps, pushes the new value to
// every fullscreen HUD element and forces the fullscreen overlay to redraw at
// the new size.
void mfd_set_hud_scale(short pct) {
    hud_scale_pct  = hud_scale_clamp(pct);
    mfd_user_scale = hud_scale_pct;
    if (full_game_3d) {
        mfd_compute_fullscreen_geometry();
        mfd_apply_fullscreen_geometry();
        mfd_force_update();
        // The inventory and vitals draw to the scaled layout (inv_real_* /
        // vitals_* now include hud_scale_factor()), but their canvases and
        // regions were built from the old size. Re-run the same refreshes the
        // mode-change path uses (change_svga_screen_mode ->
        // inventory_update_screen_mode / status_vitals_update) so the hitboxes
        // and block canvases follow the boxes.
        inventory_update_screen_mode();
	init_all_side_icons();
	side_icon_rebuild_regions();
        status_vitals_update(TRUE);
	change_svga_cursors();
	lean_meter_update_screen_mode();
        fullscreen_overlay();
    }
}

// Re-lays-out the fullscreen HUD after a bounds change. Reuses the HUD-scale
// refresh (geometry -> regions -> canvases -> redraw), which consults
// hud_bounds_insets(). Outside fullscreen it does nothing; the new bounds are
// picked up when the fullscreen HUD is next built.
void hud_bounds_apply(void) { mfd_set_hud_scale(hud_scale_pct); }

// Sets the bounds mode (HUD_BOUND_*) and, if cw/ch are both > 0, the custom
// ratio, then applies it. Does not write the prefs file -- the caller decides
// whether to SavePrefs().
void hud_bounds_set(short mode, short cw, short ch) {
    if (mode < HUD_BOUND_OFF || mode > HUD_BOUND_CUSTOM)
        mode = HUD_BOUND_OFF;
    gShockPrefs.hudBoundMode = mode;
    if (cw > 0 && ch > 0) {
        gShockPrefs.hudBoundCustomW = cw;
        gShockPrefs.hudBoundCustomH = ch;
    }
    hud_bounds_apply();
}

// off -> 4:3 -> 16:9 -> custom -> off. Handy for a menu button or hotkey.
void hud_bounds_cycle(void) { hud_bounds_set((short)((gShockPrefs.hudBoundMode + 1) % 4), 0, 0); }


// -----------
// Prototypes
// -----------
void mfd_set_slot(ubyte mfd_id, ubyte newSlot, uchar OnOff);
void mfd_draw_all_buttons(ubyte mfd_id);
errtype mfd_clear_all();

void mfd_clear_func(ubyte func_id);

uchar mfd_object_cursor_handler(uiEvent *ev, LGRegion *, int which_mfd);

void mfd_draw_button(ubyte mfd_id, ubyte b);
void mfd_select_button(int which_panel, int which_button);

void mfd_default_mru(uchar func);
void set_mfd_from_defaults(int mfd_id, uchar func, uchar slot);

static void mfd_remap_event_pos(MFD *m, LGRegion *r, uiEvent *e);

// Maps an event whose position is in the fullview region's 320x200 space into
// the MFD view's 74x58 canvas space, using the drawn (real-pixel) view rect.
// Used by the minigames, which sample the mouse directly (not via a region
// event) -- without this their synthetic events used a different scale than
// real ones and the game reacted erratically. Callers must build the synthetic
// event's position as a 320x200 coordinate (ui_mouse_get_xy() already does).
void mfd_view_event_to_canvas(MFD *m, uiEvent *e);

void hud_dump_layout(void);

// HUD overlay module
int  hud_register(grs_canvas *canvas, int src_w, int src_h, int anchor,
                  int margin_x, int margin_y, const char *name);
void hud_layout(void);
void hud_redraw(void);
LGPoint hud_screen_to_canvas(hud_element *e, LGPoint screen);

// KLC  dbg_mfd_state used to be here.

// ------------------
//    INITIALIZERS
// ------------------

// ---------------------------------------------------------------------------
// init_newmfd()
//
// Initialize the MFD system (called from init_all() in init.c)

void init_newmfd() {
    ubyte i;

    // Set the default MFD function
    set_default_to_func(MFD_LEFT, MFD_EMPTY_FUNC);
    set_default_to_func(MFD_RIGHT, MFD_EMPTY_FUNC);

    // Now set actual MFD slots to point at virtual slots
    for (i = 0; i < NUM_MFDS; i++)
        mfd[i].id = i;

    player_struct.mfd_current_slots[MFD_LEFT] = MFD_WEAPON_SLOT;
    player_struct.mfd_current_slots[MFD_RIGHT] = MFD_ITEM_SLOT;

    for (i = 0; i < MFD_NUM_VIRTUAL_SLOTS; i++) {
        set_mfd_to_slot(MFD_LEFT, i, i);
        set_mfd_to_slot(MFD_RIGHT, i, i);
    }

    for (i = 0; i < MFD_NUM_FUNCS; i++)
        if (mfd_funcs[i].flags & MFD_INCREMENTAL)
            player_struct.mfd_func_status[i] |= 1 << 4;

    chg_set_flg(MFD_UPDATE);

    return;
}

// ---------------------------------------------------------------------------
// init_newmfd_button_cursors()
//
// Initialize the twelve goofy cursors, each of which hovers over an MFD button,
// as spec'd in last nights warren/artist/programmers meeting (SPAZ 8/5)

static char *cursor_strings[MFD_NUM_BTTNS];
static char cursor_strbuf[128];

void mfd_language_change(void) {
    load_string_array(REF_STR_MFDCursor, cursor_strings, cursor_strbuf, sizeof(cursor_strbuf), MFD_NUM_BTTNS);
}

void init_newmfd_button_cursors() {
    int i;
    mfd_language_change();
    for (i = 0; i < NUM_MFDS; i++) {
        LGCursor *c = &mfd_bttn_cursors[i];
        grs_bitmap *bm = &mfd_bttn_bitmaps[i];
        LGPoint offset = {0, 0};
        make_popup_cursor(c, bm, cursor_strings[i], i, TRUE, offset);
    }
}

// ---------------------------------------------------------------------------
// screen_init_mfd_draw()
//
// Basically, just draw the friggin' buttons and set mfd's to their
// first slot. (called from screen_start() in screen.c)

void screen_init_mfd_draw() {
    mfd_set_slot(MFD_LEFT, mfd_index(MFD_LEFT), TRUE);
    mfd_set_slot(MFD_RIGHT, mfd_index(MFD_RIGHT), TRUE);

    mfd_draw_all_buttons(MFD_LEFT);
    mfd_draw_all_buttons(MFD_RIGHT);

    return;
}

#ifdef SVGA_SUPPORT
#define MAX_WD(x) (fix_int(fix_mul_div(fix_make((x), 0), fix_make(1024, 0), fix_make(320, 0))))
#define MAX_HT(y) (fix_int(fix_mul_div(fix_make((y), 0), fix_make(768, 0), fix_make(200, 0))))
#endif

// ---------------------------------------------------------------------------
// screen_init_mfd();
//
// Declare the appropriate regions for the MFD's and their button panels.
// (called from screen_start() in screen.c)

void screen_init_mfd(uchar fullscrn) {
    static uchar done_init = FALSE;
    FrameDesc *f;
    int id;
    int lval, rval;

    lval = MFD_LEFT;  // Screen callbacks need to know their
    rval = MFD_RIGHT; // left from their right, thusly

//    // Set up the Rect structures for MFD screen-space
//
//    // Left View Window
//    mfdL.rect.ul.x = MFD_VIEW_LFTX;
//    mfdL.rect.ul.y = MFD_VIEW_Y;
//    mfdL.rect.lr.x = MFD_VIEW_LFTX + MFD_VIEW_WID;
//    mfdL.rect.lr.y = MFD_VIEW_Y + MFD_VIEW_HGT;
//
//    // Right View Window
//    mfdR.rect.ul.x = MFD_VIEW_RGTX;
//    mfdR.rect.ul.y = MFD_VIEW_Y;
//    mfdR.rect.lr.x = MFD_VIEW_RGTX + MFD_VIEW_WID;
//    mfdR.rect.lr.y = MFD_VIEW_Y + MFD_VIEW_HGT;
//
//    // Left Button Panel
//    mfdL.bttn.rect.ul.x = MFD_BTTN_LFTX;
//    mfdL.bttn.rect.ul.y = MFD_BTTN_Y;
//    mfdL.bttn.rect.lr.x = MFD_BTTN_LFTX + MFD_BTTN_WID;
//    mfdL.bttn.rect.lr.y = MFD_BTTN_Y + MFD_BTTN_HGT;
//
//    // Right Button Panel
//    mfdR.bttn.rect.ul.x = MFD_BTTN_RGTX;
//    mfdR.bttn.rect.ul.y = MFD_BTTN_Y;
//   mfdR.bttn.rect.lr.x = MFD_BTTN_RGTX + MFD_BTTN_WID;
//    mfdR.bttn.rect.lr.y = MFD_BTTN_Y + MFD_BTTN_HGT;

void hud_dump_layout(void) {
    INFO("HUD LAYOUT DUMP");
    INFO("  invdims:  INV_PANEL=(%d,%d) %dx%d",
         INVENTORY_PANEL_X, INVENTORY_PANEL_Y,
         INVENTORY_PANEL_WIDTH, INVENTORY_PANEL_HEIGHT);
    INFO("  invdims:  GAME_MSG=(%d,%d) %dx%d",
         GAME_MESSAGE_X, GAME_MESSAGE_Y, GAME_MESSAGE_W, GAME_MESSAGE_H);
    INFO("  mfddims:  MFD_VIEW_L=(%d,%d) %dx%d  R=(%d,%d) %dx%d",
         MFD_VIEW_LFTX, MFD_VIEW_Y, MFD_VIEW_WID, MFD_VIEW_HGT,
         MFD_VIEW_RGTX, MFD_VIEW_Y, MFD_VIEW_WID, MFD_VIEW_HGT);
    INFO("  mfddims:  MFD_BTTN_L=(%d,%d) %dx%d  R=(%d,%d) %dx%d",
         MFD_BTTN_LFTX, MFD_BTTN_Y, MFD_BTTN_WID, MFD_BTTN_HGT,
         MFD_BTTN_RGTX, MFD_BTTN_Y, MFD_BTTN_WID, MFD_BTTN_HGT);
    INFO("  screen:   grd_cap=%dx%d  convert_use_mode=%d  full_game_3d=%d",
         grd_cap->w, grd_cap->h, convert_use_mode, full_game_3d);

    extern LGRegion *inventory_region;
    extern LGRegion *pagebutton_region;
    if (inventory_region && inventory_region->r) {
        INFO("  inv region rect: (%d,%d)-(%d,%d)",
             inventory_region->r->ul.x, inventory_region->r->ul.y,
             inventory_region->r->lr.x, inventory_region->r->lr.y);
    }
    if (pagebutton_region && pagebutton_region->r) {
        INFO("  pagebtn region rect: (%d,%d)-(%d,%d)",
             pagebutton_region->r->ul.x, pagebutton_region->r->ul.y,
             pagebutton_region->r->lr.x, pagebutton_region->r->lr.y);
    }
    INFO("  mfd[0].rect: (%d,%d)-(%d,%d)", mfd[0].rect.ul.x, mfd[0].rect.ul.y,
         mfd[0].rect.lr.x, mfd[0].rect.lr.y);
    INFO("  mfd[1].rect: (%d,%d)-(%d,%d)", mfd[1].rect.ul.x, mfd[1].rect.ul.y,
         mfd[1].rect.lr.x, mfd[1].rect.lr.y);
}


    // Set up the Rect structures for MFD screen-space
    if (fullscrn) {
        // Fullscreen: compute geometry in 320x200 logical space and
        // place the four rects there.  fullview_region (the parent)
        // is 320x200, and the SS layer takes the whole thing up to
        // the real framebuffer size.
        mfd_compute_fullscreen_geometry();

        mfdL.rect.ul.x = mfd_fg.view_lx;
        mfdL.rect.ul.y = mfd_fg.view_y;
        mfdL.rect.lr.x = mfd_fg.view_lx + mfd_fg.view_w;
        mfdL.rect.lr.y = mfd_fg.view_y  + mfd_fg.view_h;

        mfdR.rect.ul.x = mfd_fg.view_rx;
        mfdR.rect.ul.y = mfd_fg.view_y;
        mfdR.rect.lr.x = mfd_fg.view_rx + mfd_fg.view_w;
        mfdR.rect.lr.y = mfd_fg.view_y  + mfd_fg.view_h;

        mfdL.bttn.rect.ul.x = mfd_fg.bttn_lx;
        mfdL.bttn.rect.ul.y = mfd_fg.bttn_y;
        mfdL.bttn.rect.lr.x = mfd_fg.bttn_lx + mfd_fg.bttn_w;
        mfdL.bttn.rect.lr.y = mfd_fg.bttn_y  + mfd_fg.bttn_h;

        mfdR.bttn.rect.ul.x = mfd_fg.bttn_rx;
        mfdR.bttn.rect.ul.y = mfd_fg.bttn_y;
        mfdR.bttn.rect.lr.x = mfd_fg.bttn_rx + mfd_fg.bttn_w;
        mfdR.bttn.rect.lr.y = mfd_fg.bttn_y  + mfd_fg.bttn_h;
    } else {
        // Non-fullscreen: original 320x200 in-game layout, verbatim.
        mfdL.rect.ul.x = MFD_VIEW_LFTX;
        mfdL.rect.ul.y = MFD_VIEW_Y;
        mfdL.rect.lr.x = MFD_VIEW_LFTX + MFD_VIEW_WID;
        mfdL.rect.lr.y = MFD_VIEW_Y + MFD_VIEW_HGT;

        mfdR.rect.ul.x = MFD_VIEW_RGTX;
        mfdR.rect.ul.y = MFD_VIEW_Y;
        mfdR.rect.lr.x = MFD_VIEW_RGTX + MFD_VIEW_WID;
        mfdR.rect.lr.y = MFD_VIEW_Y + MFD_VIEW_HGT;

        mfdL.bttn.rect.ul.x = MFD_BTTN_LFTX;
        mfdL.bttn.rect.ul.y = MFD_BTTN_Y;
        mfdL.bttn.rect.lr.x = MFD_BTTN_LFTX + MFD_BTTN_WID;
        mfdL.bttn.rect.lr.y = MFD_BTTN_Y + MFD_BTTN_HGT;

        mfdR.bttn.rect.ul.x = MFD_BTTN_RGTX;
        mfdR.bttn.rect.ul.y = MFD_BTTN_Y;
        mfdR.bttn.rect.lr.x = MFD_BTTN_RGTX + MFD_BTTN_WID;
        mfdR.bttn.rect.lr.y = MFD_BTTN_Y + MFD_BTTN_HGT;
    }

    // Now, actually create the four regions, and add handlers
    if (!fullscrn) {
        macro_region_create(root_region, &(mfdL.reg), &(mfdL.rect));
        macro_region_create(root_region, &(mfdR.reg), &(mfdR.rect));
        macro_region_create(root_region, &(mfdL.bttn.reg), &(mfdL.bttn.rect));
        macro_region_create(root_region, &(mfdR.bttn.reg), &(mfdR.bttn.rect));

        uiInstallRegionHandler(&(mfdL.reg), (UI_EVENT_MOUSE | UI_EVENT_MOUSE_MOVE), mfd_view_callback, MFD_LEFT,
                               &id);
        uiInstallRegionHandler(&(mfdR.reg), (UI_EVENT_MOUSE | UI_EVENT_MOUSE_MOVE), mfd_view_callback,
                               MFD_RIGHT, &id);

        uiInstallRegionHandler(&(mfdL.bttn.reg), (UI_EVENT_MOUSE | UI_EVENT_MOUSE_MOVE), mfd_button_callback,
                               MFD_LEFT, &id);
        uiInstallRegionHandler(&(mfdR.bttn.reg), (UI_EVENT_MOUSE | UI_EVENT_MOUSE_MOVE), mfd_button_callback,
                               MFD_RIGHT, &id);

        // region_create() normalises the LGRect it is handed, which can leave the
        // MFD view rect smaller than MFD_VIEW_WID x MFD_VIEW_HGT. Re-assert the
        // exact logical view rect so the view content composes into the full view
        // space and fills its box (mfd_update_display blits the SCONV-sized canvas
        // into this rect).
        mfdL.rect.ul.x = MFD_VIEW_LFTX;
        mfdL.rect.ul.y = MFD_VIEW_Y;
        mfdL.rect.lr.x = MFD_VIEW_LFTX + MFD_VIEW_WID;
        mfdL.rect.lr.y = MFD_VIEW_Y + MFD_VIEW_HGT;
        mfdR.rect.ul.x = MFD_VIEW_RGTX;
        mfdR.rect.ul.y = MFD_VIEW_Y;
        mfdR.rect.lr.x = MFD_VIEW_RGTX + MFD_VIEW_WID;
        mfdR.rect.lr.y = MFD_VIEW_Y + MFD_VIEW_HGT;
    } else {
        uiCursorStack *cs;
        macro_region_create(fullview_region, &(mfdL.reg2), &(mfdL.rect));
        uiGetRegionCursorStack(&(mfdL.reg), &cs);
        uiSetRegionCursorStack(&(mfdL.reg2), cs);
        macro_region_create(fullview_region, &(mfdR.reg2), &(mfdR.rect));
        uiGetRegionCursorStack(&(mfdR.reg), &cs);
        uiSetRegionCursorStack(&(mfdR.reg2), cs);
        region_create(fullview_region, &(mfdL.bttn.reg2), &(mfdL.bttn.rect), 2, 0, REG_USER_CONTROLLED, NULL, NULL,
                      NULL, NULL);
        uiGetRegionCursorStack(&(mfdL.bttn.reg), &cs);
        uiSetRegionCursorStack(&(mfdL.bttn.reg2), cs);
        region_create(fullview_region, &(mfdR.bttn.reg2), &(mfdR.bttn.rect), 2, 0, REG_USER_CONTROLLED, NULL, NULL,
                      NULL, NULL);
        uiGetRegionCursorStack(&(mfdR.bttn.reg), &cs);
        uiSetRegionCursorStack(&(mfdR.bttn.reg2), cs);

        uiInstallRegionHandler(&(mfdL.reg2), (UI_EVENT_MOUSE | UI_EVENT_MOUSE_MOVE), mfd_view_callback_full,
                               MFD_LEFT, &id);
        uiInstallRegionHandler(&(mfdR.reg2), (UI_EVENT_MOUSE | UI_EVENT_MOUSE_MOVE), mfd_view_callback_full,
                               MFD_RIGHT, &id);

        uiInstallRegionHandler(&(mfdL.bttn.reg2), (UI_EVENT_MOUSE | UI_EVENT_MOUSE_MOVE), mfd_button_callback,
                               MFD_LEFT, &id);
        uiInstallRegionHandler(&(mfdR.bttn.reg2), (UI_EVENT_MOUSE | UI_EVENT_MOUSE_MOVE), mfd_button_callback,
                               MFD_RIGHT, &id);
    }

    if (fullscrn) {
        INFO("region after create: mfdL.reg2.r=(%d,%d)-(%d,%d) mfdR.reg2.r=(%d,%d)-(%d,%d)",
             mfdL.reg2.r->ul.x, mfdL.reg2.r->ul.y, mfdL.reg2.r->lr.x, mfdL.reg2.r->lr.y,
             mfdR.reg2.r->ul.x, mfdR.reg2.r->ul.y, mfdR.reg2.r->lr.x, mfdR.reg2.r->lr.y);
    }

    if (!done_init) {
        done_init = TRUE;

        // CC: These bytes get made on the fly now
        mfd_canvas_bits = (uchar *)malloc(MAX_WD(MFD_VIEW_WID) * MAX_HT(MFD_VIEW_HGT));

        // Pull in the background bitmap
        f = RefLock(REF_IMG_bmBlankMFD);
        mfd_background = f->bm;
        mfd_background.bits = (uchar *)malloc(MAX_WD(MFD_VIEW_WID) * MAX_HT(MFD_VIEW_HGT));

        LG_memcpy(mfd_background.bits, (f + 1), f->bm.w * f->bm.h);
        RefUnlock(REF_IMG_bmBlankMFD);

        gr_init_canvas(&_offscreen_mfd, mfd_canvas_bits, BMT_FLAT8, MFD_VIEW_WID, MFD_VIEW_HGT);
        gr_init_canvas(&_fullscreen_mfd, mfd_background.bits, BMT_FLAT8, MFD_VIEW_WID, MFD_VIEW_HGT);
        pmfd_canvas = &_offscreen_mfd;
        init_newmfd_button_cursors();
        mfd_init_funcs();
    }

    if (!done_init) {
        hud_dump_layout();
    }

    return;
}

#ifdef SVGA_SUPPORT
errtype mfd_update_screen_mode() {
    if (full_game_3d) {
        mfd_compute_fullscreen_geometry();
        // The geometry is now expressed in real pixels, so it depends on the
        // current framebuffer size and must be re-applied here (not just at
        // first region creation in screen_init_mfd) after every resolution
        // change -- otherwise a mode switch leaves the regions/rects pointing
        // at the previous resolution's layout.
        mfd_apply_fullscreen_geometry();
    }
    if (convert_use_mode == 0) {
        gr_init_canvas(&_offscreen_mfd, mfd_canvas_bits, BMT_FLAT8, MFD_VIEW_WID, MFD_VIEW_HGT);
        gr_init_canvas(&_fullscreen_mfd, mfd_background.bits, BMT_FLAT8, MFD_VIEW_WID, MFD_VIEW_HGT);
    } else {

        int new_width  = full_game_3d ? mfd_view_w : SCONV_X(MFD_VIEW_WID);
        int new_height = full_game_3d ? mfd_view_h : SCONV_Y(MFD_VIEW_HGT);
        size_t need = (size_t)new_width * (size_t)new_height;

        // Do NOT free+malloc here in the common case. wrapper.c overlays the
        // options panel's OButtons array on _offscreen_mfd.bm.bits (which
        // aliases mfd_canvas_bits), so reallocating this buffer makes the
        // OButtons pointer dangle -- any option-panel access after a mode
        // change reads/writes freed memory. Track capacity and only grow,
        // never shrink or move. The initial allocation in screen_init_mfd()
        // is MAX_WD(MFD_VIEW_WID) * MAX_HT(MFD_VIEW_HGT), larger than any
        // runtime size will ever need, so this branch won't fire in practice.
        static size_t mfd_canvas_capacity = 0;
        static size_t mfd_bg_capacity = 0;

        if (mfd_canvas_capacity == 0) {
            mfd_canvas_capacity = (size_t)MAX_WD(MFD_VIEW_WID) * MAX_HT(MFD_VIEW_HGT);
            mfd_bg_capacity     = mfd_canvas_capacity;
        }

        if (need > mfd_canvas_capacity) {
            free(mfd_canvas_bits);
            mfd_canvas_bits = (uchar *)malloc(need);
            mfd_canvas_capacity = need;
        }
        if (need > mfd_bg_capacity) {
            free(mfd_background.bits);
            mfd_background.bits = (uchar *)malloc(need);
            mfd_bg_capacity = need;
        }

        // Copy the background bytes, clamped: source art is 74x58, the
        // destination may be smaller in non-fullscreen modes.
        {
            grs_bitmap *bm = lock_bitmap_from_ref(REF_IMG_bmBlankMFD);
            size_t dst_bytes = (size_t)new_width * new_height;
            size_t src_bytes = (size_t)bm->w * bm->h;
            size_t n = dst_bytes < src_bytes ? dst_bytes : src_bytes;
            LG_memcpy(mfd_background.bits, bm->bits, n);
            RefUnlock(REF_IMG_bmBlankMFD);
        }

        gr_init_canvas(&_offscreen_mfd, mfd_canvas_bits, BMT_FLAT8, new_width, new_height);
        gr_init_canvas(&_fullscreen_mfd, mfd_background.bits, BMT_FLAT8, new_width, new_height);
    }
    return (OK);
}

errtype mfd_clear_all() {
    if (full_game_3d) {
        gr_push_canvas(&_offscreen_mfd);
        gr_clear(0);
        gr_pop_canvas();
        gr_push_canvas(&_fullscreen_mfd);
        gr_clear(0);
        gr_pop_canvas();
    }
    return (OK);
}
#endif

// ---------------------------------------------
// mfd_change_fullscreen(uchar on);
//
// set up and clean up for fullscreen mode.

void mfd_change_fullscreen(uchar on) {
    if (on) {
        gr_push_canvas(&_fullscreen_mfd);
        gr_clear(0);
        gr_pop_canvas();
        gr_push_canvas(&_offscreen_mfd);
        gr_clear(0);
        gr_pop_canvas();
    } else {
        // we use the mfd background for the canvas, so
        // put the background bitmap back
        grs_bitmap *bm = lock_bitmap_from_ref(REF_IMG_bmBlankMFD);
        RefUnlock(REF_IMG_bmBlankMFD);
        LG_memcpy(_fullscreen_mfd.bm.bits, bm->bits, bm->w * bm->h);
    }
}

// ---------------------------------------------------------------------------
// keyboard_init_mfd()
//
// Tell the function keys that they're supposed to map to our button panels.
// (Called from init_input() in input.c)

void keyboard_init_mfd() {
    /* KLC leave out F-keys and char codes.

       hotkey_add(KEY_F1, DEMO_CONTEXT,mfd_button_callback_kb,0);
       hotkey_add(KEY_F2, DEMO_CONTEXT,mfd_button_callback_kb,1);
       hotkey_add(KEY_F3, DEMO_CONTEXT,mfd_button_callback_kb,2);
       hotkey_add(KEY_F4, DEMO_CONTEXT,mfd_button_callback_kb,3);
       hotkey_add(KEY_F5, DEMO_CONTEXT,mfd_button_callback_kb,4);
       hotkey_add(KEY_F6, DEMO_CONTEXT,mfd_button_callback_kb,5);
       hotkey_add(KEY_F7, DEMO_CONTEXT,mfd_button_callback_kb,6);
       hotkey_add(KEY_F8, DEMO_CONTEXT,mfd_button_callback_kb,7);
       hotkey_add(KEY_F9, DEMO_CONTEXT,mfd_button_callback_kb,8);
       hotkey_add(KEY_F10,DEMO_CONTEXT,mfd_button_callback_kb,9);
    */
    install_keypad_hotkeys();
}

// --------------
//    FROBBERS
// --------------

// ---------------------------------------------------------------------------
// set_slot_to_func()
//
// Sets a slot to point to a given function struct, and sets status too.

void set_slot_to_func(ubyte snum, ubyte fnum, MFD_Status stat) {

    player_struct.mfd_all_slots[snum] = fnum;

    if ((player_struct.mfd_slot_status[snum] == MFD_FLASH) && (stat == MFD_ACTIVE))
        ;
    else
        player_struct.mfd_slot_status[snum] = stat;

    return;
}

// ---------------------------------------------------------------------------
// mfd_clear_func()
//
// Set a functions last update to current game time, clear its CHANGEBIT
// field if set.

void mfd_clear_func(ubyte func_id) {
    mfd_funcs[func_id].last = player_struct.game_time;

    player_struct.mfd_func_status[func_id] &= ~MFD_CHANGEBIT;
    player_struct.mfd_func_status[func_id] &= ~MFD_CHANGEBIT_FULL;
    return;
}

#define MFD_STEREO_HACK_MODE 6

// ---------------------------------------------------------------------------
// mfd_notify_func()
//
// Let a function know its been changed and needs re-exposure.
// Also check a specified slot to see if that function is there: if
// is not, we might grab it and put it there.  We also set the slot's
// status as demanded.

//#define MFD_STEREO_HACK_MODE  ((i6d_device == I6D_CTM) ? 6 : 7)
void mfd_notify_func(ubyte fnum, ubyte snum, uchar Grab, MFD_Status stat, uchar Full) {
    ubyte i, j;
    int oldf = player_struct.mfd_all_slots[snum];
    byte mfd_but[NUM_MFDS];

    if (fnum == NOTIFY_ANY_FUNC)
        fnum = oldf;

    for (i = 0; i < NUM_MFDS; i++)
        mfd_but[i] = -1;

    if ((oldf != fnum) && (Grab)) {
        player_struct.mfd_all_slots[snum] = fnum;
        Full = TRUE;
    }

    player_struct.mfd_func_status[fnum] |= MFD_CHANGEBIT;
    if (Full) {
        void mfd_default_mru(uchar func);
#ifdef SVGA_SUPPORT
        uchar old_over = gr2ss_override;
        short temp;
        gr2ss_override = OVERRIDE_ALL;
        ss_set_hack_mode(MFD_STEREO_HACK_MODE, &temp);
#endif

        for (i = 0; i < NUM_MFDS; i++) {
            if (oldf != fnum && player_struct.mfd_current_slots[i] == snum) {
                mfd_block_scale_begin();
                mfd_funcs[oldf].expose(&(mfd[i]), 0);
                mfd_block_scale_end();
            }
        }
#ifdef SVGA_SUPPORT
        ss_set_hack_mode(0, &temp);
        gr2ss_override = old_over;
#endif
        player_struct.mfd_func_status[fnum] |= MFD_CHANGEBIT_FULL;
        mfd_default_mru(fnum);
    }

    if (player_struct.mfd_all_slots[snum] == fnum) {
        set_slot_to_func(snum, fnum, stat);

        // Find which buttons we have to redraw because of the change
        for (i = 0; i < NUM_MFDS; i++) {
            for (j = 0; j < MFD_NUM_VIRTUAL_SLOTS; j++) {
                if (player_struct.mfd_virtual_slots[i][j] == snum) {
                    mfd_but[i] = j;
                    if (player_struct.mfd_current_slots[i] == j) {
                        player_struct.mfd_slot_status[snum] = stat;
                        if (stat == MFD_FLASH)
                            player_struct.mfd_slot_status[snum] = MFD_ACTIVE;
                    }
                }
            }
        }

        // Now redraw them
        if (_current_loop <= FULLSCREEN_LOOP && !global_fullmap->cyber)
            for (i = 0; i < NUM_MFDS; i++)
                if (mfd_but[i] != -1)
                    mfd_draw_button(i, mfd_but[i]);
    }

    chg_set_flg(MFD_UPDATE);

}

// ---------------------------------------------------------------------------
// mfd_get_func()
//
// Returns an MFD's slot's function.

ubyte mfd_get_func(ubyte mfd_id, ubyte s) {
    ubyte slot_num;

    slot_num = player_struct.mfd_virtual_slots[mfd_id][s];

    if (player_struct.mfd_slot_status[slot_num] == MFD_EMPTY)
        return player_struct.mfd_empty_funcs[mfd_id];
    else
        return player_struct.mfd_all_slots[slot_num];
}

// ---------------------------------------------------------------------------
// mfd_set_slot()
//
// Sets the mfd to a given slot without caring about turning off what was
// previously there, or any change-related state.  Permits usage from both
// initializer and slot-changer.

void mfd_set_slot(ubyte mfd_id, ubyte newSlot, uchar OnOff) {
    MFD_Func *f;
    ubyte old_index;
    ubyte f_id;
    ubyte new_slot;
    ubyte old_slot;

    old_index = player_struct.mfd_current_slots[mfd_id];
    old_slot = player_struct.mfd_virtual_slots[mfd_id][old_index];
    new_slot = player_struct.mfd_virtual_slots[mfd_id][newSlot];

    if (!OnOff && ((player_struct.mfd_slot_status[old_slot] != MFD_EMPTY) ||
                   (player_struct.mfd_slot_status[new_slot] != MFD_EMPTY))) {
        uchar old_over = gr2ss_override;
        short temp;
        gr2ss_override = OVERRIDE_ALL;
        ss_set_hack_mode(MFD_STEREO_HACK_MODE, &temp);
        f_id = mfd_get_func(mfd_id, newSlot);
        f = &(mfd_funcs[f_id]);
        mfd_block_scale_begin();
        f->expose(&(mfd[mfd_id]), 0);
        mfd_block_scale_end();
        ss_set_hack_mode(0, &temp);
        gr2ss_override = old_over;
    }

    if (player_struct.mfd_slot_status[new_slot] == MFD_FLASH) {
        player_struct.mfd_slot_status[new_slot] = MFD_ACTIVE;

        // We have to tell other panel about this button change!
        if (global_fullmap->cyber) {
            if (mfd_id == MFD_LEFT)
                mfd_draw_button(MFD_RIGHT, newSlot);
            else
                mfd_draw_button(MFD_LEFT, newSlot);
        }
    }

    if (OnOff) {
        player_struct.mfd_current_slots[mfd_id] = newSlot;
        mfd_force_update_single(mfd_id);
        if (full_game_3d) {
            if (!(full_visible & visible_mask(mfd_id))) {
                if (mfd_id == MFD_LEFT)
                    gr_push_canvas(&_offscreen_mfd);
                else
                    gr_push_canvas(&_fullscreen_mfd);
                gr_clear(0);
                gr_pop_canvas();
            }
#ifdef STEREO_SUPPORT
            if (convert_use_mode == 5)
                full_visible = visible_mask(mfd_id);
            else
#endif
            {
                full_visible |= visible_mask(mfd_id);
            }
            // Keep the view at the same z as mfd_apply_fullscreen_geometry()
            // (full_raise_region() only goes to z=2, which would drop it back
            // below the inventory region in the bounds overlap).
            region_move(&mfd[mfd_id].reg2, mfd[mfd_id].reg2.r->ul.x,
                        mfd[mfd_id].reg2.r->ul.y, MFD_REGION_Z);
            chg_set_sta(FULLSCREEN_UPDATE);
        }
    }

    return;
}

// ---------------------------------------------------------------------------
// mfd_change_slot()
//
// Shifts an mfd over to a new slot.

void mfd_change_slot(ubyte mfd_id, ubyte new_slot) {
    ubyte old;

    if (global_fullmap->cyber && (new_slot != MFD_INFO_SLOT || mfd_id != MFD_RIGHT))
        return; // no slots in c-space
    old = player_struct.mfd_current_slots[mfd_id];

    if (new_slot == old && !full_game_3d)
        return;

    // Tell old slot it needs to stop drawing whatever it was in that mfd
    if (new_slot != old)
        mfd_set_slot(mfd_id, old, FALSE);

    // Set to new slot and draw its graphics if neccessary
    mfd_set_slot(mfd_id, new_slot, TRUE);

    // Update the buttons
    if (!global_fullmap->cyber) {
        mfd_draw_button(mfd_id, old);
        mfd_draw_button(mfd_id, new_slot);
    }

    return;
}

// ---------------------------------------------------------------------------
// mfd_grab()
//
// Picks an MFD to grab (i.e. the MFD whose information is
// lowest priority.  Returns the mfd id.

int mfd_grab(void) {
    int i;
    ubyte min = 0;
    int id;
    for (i = 0; i < NUM_MFDS; i++) {
        ubyte slot = player_struct.mfd_current_slots[i];
        ubyte func = mfd_get_func(i, slot);
        ubyte p = mfd_funcs[func].priority;
        if (p > min) {
            min = p;
            id = i;
        }
    }
    return id;
}

// ---------------------------------------------------------------------------
// mfd_grab_func()
//
// Like mfd_grab(), except specifies a func number.  If any mfd is already
// set to that func, returns that mfd id instead of the lowest prority.
// Otherwise, if there is already an mfd on the given slot, returns that
// mfd.  If neither of these conditions holds, returns the mfd with the
// lowest priority.  If other mfds are on the same slot as the one we
// are grabbing for a new func, try to restore to them.

// mfd_choose_func() does the same thing as mfd_grab_func, but does not
// assume we necessarily actually want to grab the mfd it returns, and
// therefore does not restore to other mfds on the same slot.
//
int mfd_choose_func(int my_func, int my_slot) {
    int i;
    ubyte min = 0;
    ubyte slot, func, p;
    int lowid, retval, sameslotid = -1;

    for (i = 0; i < NUM_MFDS; i++) {
        slot = player_struct.mfd_current_slots[i];
        func = mfd_get_func(i, slot);
        p = mfd_funcs[func].priority;

        if (func == my_func)
            return i;
        if (slot == my_slot)
            sameslotid = i;
        if (p > min) {
            min = p;
            lowid = i;
        }
    }
    if (sameslotid != -1)
        retval = sameslotid;
    else
        retval = lowid;

    return retval;
}

int mfd_grab_func(int my_func, int my_slot) {
    ubyte mfd, slot;
    int i;

    mfd = mfd_choose_func(my_func, my_slot);
    slot = player_struct.mfd_current_slots[mfd];

    // if more than one mfd is on the slot we're grabbing, try
    // restoring to the other slots.
    for (i = 0; i < NUM_MFDS; i++) {
        if (i != mfd && player_struct.mfd_current_slots[i] == slot) {
            restore_mfd_slot(i);
            break;
        }
    }
    return mfd;
}

// -----------------------------------------------------------------------
// mfd_yield_func()
//
//   If no mfd has func as its current function, returns FALSE.  Otherwise,
// if *mfd_id is NUM_MFDS, sets mfd_id to the lowest mfd id which has func as
// its function and returns TRUE.  Otherwise, sets mfd_id to the lowest
// such id which is greater than the one provided and returns TRUE, or
// returns FALSE if there is no such greater mfd.  Thus acts as an
// iterator on mfd's with the given func.
//   Why, you may ask?  'Cause it's pretty much exactly as easy as a
// function which just finds out if some mfd has this current function,
// which is what I need, and it's loads more generally useful.  Har har.

uchar mfd_yield_func(int func, int *mfd_id) {
    int id;

    for (id = (*mfd_id != NUM_MFDS) ? (*mfd_id) + 1 : 0; id < NUM_MFDS; id++) {
        if (mfd_get_active_func(id) == func) {
            *mfd_id = id;
            return TRUE;
        }
    }
    return FALSE;
}

// -----------------------------------------------------------
// mfd_zoom_rect(Rect* start, int mfd)
//
// Zooms a rect from the specified starting point to the
// the indicated rect.

void mfd_zoom_rect(LGRect *start, int mfdnum) {
    DEBUG("Zooming mfd %i", mfdnum);
    LGRect r1, r2;
    play_digi_fx(SFX_ZOOM_BOX, 1);
    r1 = *start;
    r2 = mfd[mfdnum].rect;
    zoom_rect(&r1, &r2);
}

// ------------------------
//    CALLBACK FUNCTIONS
// ------------------------

// ------------------------------------------------------------------
// mfd_object_cursor_handler() gets called for events in the MFD
// region with an object on the cursor.

uchar object_button_down = FALSE;

uchar mfd_object_cursor_handler(uiEvent *ev, LGRegion *reg, int which_mfd) {
    uchar retval = FALSE;
    int trip, mid;
    int new_slot = -1;
    ObjID obj = object_on_cursor;
    if (ev->type != UI_EVENT_MOUSE)
        return TRUE;
    mfd_remap_event_pos(&mfd[which_mfd], reg, ev);
    if (ev->subtype & (MOUSE_RDOWN | MOUSE_LDOWN)) {
        object_button_down = TRUE;
        retval = TRUE;
    }
    if ((ev->subtype & (MOUSE_LUP | MOUSE_RUP)) && object_button_down) {
        extern uchar gump_num_objs;
        uchar is_gump = mfd_get_active_func(which_mfd) == MFD_GUMP_FUNC && gump_num_objs != 0;

        object_button_down = FALSE;
        retval = TRUE;
        if (inventory_add_object(object_on_cursor, !is_gump)) {
            if (!is_gump) {
                switch (objs[obj].obclass) {
                case CLASS_GUN:
                    new_slot = MFD_WEAPON_SLOT;
                    break;
                case CLASS_AMMO:
                    trip = current_weapon_trip();
                    if (trip != -1 && gun_takes_ammo(trip, ID2TRIP(obj)) &&
                        player_struct.mfd_current_slots[which_mfd] == MFD_WEAPON_SLOT)
                        ; // do nothing
                    else
                        new_slot = MFD_ITEM_SLOT;
                    break;

                case CLASS_SOFTWARE:
                    if (objs[obj].subclass == SOFTWARE_SUBCLASS_DATA) {
                        new_slot = MFD_INFO_SLOT;
                        break;
                    }
                default:
                    new_slot = MFD_ITEM_SLOT;
                }
                for (mid = 0; mid < NUM_MFDS; mid++) {
                    if (mid != which_mfd && mfd_index(mid) == new_slot)
                        restore_mfd_slot(mid);
                }
                mfd_change_slot(which_mfd, new_slot);
                mfd_force_update_single(which_mfd);
            }
            pop_cursor_object();
        }
    }
    return retval;
}

    // ---------------------------------------------------------------------------
    // mfd_view_callback()
    //
    // The callback for the MFD view windows.  Triggered by mouseclicks inside
    // the regions.

// In fullscreen mode, the MFD view canvas (74x58) is stretch-blitted into
// a larger destination rect (mfd_fg.view_w x mfd_fg.view_h).  Mouse events
// arrive in the destination rect's coordinate space (which is fullview_region's
// 320x200 space), but all the per-function handlers in mfdfunc.c assume
// canvas space -- they do `pos.x -= m->rect.ul.x` and then compare against
// rects defined in canvas coordinates.  To keep those handlers unchanged,
// we pre-rewrite e->pos such that after the handler's own subtraction, the
// result is canvas-space.
//
//   canvas_pos = (e->pos - m->rect.ul) * canvas_size / dest_size
//   rewritten  = m->rect.ul + canvas_pos
//   handler sees rewritten - m->rect.ul == canvas_pos
//
// In non-fullscreen mode the dest size equals the canvas size, so the
// rewrite is a no-op and we skip it entirely.
void mfd_view_event_to_canvas(MFD *m, uiEvent *e) {
    int cx, cy;
    if (!full_game_3d)
        return;
    if (mfd_fg.view_w_r <= 0 || mfd_fg.view_h_r <= 0)
        return;
    cx = (int)((SCONV_X(e->pos.x) - mfd_fg.view_x_r[m->id]) * MFD_VIEW_WID / mfd_fg.view_w_r);
    cy = (int)((SCONV_Y(e->pos.y) - mfd_fg.view_y_r) * MFD_VIEW_HGT / mfd_fg.view_h_r);
    e->pos.x = m->rect.ul.x + cx;
    e->pos.y = m->rect.ul.y + cy;
}

static void mfd_remap_event_pos(MFD *m, LGRegion *r, uiEvent *e) {
    const LGRect *rr;
    short rw, rh, cx, cy;
    if (!full_game_3d)
        return;
    // Map the view's own input rect -- the region the hit-test just used, which
    // SCONV-maps onto the drawn real view -- into the 74x58 canvas space the
    // per-function handlers work in. Using the region guarantees that any click
    // that reaches this handler maps inside 0..MFD_VIEW_*, i.e. the whole
    // reachable area lines up with the drawn view (mfd_fg can disagree with the
    // region, which left only a sub-strip interactive).
    rr = r->r;
    rw = rr->lr.x - rr->ul.x;
    rh = rr->lr.y - rr->ul.y;
    if (rw <= 0 || rh <= 0)
        return;
    if (rw < 4 || rh < 4) {
        // Degenerate/short region (e.g. a stale strip): fall back to the drawn
        // real-pixel view rect so the cursor still lines up with the game.
        if (mfd_fg.view_w_r <= 0 || mfd_fg.view_h_r <= 0)
            return;
        cx = (short)((SCONV_X(e->pos.x) - mfd_fg.view_x_r[m->id]) * MFD_VIEW_WID / mfd_fg.view_w_r);
        cy = (short)((SCONV_Y(e->pos.y) - mfd_fg.view_y_r) * MFD_VIEW_HGT / mfd_fg.view_h_r);
        e->pos.x = m->rect.ul.x + cx;
        e->pos.y = m->rect.ul.y + cy;
        return;
    }
    cx = (short)((e->pos.x - rr->ul.x) * MFD_VIEW_WID / rw);
    cy = (short)((e->pos.y - rr->ul.y) * MFD_VIEW_HGT / rh);
    {
        // One-shot diagnostic per MFD: helps pin down minigame input problems
        // (which region owns the click, and where the canvas point lands).
        static uchar dbg_done[2] = {0, 0};
        int di = (m->id == MFD_RIGHT) ? 1 : 0;
        if (!dbg_done[di]) {
            dbg_done[di] = 1;
            INFO("MFD input dbg id=%d reg=(%d,%d)-(%d,%d) rect=(%d,%d)-(%d,%d) fgview=(%d,%d)+%dx%d "
                 "ev=(%d,%d) canvas=(%d,%d)",
                 m->id, rr->ul.x, rr->ul.y, rr->lr.x, rr->lr.y,
                 m->rect.ul.x, m->rect.ul.y, m->rect.lr.x, m->rect.lr.y,
                 mfd_fg.view_x_r[m->id], mfd_fg.view_y_r, mfd_fg.view_w_r, mfd_fg.view_h_r,
                 e->pos.x, e->pos.y, cx, cy);
        }
    }
    // Encode canvas space as m->rect.ul + canvas: the handlers then do their own
    // "pos -= m->rect.ul" and land exactly in canvas coordinates.
    e->pos.x = m->rect.ul.x + cx;
    e->pos.y = m->rect.ul.y + cy;
}

#define SEARCH_MARGIN 2

uchar mfd_scan_opacity(int mfd_id, LGPoint epos) {
    uchar retval = FALSE;
    LGPoint pos = epos;
    short x, y;
    grs_canvas *cv = ((int)mfd_id == MFD_RIGHT) ? &_fullscreen_mfd : &_offscreen_mfd;

    pos.x -= mfd[mfd_id].reg.abs_x;
    pos.y -= mfd[mfd_id].reg.abs_y;
    // The view canvas is uniform-scaled, so map the 74x58 view space to canvas
    // pixels with the block scale (guarded to the canvas).
    pos.x = (short)(pos.x * mfd_view_w / MFD_VIEW_WID);
    pos.y = (short)(pos.y * mfd_view_h / MFD_VIEW_HGT);
    gr_push_canvas(cv);
    for (x = pos.x - SEARCH_MARGIN; x <= pos.x + SEARCH_MARGIN; x++)
        for (y = pos.y - SEARCH_MARGIN; y <= pos.y + SEARCH_MARGIN; y++)
            if (gr_get_pixel(x, y) != 0)
                retval = TRUE;
    gr_pop_canvas();
    return retval;
}

uchar mfd_view_callback_full(uiEvent *e, LGRegion *r, intptr_t udata) {
    uchar retval = FALSE;
    uchar mask;
    if (udata == MFD_RIGHT)
        mask = FULL_R_MFD_MASK;
    else
        mask = FULL_L_MFD_MASK;
    if (full_visible & mask) {
        // Save the 320x200 position: mfd_view_callback() rewrites e->pos into
        // canvas space for the function handlers, but mfd_scan_opacity() maps
        // the region-relative logical position itself.
        LGPoint saved = e->pos;
        retval = mfd_view_callback(e, r, udata);
        if (!retval) {
            retval = mfd_scan_opacity(udata, saved);
        }
    }
    return retval;
}

uchar mfd_view_callback(uiEvent *e, LGRegion *r, intptr_t udata) {
    int i;
    int which_mfd;
    MFD *m;
    ubyte func_id;
    MFD_Func *f;

    LGRegion dummy; // dummy
    dummy = *r;     // dummy

    which_mfd = (int)udata;

    // We should pass on info to the appropriate slot's current handler
    if (which_mfd == MFD_LEFT)
        m = &mfdL;
    else
        m = &mfdR;

    mfd_remap_event_pos(m, r, e);

    if (input_cursor_mode == INPUT_OBJECT_CURSOR)
        return mfd_object_cursor_handler(e, r, which_mfd);
    else
        object_button_down = FALSE;
    func_id = mfd_get_active_func(which_mfd);
    f = &(mfd_funcs[func_id]);
    if (f->simp && f->simp(m, e))
        return TRUE;
    for (i = 0; i < f->handler_count; i++) {
        LGPoint pos = e->pos;
#ifdef STEREO_SUPPORT
        if (convert_use_mode == 5) {
            pos.y -= m->rect.ul.y;
            switch (i6d_device) {
            case I6D_CTM:
                if (which_mfd == 0)
                    pos.x -= m->rect.ul.x;
                else
                    pos.x -= (m->rect.ul.x << 1);
                break;
            case I6D_VFX1:
                Warning(("original pos.x = %d, m->rect.ul.x = %d!\n", pos.x, m->rect.ul.x));
                pos.x -= (m->rect.ul.x);
                break;
            }
        } else {
#endif
            pos.x -= m->rect.ul.x;
            pos.y -= m->rect.ul.y;
#ifdef STEREO_SUPPORT
        }
#endif
        if (RECT_TEST_PT(&f->handlers[i].r, pos))
            if (f->handlers[i].proc(m, e, &f->handlers[i]))
                return TRUE;
    }

    return FALSE;
}

// ---------------------------------------------------------------------------
// mfd_button_callback()
//
// The callback for the MFD button panels.  Triggered by mouseclicks inside
// the button panels.
int last_mfd_cnum[NUM_MFDS] = {-1, -1};
uchar mfd_button_callback(uiEvent *e, LGRegion *r, intptr_t udata) {
    int cnum, which_panel, which_button;
    div_t result;

#ifndef NO_DUMMIES
    LGRegion dummy;
    dummy = *r;
#endif

    if (global_fullmap->cyber) {
        uiSetRegionDefaultCursor(r, NULL);
        return FALSE;
    } else {
        which_panel = (int)udata;

        // Divide mouseclick height to discover which button we meant
//        result = div((e->pos.y - MFD_BTTN_Y), MFD_BTTN_SZ + MFD_BTTN_BLNK);
//        which_button = result.quot;

        // Divide mouseclick height to discover which button we meant.
        // In fullscreen the buttons are DRAWN at exact real pixels (see
        // mfd_draw_button / mfd_compute_fullscreen_geometry), so the click must
        // be tested in real pixels too: convert the event's logical y through
        // SCONV_Y, then hit-test against the real strip geometry
        // (bttn_y_r / btn_sz_r / btn_blnk_r). Testing against the logical rect
        // directly made clicks and drawn buttons diverge at extreme resolutions
        // (5120x1000), where the logical strip collapses to ~1 unit.
        short rel_y, stride, btn_sz;
        if (full_game_3d) {
            int real_y = SCONV_Y(e->pos.y);
            rel_y  = (short)(real_y - mfd_fg.bttn_y_r);
            stride = (short)mfd_fg.btn_sz_r + (short)mfd_fg.btn_blnk_r;
            btn_sz = (short)mfd_fg.btn_sz_r;
        } else {
            rel_y  = e->pos.y - MFD_BTTN_Y;
            stride = MFD_BTTN_SZ + MFD_BTTN_BLNK;
            btn_sz = MFD_BTTN_SZ;
        }
        if (rel_y < 0) return FALSE;
        if (stride <= 0) return FALSE; // guard against a degenerate button strip
        result = div(rel_y, stride);
        which_button = result.quot;

        // The strip rect and the button stride can disagree by a pixel or two
        // after rounding (see mfd_compute_fullscreen_geometry), so a click in
        // the trailing slack can map past the last button. cursor_strings[]
        // has exactly MFD_NUM_VIRTUAL_SLOTS entries, so indexing it out of
        // range reads a garbage pointer and crashes deeper in the cursor code.
        if ((which_button < 0) || (which_button >= MFD_NUM_VIRTUAL_SLOTS))
            return FALSE;

        cnum = which_button;

        if (player_struct.mfd_slot_status[which_button] == MFD_UNAVAIL || !popup_cursors) {
            if ((cnum != last_mfd_cnum[which_panel])) {
                last_mfd_cnum[which_panel] = cnum;
                uiSetRegionDefaultCursor(r, &globcursor);
            }
        }
        if (player_struct.mfd_slot_status[which_button] != MFD_UNAVAIL) {
            if ((cnum != last_mfd_cnum[which_panel]) && popup_cursors) {
                LGPoint offset = {0, 0};
                last_mfd_cnum[which_panel] = cnum;
                free(mfd_bttn_bitmaps[which_panel].bits);
                make_popup_cursor(&mfd_bttn_cursors[which_panel], &mfd_bttn_bitmaps[which_panel], cursor_strings[cnum],
                                  which_panel, TRUE, offset);
                uiSetRegionDefaultCursor(r, &mfd_bttn_cursors[which_panel]);
            }

            if (!(e->mouse_data.action & (MOUSE_LDOWN | UI_MOUSE_LDOUBLE)))
                return TRUE; // ignore all but left clickdowns

            // If things are ok, select button
//            if ((result.rem < MFD_BTTN_SZ) && (which_button < MFD_NUM_VIRTUAL_SLOTS))

            if ((result.rem < btn_sz) && (which_button < MFD_NUM_VIRTUAL_SLOTS))
                mfd_select_button(which_panel, which_button);
        }
    }

    return TRUE;
}

// ---------------------------------------------------------------------------
// mfd_button_callback_kb()
//
// The callback for the MFD button panels, as triggered by function keys

uchar mfd_button_callback_kb(ushort keycode, uint32_t context, intptr_t data) {
    int which_panel, which_button;
    int fkeynum;

    if (!global_fullmap->cyber) {

        fkeynum = (int)keycode - 128;

        if (fkeynum >= MFD_NUM_VIRTUAL_SLOTS) {
            which_panel = MFD_RIGHT;
        }
        else {
            which_panel = MFD_LEFT;
        }

        which_button = ((int)fkeynum) % MFD_NUM_VIRTUAL_SLOTS;

        mfd_select_button(which_panel, which_button);
    }

    return TRUE;
}

// ---------------------------------------------------------------------------
// mfd_select_button()
//
// A more specific version of mfd_button_callback(), where we've figured
// out which button exactly has been hit, whether because we came here
// straight from a function key or through the mouse callback parser.
// The passed argument is the equivalent of the function key number, even
// if we got it from the mouse handler.

void mfd_select_button(int which_panel, int which_button) {
    // Play sound effect
    ubyte old = player_struct.mfd_current_slots[which_panel];
    int hnd = play_digi_fx(SFX_MFD_BUTTON, 1);

    if (hnd >= 0) {
        snd_digi_parms *ssp;
        ssp = snd_sample_parms(hnd);

        // Woo hoo, hardcode city
        if (which_panel == MFD_LEFT)
            ssp->pan = 30;
        else
            ssp->pan = 97;
    }

    if (full_game_3d && which_button == old && (full_visible & visible_mask(which_panel))) {
        full_visible &= ~visible_mask(which_panel);
    } else
        mfd_change_slot((ubyte)which_panel, (ubyte)which_button);
}

// ---------------------------------------------------------------------------
// mfd_update()
//
// This is what gets called from the main loop each frame.

void mfd_update() {
    static uchar LastFlash = FALSE;
    ubyte steps_cache[NUM_MFDS] = {0, 0};

    int i, j;
    ubyte slots[NUM_MFDS];
    ubyte status_cache[NUM_MFDS];

    Flash = (bool)((player_struct.game_time / MFD_BTTN_FLASH_TIME) % 2);

    if (!global_fullmap->cyber)
        for (i = 0; i < NUM_MFDS; i++) {
            for (j = 0; j < MFD_NUM_VIRTUAL_SLOTS; j++) {
                slots[i] = player_struct.mfd_virtual_slots[i][j];
                if (player_struct.mfd_slot_status[slots[i]] == MFD_FLASH) {
                    chg_set_flg(MFD_UPDATE);
                    if (LastFlash != Flash)
                        mfd_draw_button(i, j);
                }
            }
        }
    if (LastFlash != Flash)
        LastFlash = Flash;

        // Is it time to update appropriate mfd's?
        // Check only current slots, and look at flag to see
        // if they need constant update

#ifndef BAD_BITS_BUG_FIXED
    _fullscreen_mfd.bm.bits = mfd_background.bits;
    _offscreen_mfd.bm.bits = mfd_canvas_bits;
#endif // BAD_BITS_BUG_FIXED

    // This code totally depends on our item func implementation.
    i = NUM_MFDS;
    if (mfd_yield_func(MFD_ITEM_FUNC, &i)) {
        update_item_mfd();
    }

    // Build the status cache.
    for (i = 0; i < NUM_MFDS; i++) {
        ubyte f_id = mfd_get_active_func(i);
        MFD_Func *f = &(mfd_funcs[f_id]);
        status_cache[i] = (player_struct.mfd_func_status[f_id]);
        if (f->flags & MFD_INCREMENTAL) {
            long deltat = (player_struct.game_time - f->last) >> 4;
            ubyte increment = (player_struct.mfd_func_status[f_id] >> 4);
            ubyte num_steps = (increment > 0) ? lg_max(0, deltat / increment) : 0;
            steps_cache[i] = num_steps;
            chg_set_flg(MFD_UPDATE);
        }
    }

    // Now update the stati that need it.
    for (i = 0; i < NUM_MFDS; i++) {
        if ((status_cache[i] & (MFD_CHANGEBIT | MFD_CHANGEBIT_FULL)) || steps_cache[i] > 0) {
            ubyte f_id = mfd_get_active_func(i);
            mfd_clear_func(f_id);
            mfd_update_current_slot(i, status_cache[i], steps_cache[i]);
        }
    }
    return;
}

// ---------------------------------------------------------------------------
// mfd_update_current_slot()
//
// See if we need to update anything in the current slot being
// viewed in an MFD.  Returns TRUE if it updated a function.

uchar mfd_update_current_slot(ubyte mfd_id, ubyte status, ubyte num_steps) {
    MFD_Func *f;
    ubyte f_id;
    ubyte control;
    MFD *m;

    f_id = mfd_get_active_func(mfd_id);
    f = &(mfd_funcs[f_id]);
    m = &(mfd[mfd_id]);
    if (player_struct.panel_ref == OBJ_NULL && mfd_distance_remove(f_id)) {
        check_panel_ref(TRUE);
    }

    // If the change bit is set, or if the function is incremental
    // and enough time has gone by, then we need to expose

    {
#ifdef SVGA_SUPPORT
        uchar old_over = gr2ss_override;
        short temp;
        gr2ss_override = OVERRIDE_ALL;
        ss_set_hack_mode(MFD_STEREO_HACK_MODE, &temp);
#endif
        control = (num_steps << 4) | MFD_EXPOSE;
        if (full_game_3d || (status & MFD_CHANGEBIT_FULL))
            control |= MFD_EXPOSE_FULL;

        // Okay, if we are in full screen mode, secretly switch the fine canvas
        // usually served in this restaurant with our own Folger's brand canvas

        pmfd_canvas = (full_game_3d && mfd_id == MFD_RIGHT) ? &_fullscreen_mfd : &_offscreen_mfd;
        if (full_game_3d && (control & MFD_EXPOSE_FULL)) {
            gr_push_canvas(pmfd_canvas);
            gr_clear(0);
            gr_pop_canvas();
        }

        mfd_block_scale_begin();
        f->expose(m, control); // pass # steps + flags to
        mfd_block_scale_end();
#ifdef SVGA_SUPPORT
        ss_set_hack_mode(0, &temp);
        gr2ss_override = old_over;
#endif

        return TRUE;
    }
}

// ---------------------------------------------------------------------------
// mfd_force_update()
//
// Forces a redraw of both the button panels and mfd slots

void mfd_force_update() {
    ubyte i;
    for (i = 0; i < NUM_MFDS; i++) {
        mfd_force_update_single(i);
    }
}

// ---------------------------------------------------------------------------
// mfd_force_update()
//
// Forces a redraw of one of the button panels and mfd slots

void mfd_force_update_single(int which_mfd) {
    ubyte f_id, s_id;
    MFD_Status stat;

    if (_current_loop <= FULLSCREEN_LOOP)
        mfd_draw_all_buttons(which_mfd);

    f_id = mfd_get_active_func(which_mfd);
    s_id = player_struct.mfd_virtual_slots[which_mfd][mfd_index(which_mfd)];
    stat = player_struct.mfd_slot_status[s_id];

    mfd_notify_func(f_id, s_id, FALSE, stat, TRUE);

    return;
}

//--------------------------------------------------------
// fullscreen_refresh_mfd()
//
// re-blits a single mfd.

void fullscreen_refresh_mfd(ubyte mfd_id) {
    ushort a, b, c, d;
    LGRect r;
    MFD *m = &mfd[mfd_id];
    uchar visible = (full_visible & visible_mask(mfd_id)) != 0;
#ifdef SVGA_SUPPORT
    uchar old_over = gr2ss_override;
#endif
    if (visible) {
        pmfd_canvas = (mfd_id == MFD_RIGHT) ? &_fullscreen_mfd : &_offscreen_mfd;

        // INFO("fullscreen_refresh_mfd id=%d m.rect=(%d,%d)-(%d,%d) fg3d=%d cum=%d visible=%d",
        //      mfd_id, m->rect.ul.x, m->rect.ul.y, m->rect.lr.x, m->rect.lr.y,
        //      full_game_3d, convert_use_mode, visible);

        // Real-pixel view rect (no logical round-trip; see mfd_fg.view_x_r).
        r.ul = MakePoint(mfd_fg.view_x_r[mfd_id], mfd_fg.view_y_r);
        r.lr = MakePoint(mfd_fg.view_x_r[mfd_id] + mfd_fg.view_w_r,
                         mfd_fg.view_y_r + mfd_fg.view_h_r);

        STORE_CLIP(a, b, c, d);
#ifdef SVGA_SUPPORT
        gr2ss_override = OVERRIDE_ALL;
#endif
#ifdef STEREO_SUPPORT
        if (convert_use_mode == 5) {
            pmfd_canvas->bm.flags |= BMF_TRANS;
            if (mfd_id == 0) {
                ss_safe_set_cliprect(r.ul.x, 0, r.lr.x << 1, r.lr.y);
                if (i6d_device == I6D_CTM)
                    ss_noscale_bitmap(&(pmfd_canvas->bm), m->rect.ul.x, -5);
                else
                    ss_noscale_bitmap(&(pmfd_canvas->bm), m->rect.ul.x, m->rect.ul.y);
            } else {
                ss_safe_set_cliprect(r.ul.x >> 1, 0, r.lr.x, r.lr.y);
                if (i6d_device == I6D_CTM)
                    ss_noscale_bitmap(&(pmfd_canvas->bm), m->rect.ul.x >> 1, -5);
                else
                    ss_noscale_bitmap(&(pmfd_canvas->bm), m->rect.ul.x >> 1, m->rect.ul.y);
            }
            pmfd_canvas->bm.flags &= ~BMF_TRANS;
        } else {
#endif
            gr_safe_set_cliprect(r.ul.x, r.ul.y, r.lr.x, r.lr.y);
            pmfd_canvas->bm.flags |= BMF_TRANS;
            // Uniform-scaled canvas == the view's real size; blit 1:1 at the
            // exact real rect from the geometry.
            gr_bitmap(&(pmfd_canvas->bm), mfd_fg.view_x_r[mfd_id], mfd_fg.view_y_r);
            pmfd_canvas->bm.flags &= ~BMF_TRANS;
#ifdef STEREO_SUPPORT
        }
#endif
#ifdef SVGA_SUPPORT
        gr2ss_override = old_over;
#endif
        RESTORE_CLIP(a, b, c, d);
    }
    region_set_invisible(&m->reg2, !visible);
}

// ---------------------------
//    MFD INTERNAL GRAPHICS
// ---------------------------

// ---------------------------------------------------------------------------
// mfd_draw_button()
//
// Draws a button in a given color code depending on its status.

uchar cyber_button_back_door = FALSE;

void mfd_draw_button(ubyte mfd_id, ubyte b) {
    MFD *m;
    LGRect r;
    ubyte slot;
#ifdef SVGA_SUPPORT
    uchar old_over = gr2ss_override;
    gr2ss_override = OVERRIDE_ALL;
#endif

    if (global_fullmap->cyber && full_game_3d)
        return;

    m = &(mfd[mfd_id]);

//    r.ul.x = m->bttn.rect.ul.x + 1;
//    r.ul.y = m->bttn.rect.ul.y + 1;
//    r.ul.y += (b * (MFD_BTTN_SZ + MFD_BTTN_BLNK));
//
//    r.lr.x = r.ul.x + MFD_BTTN_WID - 1;
//    r.lr.y = r.ul.y + MFD_BTTN_SZ;

    if (full_game_3d) {
        // Real-pixel button rect (mfd_fg.*_r); see mfd_compute_fullscreen_geometry.
        // NOTE: when the buttons are drawn as a bitmap the indicator must fit
        // INSIDE that bitmap, so any size change must be driven by the bitmap /
        // geometry -- not by a blind inset (a previous inset misaligned it).
        r.ul.x = mfd_fg.bttn_x_r[mfd_id] + 1;
        r.ul.y = mfd_fg.bttn_y_r + 1 + b * (mfd_fg.btn_sz_r + mfd_fg.btn_blnk_r);
        r.lr.x = r.ul.x + mfd_fg.btn_wid_r - 2;
        r.lr.y = r.ul.y + mfd_fg.btn_sz_r;
    } else {
        r.ul.x = m->bttn.rect.ul.x + 1;
        r.ul.y = m->bttn.rect.ul.y + 1;
        r.ul.y += (b * (MFD_BTTN_SZ + MFD_BTTN_BLNK));
        r.lr.x = r.ul.x + MFD_BTTN_WID - 1;
        r.lr.y = r.ul.y + MFD_BTTN_SZ;
    }

    slot = player_struct.mfd_virtual_slots[mfd_id][b];

    if (player_struct.mfd_slot_status[slot] == MFD_EMPTY)
        gr_set_fcolor(MFD_BTTN_EMPTY);
    else if (player_struct.mfd_slot_status[slot] == MFD_ACTIVE)
        gr_set_fcolor(MFD_BTTN_ACTIVE);
    else if (player_struct.mfd_slot_status[slot] == MFD_UNAVAIL)
        gr_set_fcolor(MFD_BTTN_UNAVAIL);
    else if (player_struct.mfd_slot_status[slot] == MFD_FLASH) {
        if (Flash)
            gr_set_fcolor((long)MFD_BTTN_FLASH); // Is blink on or off?
        else
            gr_set_fcolor((long)MFD_BTTN_EMPTY); // Draw appropriately
    }

    if (mfd_index(m->id) == b)
        gr_set_fcolor((long)MFD_BTTN_SELECT); // current

    uiHideMouse(&r);
    if (full_game_3d)
        gr_rect(r.ul.x, r.ul.y, r.lr.x, r.lr.y);   // real px, no SCONV
    else
        ss_rect(r.ul.x, r.ul.y, r.lr.x - 2, r.lr.y - 2);
/*{
        short		bx = (mfd_id == 0) ? 3 : 629;
        short		by = 333 + (b*26);
        gr_rect(bx, by, bx+6, by+17);
}*/
#ifdef SVGA_SUPPORT
    gr2ss_override = old_over;
#endif
    uiShowMouse(&r);

    return;
}

    // ---------------------------------------------------------------------------
    // mfd_draw_button_panel()
    //
    // Draws an MFD's panel of buttons, and their associated background art as well

#define MFD_PANEL_Y 326
#define MFD_LEFT_PANEL_X 1
// This panel's background art was authored assuming a real 640px-wide
// screen (627 = 13px inset from the 640 right edge). These x values are
// raw screen pixels here (not run through the SCONV_X logical->real
// scaler), so on a wider-than-640 real resolution (e.g. a custom
// widescreen mode) a literal 627 would leave the panel stranded well
// short of the actual right edge. Anchor it to the real screen width
// instead, keeping the same 13px inset, so it hugs the right edge at
// any resolution.
#define MFD_RIGHT_PANEL_INSET (640 - 627)

//void mfd_draw_button_panel(ubyte mfd_id) {
//    int x[2] = {MFD_LEFT_PANEL_X, grd_cap->w - MFD_RIGHT_PANEL_INSET};
//
//    draw_res_bm(REF_IMG_bmMFDButtonBackground, x[mfd_id], MFD_PANEL_Y);
//    mfd_draw_all_buttons(mfd_id);
//    return;
//}

void mfd_draw_button_panel(ubyte mfd_id) {
    if (full_game_3d) {
        // Draw the button-strip background into the exact real-pixel strip rect
        // (mfd_fg.bttn_*_r). The logical rect rounds a 6px strip to 0 at 5120
        // wide, collapsing the strip.
        FrameDesc *f = RefLock(REF_IMG_bmMFDButtonBackground);
        if (f) {
            gr_scale_bitmap(&f->bm,
                            mfd_fg.bttn_x_r[mfd_id], mfd_fg.bttn_y_r,
                            mfd_fg.bttn_w_r, mfd_fg.bttn_h_r);
            RefUnlock(REF_IMG_bmMFDButtonBackground);
        }
    } else {
        // Non-fullscreen: draw the panel background into the SAME logical strip
        // rect the buttons use, so SCONV maps both together. (Previously it used
        // raw 640x480 screen pixels -- MFD_PANEL_Y=326, grd_cap->w-13 -- which
        // draw_res_bm feeds through SCONV again, landing the panel somewhere else
        // entirely and clipping its art.)
        int lx = (mfd_id == MFD_LEFT) ? MFD_BTTN_LFTX : MFD_BTTN_RGTX;
        FrameDesc *f = RefLock(REF_IMG_bmMFDButtonBackground);
        if (f) {
            ss_scale_bitmap(&f->bm, lx, MFD_BTTN_Y, MFD_BTTN_WID, MFD_BTTN_HGT);
            RefUnlock(REF_IMG_bmMFDButtonBackground);
        }
    }
    mfd_draw_all_buttons(mfd_id);
    return;
}


// ---------------------------------------------------------------------------
// mfd_draw_all_buttons()
//
// Draws an MFD's panel of buttons.

void mfd_draw_all_buttons(ubyte mfd_id) {
    ubyte i;

    for (i = 0; i < MFD_NUM_VIRTUAL_SLOTS; i++)
        mfd_draw_button(mfd_id, i);

    return;
}

// ---------------------------------------------------------------------------
// mfd_draw_string()
//
// Draws a string to the mfd canvas, at a relative x, y location.  It is
// the calling expose functions responsibility to recopy from the canvas
// to the screen.  Returns a point describing the pixel dimensions of the string

uchar mfd_string_wrap = TRUE;
ubyte mfd_string_shadow = MFD_SHADOW_FULLSCREEN;

LGPoint mfd_full_draw_string(char *s, short x, short y, long c, int font, uchar DrawString, uchar transp) {
    LGPoint siz;
    short w, h;
    ushort sc1, sc2, sc3, sc4;
    short border = 0;
    grs_font *thefont = ResLock(font);

    x = lg_min(lg_max(x, 0), MFD_VIEW_WID - 1);
    y = lg_min(lg_max(y, 0), MFD_VIEW_HGT - 1);
    STORE_CLIP(sc1, sc2, sc3, sc4);
    if ((full_game_3d && mfd_string_shadow == MFD_SHADOW_FULLSCREEN) ||
        mfd_string_shadow == MFD_SHADOW_ALWAYS)
        border = 1;

    gr_set_font(thefont);
    if (mfd_string_wrap)
      gr_string_wrap(s, MFD_VIEW_WID - x - 1);
    gr_string_size(s, &w, &h);
    w = lg_min(w, MFD_VIEW_WID - x - border);
    h = lg_min(h, MFD_VIEW_HGT - y - border);
    siz.x = w;
    siz.y = h;
    if (w <= 0 || h <= 0)
        goto out;
    ss_safe_set_cliprect(lg_max(x - border, 0), lg_max(y - border, 0), x + w + border, y + h + border);
    if (!full_game_3d && !transp)
        ss_bitmap(&mfd_background, 0, 0);
    // gr_bitmap(&mfd_background,0,0);
    if (DrawString) {
        gr_set_fcolor(c);
        draw_shadowed_string(s, x, y, border > 0);
    }
    mfd_add_rect(x - border, y - border, x + w + border, y + h + border);
out:
    if (mfd_string_wrap)
      gr_font_string_unwrap(s);
    ResUnlock(font);
    RESTORE_CLIP(sc1, sc2, sc3, sc4);

    return siz;
}

LGPoint mfd_draw_font_string(char *s, short x, short y, long c, int font, uchar DrawString) {
    // Hey, this used to always specify non-transparent strings,but that just plain
    // seemed wrong, so I switched it.... -- Xemu
    return mfd_full_draw_string(s, x, y, c, font, DrawString, TRUE);
}

LGPoint mfd_draw_string(char *s, short x, short y, long c, uchar DrawString) {
    return mfd_draw_font_string(s, x, y, c, RES_tinyTechFont, DrawString);
}

// ----------------------------------------------------------------------
// mfd_draw_bitmap() draws a bitmap and adds its rect to the
// update list.

void mfd_draw_bitmap(grs_bitmap *bmp, short x, short y) {
    ss_bitmap(bmp, x, y);
    mfd_add_rect(x, y, x + bmp->w, y + bmp->h);
}

// --------------------------------------------------------------------------
// mfd_partial_clear()
//
// Clears a portion of an mfd canvas

void mfd_partial_clear(LGRect *r) {
    if (!full_game_3d) {
        ss_safe_set_cliprect(r->ul.x, r->ul.y, r->lr.x, r->lr.y);
        mfd_add_rect(r->ul.x, r->ul.y, r->lr.x, r->lr.y);
        ss_bitmap(&mfd_background, 0, 0);
        // gr_bitmap(&mfd_background, 0, 0);
    }

    return;
}

    // -------------------------------------------------------------------
    // UPDATE RECT STUFF
    //
    // Here we are collecting a list refresh rectangles which will be
    // updated from the off-screen mfd canvas

#define NUM_MFD_RECTS 16
LGRect mfd_update_list[NUM_MFD_RECTS];
int mfd_num_updates = 0;

// -------------------------------------------------------------------
//
// mfd_clear_rects(), clears the update list to empty

void mfd_clear_rects(void) { mfd_num_updates = 0; }

// -------------------------------------------------------------------
//
// mfd_add_rect() adds a rect to the rect list.

errtype mfd_add_rect(short x, short y, short x1, short y1) {
    int i;
    LGRect r;
    short tmp;
    // check for invalid rect
    if (x > x1) {
        tmp = x;
        x = x1;
        x1 = tmp;
    }
    if (y > y1) {
        tmp = y;
        y = y1;
        y1 = tmp;
    }
    r.ul.x = x;
    r.ul.y = y;
    r.lr.x = x1;
    r.lr.y = y1;
    for (i = 0; i < mfd_num_updates; i++)
        if (RECT_TEST_SECT(&mfd_update_list[i], &r)) {
            // If we intersect with some existing rect, union it in to r and
            // Delete it from the list
            RECT_UNION(&mfd_update_list[i], &r, &r);
            if (i != mfd_num_updates - 1)
                mfd_update_list[i] = mfd_update_list[mfd_num_updates - 1];
            mfd_num_updates--;
            i = 0; // We might now intersect a previous one.
        }
    if (mfd_num_updates >= NUM_MFD_RECTS)
        return ERR_DOVERFLOW;
    mfd_update_list[mfd_num_updates++] = r;
    return OK;
}

// -------------------------------------------------------------------
//
// mfd_update_rects() Updates all rects in the update list, then clears
// the update list.

void mfd_update_rects(MFD *m) {
    int i;
    for (i = 0; i < mfd_num_updates; i++) {
        LGRect *r = &mfd_update_list[i];
        // Filter out degenerate rects!
        if ((r->lr.x <= r->ul.x) || (r->lr.y <= r->ul.y))
            continue;
        mfd_update_display(m, r->ul.x, r->ul.y, r->lr.x, r->lr.y);
    }
    mfd_num_updates = 0;
}

// --------------------------------------------------------------------------
// mfd_update_display()
//
// Updates a portion of the view window from canvas.

void mfd_update_display(MFD *m, short x0, short y0, short x1, short y1) {

    // INFO("mfd_update_display id=%d m.rect=(%d,%d)-(%d,%d) args=(%d,%d)-(%d,%d) fg3d=%d cum=%d",
    //      m->id, m->rect.ul.x, m->rect.ul.y, m->rect.lr.x, m->rect.lr.y,
    //      x0, y0, x1, y1, full_game_3d, convert_use_mode);

    ushort a, b, c, d;
    uchar old_over = gr2ss_override;

    if (!full_game_3d) {
        LGRect r;

        if ((x0 > x1) || (y0 > y1))
            return;

        // Force the view rect to the full logical view size. mfd[].rect ends up
        // normalised to a smaller box at runtime (observed 55x58 instead of
        // 74x58, constant across modes), which left the view content not filling
        // its box on 16:9. Re-asserting here (on the draw path) guarantees it.
        m->rect.ul.x = (m->id == MFD_LEFT) ? MFD_VIEW_LFTX : MFD_VIEW_RGTX;
        m->rect.ul.y = MFD_VIEW_Y;
        m->rect.lr.x = m->rect.ul.x + MFD_VIEW_WID;
        m->rect.lr.y = MFD_VIEW_Y + MFD_VIEW_HGT;

        r.ul.x = x0;
        r.ul.y = y0;
        r.lr.x = x1;
        r.lr.y = y1;

        RECT_OFFSETTED_RECT(&r, m->rect.ul, &r);
        if (!RECT_TEST_SECT(&r, &m->rect))
            return;
        RectSect(&r, &m->rect, &r);

        gr_push_canvas(grd_screen_canvas);
        uiHideMouse(&r);
        STORE_CLIP(a, b, c, d);
        gr2ss_override = OVERRIDE_ALL;
//        ss_safe_set_cliprect(r.ul.x, r.ul.y, r.lr.x, r.lr.y);
//        ss_noscale_bitmap(&(pmfd_canvas->bm), m->rect.ul.x, m->rect.ul.y);

        ss_safe_set_cliprect(r.ul.x, r.ul.y, r.lr.x, r.lr.y);
        if (full_game_3d) {
            ss_scale_bitmap(&(pmfd_canvas->bm),
                            m->rect.ul.x, m->rect.ul.y,
                            m->rect.lr.x - m->rect.ul.x,
                            m->rect.lr.y - m->rect.ul.y);
        } else {
            // Scale the view canvas into the logical view rect (SCONV maps both)
            // instead of blitting 1:1 -- a 1:1 blit left the window at the
            // canvas's own size, not filling its box, so the view looked wrong
            // and its graphics got clipped.
            ss_scale_bitmap(&(pmfd_canvas->bm),
                            m->rect.ul.x, m->rect.ul.y,
                            m->rect.lr.x - m->rect.ul.x,
                            m->rect.lr.y - m->rect.ul.y);
        }

        gr2ss_override = old_over;
        uiShowMouse(&r);
        RESTORE_CLIP(a, b, c, d);
        gr_pop_canvas();
    }

    return;
}

// ***********************************************
// **** SAVE/RESTORE and DEFAULT MFD MANAGER *****
// ***********************************************

// for saving and restoring mfd settings around "panel" mfd's, we
// use our secret 6th slot.

typedef uchar (*mfd_def_qual)(void);

typedef struct {
    uchar func;
    uchar slot;
    mfd_def_qual qual;
} mfd_default;

mfd_default default_mfds[] = {{MFD_MAP_FUNC, MFD_MAP_SLOT, &mfd_automap_qual},
                              {MFD_WEAPON_FUNC, MFD_WEAPON_SLOT, &mfd_weapon_qual},
                              {MFD_TARGET_FUNC, MFD_TARGET_SLOT, &mfd_target_qual}};

#define NUM_MFD_DEFAULTS (sizeof(default_mfds) / sizeof(mfd_default))

void mfd_default_mru(uchar func) {
    int i, pos = -1;
    mfd_default tmp;

    for (i = 0; i < NUM_MFD_DEFAULTS; i++) {
        if (default_mfds[i].func == func) {
            pos = i;
            tmp = default_mfds[i];
            break;
        }
    }
    if (pos < 0)
        return; // func not in default list

    for (i = pos; i > 0; i--)
        default_mfds[i] = default_mfds[i - 1];
    default_mfds[0] = tmp;
}

void save_mfd_slot(int mfd_id) {
    uchar func, mask;
    int slot;

    if (global_fullmap->cyber)
        return;
    if (player_struct.mfd_save_slot[mfd_id] < 0) {
        slot = player_struct.mfd_current_slots[mfd_id];

        func = mfd_get_func(mfd_id, slot);
        if (!(mfd_funcs[func].flags & MFD_NOSAVEREST)) {
            player_struct.mfd_save_slot[mfd_id] = slot;
            set_slot_to_func(MFD_SPECIAL_SLOT + mfd_id, func, MFD_ACTIVE);
            if (full_game_3d) {
                mask = visible_mask(mfd_id);
                player_struct.mfd_save_vis &= ~mask;
                player_struct.mfd_save_vis |= (mask & full_visible);
            }
        }
    }
}

// sets mfd to given slot and func, unless the func passed in is MFD_EMPTY_FUNC
// or some MFD already has that func.  In that case, set to some other slot
// from a list of hopefully useful defaults.
// note that if you pass in MFD_EMPTY_FUNC, a new setting is always selected,
// so the slot argument is ignored.

void set_mfd_from_defaults(int mfd_id, uchar func, uchar slot) {
    uchar def, mid;
    uchar check;

    def = 0;
    do {
        check = FALSE;
        for (mid = 0; mid < NUM_MFDS; mid++) {
            if (func == MFD_EMPTY_FUNC || func == mfd_get_func(mid, player_struct.mfd_current_slots[mid])) {
                // don't restore func that we already have on some mfd.
                if (default_mfds[def].qual()) {
                    func = default_mfds[def].func;
                    slot = default_mfds[def].slot;
                }
                def++;
                check = TRUE;
                break;
            }
        }
    } while (check && def < NUM_MFD_DEFAULTS);

    if (func == MFD_EMPTY_FUNC)
        slot = (global_fullmap->cyber) ? MFD_INFO_SLOT : MFD_ITEM_SLOT; // failure case
    mfd_notify_func(func, slot, TRUE, MFD_ACTIVE, TRUE);
    if (!full_game_3d || (full_visible & FULL_MFD_MASK(mfd_id)))
        mfd_change_slot(mfd_id, slot);
}

// scans throught the mfd's looking for mfd's that are set to the given slot/func.
// once it have found max such mfd's, starts setting any subsequent mfd's
// with that func to defaults as above.

void cap_mfds_with_func(uchar func, uchar max) {
    int mid;

    for (mid = 0; mid < NUM_MFDS; mid++) {
        if (mfd_get_func(mid, player_struct.mfd_current_slots[mid]) == func) {
            if (max == 0)
                restore_mfd_slot(mid);
            else
                max--;
        }
    }
}

void restore_mfd_slot(int mfd_id) {
    uchar func, slot;
    if (global_fullmap->cyber)
        return;
    if (full_game_3d && !(visible_mask(mfd_id) & full_visible))
        return;
    if (player_struct.mfd_save_slot[mfd_id] < 0) {
        func = MFD_EMPTY_FUNC;
        slot = MFD_INFO_SLOT;
    } else {
        func = player_struct.mfd_all_slots[MFD_SPECIAL_SLOT + mfd_id];
        slot = player_struct.mfd_save_slot[mfd_id];
    }

    set_mfd_from_defaults(mfd_id, func, slot);
    player_struct.mfd_save_slot[mfd_id] = -1;
    full_visible &= ~(visible_mask(mfd_id));
#ifdef STEREO_SUPPORT
    if (convert_use_mode == 5)
        full_visible = (player_struct.mfd_save_vis & visible_mask(mfd_id));
    else
#endif
    {
        full_visible |= (player_struct.mfd_save_vis & visible_mask(mfd_id));
    }
}
