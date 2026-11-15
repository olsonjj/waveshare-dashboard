/**
 * ui_tokens.h — design tokens for the Waveshare dashboard (v2, "solar ops" theme).
 *
 * Design tokens = the named, one-place definitions of every visual constant
 * (colors, radii, type scale, spacing, opacity). Widgets should reference
 * tokens, never raw hex/palette values — change the token, restyle the app.
 *
 * Layer semantics:
 *   1. Primitives  — hex values, no meaning
 *   2. Tokens      — named constants ("solar_amber")
 *   3. Semantics   — role mapping ("color_power" = amber, "panel" = card bg)
 * Widgets use layer 3 (and layer 2 for role-agnostic accents).
 */
#ifndef UI_TOKENS_H
#define UI_TOKENS_H

#include <lvgl.h>

namespace ui {

/* -------- 1/2. primitives & tokens -------- */

inline lv_color_t background(void)      { return lv_color_hex(0x070B10); }  /* screen bg            */
inline lv_color_t panel(void)           { return lv_color_hex(0x0D1319); }  /* card fill (opaque)   */
inline lv_color_t panel_deep(void)      { return lv_color_hex(0x0A0F15); }  /* header / chips bg    */
inline lv_color_t border(void)          { return lv_color_hex(0x2C3A4C); }  /* chip/card outline    */
inline lv_color_t grid(void)            { return lv_color_hex(0x1B2531); }  /* chart gridlines      */

inline lv_color_t text(void)            { return lv_color_hex(0xF2F6FC); }  /* primary text         */
inline lv_color_t text_dim(void)        { return lv_color_hex(0x94A3B8); }  /* secondary text       */
inline lv_color_t text_faint(void)      { return lv_color_hex(0x5C6B7F); }  /* axis labels          */

inline lv_color_t solar_amber(void)     { return lv_color_hex(0xF5A623); }  /* sun / power          */
inline lv_color_t amber_tint(void)      { return lv_color_hex(0x271C08); }  /* amber-tinted fill    */
inline lv_color_t green(void)           { return lv_color_hex(0x34D399); }  /* ok / yield           */
inline lv_color_t green_tint(void)      { return lv_color_hex(0x0F2A20); }  /* green-tinted fill    */
inline lv_color_t yellow(void)          { return lv_color_hex(0xFACC15); }  /* gradient bar end     */
inline lv_color_t cyan(void)            { return lv_color_hex(0x38BDF8); }  /* "now" / sync marker  */
inline lv_color_t cyan_tint(void)       { return lv_color_hex(0x0C2233); }  /* tooltip fill         */
inline lv_color_t violet(void)          { return lv_color_hex(0xA78BFA); }  /* web / commits (rsvd) */

/* -------- 3. semantic aliases -------- */

inline lv_color_t color_power(void)     { return solar_amber(); }
inline lv_color_t color_yield(void)     { return green(); }
inline lv_color_t color_now(void)       { return cyan(); }
inline lv_color_t color_github(void)    { return green(); }   /* repos pill, active gh tab */

/* -------- radii -------- */
enum : int {
    radius_card = 18,    /* panels */
    radius_ctrl = 10,    /* chips, small controls */
    radius_pill = 320,   /* >= half height = pill */
};

/* -------- type scale (Montserrat, enabled in lv_conf.h) -------- */
enum font_e : uint8_t {
    FONT_SMALL  = 12,
    FONT_BODY   = 14,
    FONT_TITLE  = 16,
    FONT_UI     = 20,
    FONT_STAT   = 24,
    FONT_HERO   = 48,
};
inline const lv_font_t *font(font_e s)
{
    switch (s) {
    case FONT_HERO:  return &lv_font_montserrat_48;
    case FONT_STAT:  return &lv_font_montserrat_24;
    case FONT_UI:    return &lv_font_montserrat_20;
    case FONT_TITLE: return &lv_font_montserrat_16;
    case FONT_SMALL: return &lv_font_montserrat_12;
    default:         return &lv_font_montserrat_14;
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
    opa_surf     = LV_OPA_COVER,     /* panels are fully opaque          */
    opa_track    = LV_OPA_30,        /* arc/bar unused part              */
    opa_tint_14  = 36,               /* ~14% tint fills (pills)          */
};

} // namespace ui

#endif /* UI_TOKENS_H */
