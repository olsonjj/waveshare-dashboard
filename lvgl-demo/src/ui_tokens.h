/**
 * ui_tokens.h — design tokens for the Waveshare dashboard.
 *
 * Design tokens = the named, one-place definitions of every visual constant
 * (colors, radii, type scale, spacing, opacity). Widgets should reference
 * tokens, never raw hex/palette values — change the token, restyle the app.
 *
 * Layers, from raw to semantic:
 *   1. Primitives  — hex values, no meaning ("#F59E0B")
 *   2. Tokens      — named constants ("solar_amber")
 *   3. Semantics   — role mapping ("color_ok" = green, "surface" = card bg)
 * Widgets use layer 3 (and layer 2 for role-agnostic accents).
 *
 * Feel free to retune freely — nothing outside this file hardcodes looks.
 */
#ifndef UI_TOKENS_H
#define UI_TOKENS_H

#include <lvgl.h>

namespace ui {

/* -------- 2. palette tokens (layer 1 lives here as literals) -------- */

inline lv_color_t background(void)      { return lv_color_hex(0x0B0F14); }  /* near-black slate   */
inline lv_color_t surface(void)         { return lv_color_hex(0x1A2230); }  /* card fill          */
inline lv_color_t surf_hi(void)         { return lv_color_hex(0x243045); }  /* hover / emphasis   */
inline lv_color_t border(void)          { return lv_color_hex(0x8AA3C8); }  /* hairline, low opa  */
inline lv_color_t text(void)            { return lv_color_hex(0xE8EEF7); }  /* primary text       */
inline lv_color_t text_dim(void)        { return lv_color_hex(0x93A3B8); }  /* secondary text     */
inline lv_color_t accent(void)          { return lv_color_hex(0x2DD4BF); }  /* teal — brand       */
inline lv_color_t solar_amber(void)     { return lv_color_hex(0xF5A623); }  /* sun / production   */
inline lv_color_t violet(void)          { return lv_color_hex(0xA78BFA); }  /* web / commits      */
inline lv_color_t sky(void)             { return lv_color_hex(0x58A6FF); }  /* links / info       */

inline lv_color_t ok(void)              { return lv_color_hex(0x3DD68C); }  /* healthy states     */
inline lv_color_t warn(void)            { return lv_color_hex(0xF2C744); }  /* approaching limit  */
inline lv_color_t bad(void)             { return lv_color_hex(0xFF6B6B); }  /* errors / alarms    */

/* -------- 3. semantic aliases -------- */

inline lv_color_t color_power(void)     { return solar_amber(); }
inline lv_color_t color_commits(void)   { return violet(); }
inline lv_color_t color_ci(void)        { return ok(); }
inline lv_color_t color_pr(void)        { return sky(); }
inline lv_color_t color_issues(void)    { return bad(); }
inline lv_color_t color_battery(void)   { return ok(); }

/* -------- radii -------- */
enum : int {
    radius_card = 16,   /* status cards / panels   */
    radius_ctrl = 10,   /* bars, gauges' ends      */
    radius_pill = 320,  /* >= half height = pill   */
};

/* -------- type scale (Montserrat, all enabled in lv_conf.h) -------- */
enum font_e : uint8_t {
    FONT_BODY   = 14,
    FONT_UI     = 20,
    FONT_NUMBER = 32,
    FONT_HERO   = 48,
};
inline const lv_font_t *font(font_e s)
{
    switch (s) {
    case FONT_HERO:   return &lv_font_montserrat_48;
    case FONT_NUMBER: return &lv_font_montserrat_32;
    case FONT_UI:     return &lv_font_montserrat_20;
    default:          return &lv_font_montserrat_14;
    }
}

/* -------- spacing -------- */
enum : int {
    space_xs = 4,
    space_s  = 8,
    space_m  = 14,
    space_l  = 24,
    space_xl = 40,
};

/* -------- opacities -------- */
enum : lv_opa_t {
    opa_surf     = LV_OPA_80,        /* card fill vs screen bg     */
    opa_border   = 48,               /* hairline card outline      */
    opa_track    = LV_OPA_20,        /* arc/bar/chart unused part  */
};

} // namespace ui

#endif /* UI_TOKENS_H */
