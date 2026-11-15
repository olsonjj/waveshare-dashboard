/**
 * dashboard_ui.cpp — v2 "Solar Ops" dashboard, built to the design mockup.
 *
 * Screen 800x480, dark theme, two functional tabs:
 *   Tab 1 "SOLAR TELEMETRY" — current power card + donut, total yield card
 *                             with target bar, generation-curve chart card
 *                             (area fill, grid, max marker, "now" cursor).
 *   Tab 2 "GITHUB OPS"      — intentionally empty for now (design only).
 *
 * All data is mock/static (values from the mockup); a 1 s timer ticks the
 * header clock from the system clock (TZ set in main.cpp). Hand-rolled tab
 * header for full badge/pill styling.
 */
#include <time.h>
#include "dashboard_ui.h"
#include "ui_tokens.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cmath>

#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

using namespace ui;

/* ------------------------------------------------------------------ */
/* big one-off buffers (area canvas) — SPIRAM on device, malloc on host */
/* ------------------------------------------------------------------ */
static void *ui_alloc_bytes(size_t n)
{
#ifdef ARDUINO
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(n);
#endif
}

/* ---------------- mock day curve ---------------- */
/* Hourly anchors 05:00..20:00 in 0.01 kW units. 13:00 -> 4.85 kW.      */
static const int CURVE_ANCHORS[16] =
    {2, 6, 28, 85, 170, 265, 350, 428, 485, 430, 360, 270, 175, 95, 30, 5};

static constexpr int N_PTS = 48;      /* chart points over 15 h          */
static constexpr int Y_MAX = 5000;    /* chart range: 5.00 kW            */
static constexpr double H0 = 5.0, H1 = 20.0;
static constexpr double NOW_H   = 14.0 + 38.0 / 60.0;
static constexpr double NOW_KW  = 3.84;
static constexpr double PEAK_KW = 4.85;
static constexpr double PEAK_H  = 13.0;

/* cosine-interpolated value at fractional hour h -> 0.01 kW */
static double curve_at(double h)
{
    if (h <= H0) return CURVE_ANCHORS[0];
    if (h >= H1) return CURVE_ANCHORS[15];
    double t = h - H0;
    int i = (int)floor(t);
    double frac = t - i;
    double w = (1.0 - cos(M_PI * frac)) * 0.5;
    return CURVE_ANCHORS[i] + (CURVE_ANCHORS[i + 1] - CURVE_ANCHORS[i]) * w;
}

/* ---------------- small widget helpers ---------------- */
static lv_obj_t *make_panel(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_pad_all(p, 0, 0);      /* children at exact coordinates */
    lv_obj_set_style_radius(p, radius_card, 0);
    lv_obj_set_style_bg_color(p, panel(), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, border(), 0);
    lv_obj_set_style_border_opa(p, 110, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return p;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *txt, font_e f,
                            lv_color_t color, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font(f), 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

/* pill badge; position set by caller (align_to chains) */
static lv_obj_t *make_pill(lv_obj_t *parent, const char *text, lv_color_t fg,
                           lv_opa_t bg_opa, bool outlined = false,
                           lv_color_t bg = lv_color_black(),
                           font_e fs = FONT_SMALL)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(p, 9, 0);
    lv_obj_set_style_pad_ver(p, 4, 0);
    lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(p, bg, 0);
    lv_obj_set_style_bg_opa(p, bg_opa, 0);
    if (outlined) {
        lv_obj_set_style_border_color(p, fg, 0);
        lv_obj_set_style_border_width(p, 1, 0);
    } else {
        lv_obj_set_style_border_width(p, 0, 0);
    }
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, fg, 0);
    lv_obj_set_style_text_font(l, font(fs), 0);
    lv_obj_center(l);
    return p;
}

/* filled "up" triangle (replaces unavailable unicode glyphs) */
static lv_obj_t *make_tri(lv_obj_t *parent, lv_color_t c, int w, int h)
{
    void *buf = ui_alloc_bytes((size_t)w * h * 4);
    lv_obj_t *cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(cv, buf, w, h, LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_canvas_fill_bg(cv, c, LV_OPA_TRANSP);
    for (int y = 0; y < h; y++) {
        int span = (int)((double)(h - y) / h * w + 0.5);
        int x0 = (w - span) / 2;
        for (int x = x0; x < x0 + span; x++) {
            lv_canvas_set_px_color(cv, x, y, c);
            lv_canvas_set_px_opa(cv, x, y, LV_OPA_COVER);
        }
    }
    lv_obj_clear_flag(cv, LV_OBJ_FLAG_CLICKABLE);
    return cv;
}

/* sun icon, 20x20 */
static lv_obj_t *make_sun_icon(lv_obj_t *parent)
{
    static uint8_t buf[20 * 20 * 4];
    lv_obj_t *cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(cv, buf, 20, 20, LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_TRANSP);
    lv_color_t c = solar_amber();
    for (int y = 0; y < 20; y++) {
        for (int x = 0; x < 20; x++) {
            double dx = x - 9.5, dy = y - 9.5;
            double d = sqrt(dx * dx + dy * dy);
            if (d <= 4.2) {
                lv_canvas_set_px_color(cv, x, y, c);
                lv_canvas_set_px_opa(cv, x, y, LV_OPA_COVER);
            } else if (d >= 6.8 && d <= 9.4) {
                double ang = atan2(dy, dx);
                int k = (int)lround(ang / (M_PI / 4));
                double dev = fabs(ang - k * (M_PI / 4));
                if (dev < 0.26 || fabs(M_PI - dev) < 0.26) {
                    lv_canvas_set_px_color(cv, x, y, c);
                    lv_canvas_set_px_opa(cv, x, y, LV_OPA_COVER);
                }
            }
        }
    }
    lv_obj_clear_flag(cv, LV_OBJ_FLAG_CLICKABLE);
    return cv;
}

/* GitHub mark approximation, 20x20 */
static lv_obj_t *make_gh_icon(lv_obj_t *parent)
{
    static uint8_t buf[20 * 20 * 4];
    lv_obj_t *cv = lv_canvas_create(parent);
    lv_canvas_set_buffer(cv, buf, 20, 20, LV_IMG_CF_TRUE_COLOR_ALPHA);
    lv_canvas_fill_bg(cv, lv_color_black(), LV_OPA_TRANSP);
    lv_color_t c = text_dim();
    for (int y = 0; y < 20; y++) {
        for (int x = 0; x < 20; x++) {
            double dx = x - 9.5, dy = y - 9.5;
            if (dx * dx + dy * dy <= 72.0) {
                lv_canvas_set_px_color(cv, x, y, c);
                lv_canvas_set_px_opa(cv, x, y, LV_OPA_COVER);
            }
        }
    }
    struct Tri { double x1, y1, x2, y2, x3, y3; };
    Tri ears[2] = {{3.5, 0.5, 1.0, 5.0, 7.0, 3.0},
                   {15.5, 0.5, 18.0, 5.0, 12.0, 3.0}};
    for (int y = 0; y < 20; y++) {
        for (int x = 0; x < 20; x++) {
            for (auto &t : ears) {
                auto sgn = [](double ax, double ay, double bx, double by,
                              double cx, double cy) {
                    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
                };
                bool neg = sgn(t.x1, t.y1, t.x2, t.y2, x, y) < 0 ||
                           sgn(t.x2, t.y2, t.x3, t.y3, x, y) < 0 ||
                           sgn(t.x3, t.y3, t.x1, t.y1, x, y) < 0;
                bool pos = sgn(t.x1, t.y1, t.x2, t.y2, x, y) > 0 ||
                           sgn(t.x2, t.y2, t.x3, t.y3, x, y) > 0 ||
                           sgn(t.x3, t.y3, t.x1, t.y1, x, y) > 0;
                if (!(neg && pos)) {
                    lv_canvas_set_px_opa(cv, x, y, LV_OPA_TRANSP);
                }
            }
        }
    }
    lv_obj_clear_flag(cv, LV_OBJ_FLAG_CLICKABLE);
    return cv;
}

/* dashed line with persistent point storage */
static lv_obj_t *make_dash(lv_obj_t *parent, lv_color_t c, int line_w,
                           int x, int y, int w_pt, int h_pt,
                           int dash_w, int dash_gap)
{
    lv_obj_t *ln = lv_line_create(parent);
    lv_point_t *pts = (lv_point_t *)ui_alloc_bytes(sizeof(lv_point_t) * 2);
    pts[0].x = 0; pts[0].y = 0;
    pts[1].x = (lv_coord_t)w_pt; pts[1].y = (lv_coord_t)h_pt;
    lv_line_set_points(ln, pts, 2);
    lv_obj_set_pos(ln, x, y);
    lv_obj_set_size(ln, w_pt > 0 ? w_pt : 1, h_pt > 0 ? h_pt : 1);
    lv_obj_set_style_line_color(ln, c, 0);
    lv_obj_set_style_line_width(ln, line_w, 0);
    lv_obj_set_style_line_dash_width(ln, dash_w, 0);
    lv_obj_set_style_line_dash_gap(ln, dash_gap, 0);
    lv_obj_clear_flag(ln, LV_OBJ_FLAG_CLICKABLE);
    return ln;
}

static lv_obj_t *make_dot(lv_obj_t *parent, int d, lv_color_t fill,
                          lv_color_t ring, int x, int y)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, d, d);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, fill, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_border_color(o, ring, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

/* ------------------------------------------------------------------ */
/* module state & chart geometry                                       */
/* ------------------------------------------------------------------ */
static lv_obj_t *tab_btn[2];
static lv_obj_t *tab_underline[2];
static lv_obj_t *s_power_val_lbl = nullptr;   /* CURRENT POWER card value (kW) */
static lv_obj_t *s_power_peak_lbl = nullptr;  /* CURRENT POWER card peak row (kW) */
static lv_obj_t *s_power_unit_lbl = nullptr;  /* kW unit; re-aligned each update */
static lv_obj_t *s_yield_val_lbl = nullptr;   /* TOTAL YIELD card value (kWh) */
static lv_obj_t *s_yield_unit_lbl = nullptr;  /* kWh unit; re-aligned each update */
static lv_obj_t *s_power_upd_lbl = nullptr;  /* last-updated pill label */

/* GitHub tab is design-only for now — hidden until it gets real data. */
#define GITHUB_TAB_ENABLED 0
static lv_obj_t *tab_title[2];
static lv_obj_t *panel_solar, *panel_github;
static lv_obj_t *clock_label;
static int sel_tab = 0;

/* generation-curve card: chart is placed at (14,38) inside the card */
static constexpr int CH_W = 748, CH_H = 214;
static constexpr int PAD_L = 40, PAD_R = 12, PAD_T = 8, PAD_B = 24;
static constexpr int PLOT_W = CH_W - PAD_L - PAD_R;   /* 696 */
static constexpr int PLOT_H = CH_H - PAD_T - PAD_B;   /* 182 */

/* px position (within the chart object) of fractional hour h */
static int hour_to_px(double h)
{
    double idx = (h - H0) / (H1 - H0) * (N_PTS - 1);
    return (int)(PAD_L + idx * ((double)PLOT_W / (N_PTS - 1)) + 0.5);
}
/* py position (within the chart object) of a kW value */
static int kw_to_py(double kw)
{
    return (int)(PAD_T + (1.0 - kw * 1000.0 / Y_MAX) * PLOT_H + 0.5);
}

/* ------------------------------------------------------------------ */
/* tabs                                                                */
/* ------------------------------------------------------------------ */
static void select_tab(int idx)
{
    sel_tab = idx;
    for (int i = 0; i < (GITHUB_TAB_ENABLED ? 2 : 1); i++) {
        bool on = (i == sel_tab);
        lv_obj_set_style_bg_opa(tab_btn[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(tab_title[i],
                                    on ? (i == 0 ? solar_amber() : color_github())
                                       : text_dim(), 0);
        if (on) lv_obj_clear_flag(tab_underline[i], LV_OBJ_FLAG_HIDDEN);
        else    lv_obj_add_flag(tab_underline[i], LV_OBJ_FLAG_HIDDEN);
    }
#if GITHUB_TAB_ENABLED
    if (sel_tab == 0) { lv_obj_clear_flag(panel_solar, LV_OBJ_FLAG_HIDDEN);
                        lv_obj_add_flag(panel_github, LV_OBJ_FLAG_HIDDEN); }
    else              { lv_obj_clear_flag(panel_github, LV_OBJ_FLAG_HIDDEN);
                        lv_obj_add_flag(panel_solar, LV_OBJ_FLAG_HIDDEN); }
#else
    /* single-tab build: solar panel is the only content; panel_github was
     * never created, so select_tab() must not touch it */
    lv_obj_clear_flag(panel_solar, LV_OBJ_FLAG_HIDDEN);
#endif
}

static void tab_click_cb(lv_event_t *e)
{
    select_tab((int)(intptr_t)lv_event_get_user_data(e));
}

/* ------------------------------------------------------------------ */
/* header                                                              */
/* ------------------------------------------------------------------ */
static void build_header(lv_obj_t *scr)
{
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, 800, 52);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, panel_deep(), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    /* --- tab 0: SOLAR TELEMETRY --- */
    lv_obj_t *b0 = lv_btn_create(header);
    tab_btn[0] = b0;
    lv_obj_set_size(b0, 258, 44);
    lv_obj_set_pos(b0, 12, 4);
    lv_obj_set_style_radius(b0, 10, 0);
    lv_obj_set_style_bg_color(b0, amber_tint(), 0);
    lv_obj_set_style_bg_opa(b0, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(b0, 0, 0);
    lv_obj_set_style_border_width(b0, 0, 0);
    lv_obj_set_style_pad_all(b0, 0, 0);

    lv_obj_t *sun = make_sun_icon(b0);
    lv_obj_align(sun, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *t0 = lv_label_create(b0);
    tab_title[0] = t0;
    lv_label_set_text(t0, "SOLAR TELEMETRY");
    lv_obj_set_style_text_font(t0, font(FONT_TITLE), 0);
    lv_obj_set_style_text_color(t0, solar_amber(), 0);
    lv_obj_align_to(t0, sun, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    lv_obj_t *live = make_pill(b0, "LIVE", lv_color_hex(0x1A1206), LV_OPA_COVER,
                               false, solar_amber());
    lv_obj_align(live, LV_ALIGN_RIGHT_MID, -10, 0);

    lv_obj_add_event_cb(b0, tab_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)0);

    /* --- tab 1: GITHUB OPS (hidden until built) --- */
#if GITHUB_TAB_ENABLED
    lv_obj_t *b1 = lv_btn_create(header);
    tab_btn[1] = b1;
    lv_obj_set_size(b1, 234, 44);
    lv_obj_set_pos(b1, 278, 4);
    lv_obj_set_style_radius(b1, 10, 0);
    lv_obj_set_style_bg_color(b1, green_tint(), 0);
    lv_obj_set_style_bg_opa(b1, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(b1, 0, 0);
    lv_obj_set_style_border_width(b1, 0, 0);
    lv_obj_set_style_pad_all(b1, 0, 0);

    lv_obj_t *gh = make_gh_icon(b1);
    lv_obj_align(gh, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *t1 = lv_label_create(b1);
    tab_title[1] = t1;
    lv_label_set_text(t1, "GITHUB OPS");
    lv_obj_set_style_text_font(t1, font(FONT_TITLE), 0);
    lv_obj_set_style_text_color(t1, text_dim(), 0);
    lv_obj_align_to(t1, gh, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    lv_obj_t *repos = make_pill(b1, "2 REPOS", color_github(), opa_tint_14,
                                false, green_tint());
    lv_obj_align(repos, LV_ALIGN_RIGHT_MID, -10, 0);

    lv_obj_add_event_cb(b1, tab_click_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
#endif /* GITHUB_TAB_ENABLED */

    /* --- underlines (header children; hidden unless active) --- */
    lv_obj_t *ul0 = lv_obj_create(header);
    lv_obj_set_size(ul0, 258, 3);
    lv_obj_set_pos(ul0, 12, 49);
    lv_obj_set_style_bg_color(ul0, solar_amber(), 0);
    lv_obj_set_style_bg_opa(ul0, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ul0, 0, 0);
    lv_obj_set_style_border_width(ul0, 0, 0);
    lv_obj_clear_flag(ul0, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    tab_underline[0] = ul0;

#if GITHUB_TAB_ENABLED
    lv_obj_t *ul1 = lv_obj_create(header);
    lv_obj_set_size(ul1, 234, 3);
    lv_obj_set_pos(ul1, 278, 49);
    lv_obj_set_style_bg_color(ul1, color_github(), 0);
    lv_obj_set_style_bg_opa(ul1, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(ul1, 0, 0);
    lv_obj_set_style_border_width(ul1, 0, 0);
    lv_obj_clear_flag(ul1, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(ul1, LV_OBJ_FLAG_HIDDEN);
    tab_underline[1] = ul1;
#endif /* GITHUB_TAB_ENABLED */

    /* --- right cluster: signal bars, clock --- */
    static const uint8_t bar_h[3] = {6, 10, 14};
    for (int i = 0; i < 3; i++) {
        lv_obj_t *bar = lv_obj_create(header);
        lv_obj_set_size(bar, 5, bar_h[i]);
        lv_obj_set_pos(bar, 635 + i * 9, 34 - bar_h[i]);   /* bottom-aligned */
        lv_obj_set_style_pad_all(bar, 0, 0);
        lv_obj_set_style_bg_color(bar, green(), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, 1, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *clk = lv_obj_create(header);
    lv_obj_set_size(clk, 118, 32);
    lv_obj_align(clk, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_pad_all(clk, 0, 0);
    lv_obj_set_style_bg_color(clk, background(), 0);
    lv_obj_set_style_bg_opa(clk, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(clk, radius_ctrl, 0);
    lv_obj_set_style_border_color(clk, border(), 0);
    lv_obj_set_style_border_width(clk, 1, 0);
    lv_obj_clear_flag(clk, LV_OBJ_FLAG_SCROLLABLE);
    clock_label = make_label(clk, "14:38:22", FONT_BODY, text(), 0, 0);
    lv_obj_center(clock_label);
}

/* ------------------------------------------------------------------ */
/* solar tab                                                           */
/* ------------------------------------------------------------------ */
static void card_accent(lv_obj_t *card, lv_color_t c)
{
    lv_obj_t *a = lv_obj_create(card);
    lv_obj_set_size(a, 4, 106);
    lv_obj_align(a, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_bg_color(a, c, 0);
    lv_obj_set_style_bg_opa(a, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(a, 2, 0);
    lv_obj_set_style_border_width(a, 0, 0);
    lv_obj_clear_flag(a, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}

static void build_power_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_panel(parent, 380, 132);
    lv_obj_set_pos(card, 12, 10);
    card_accent(card, color_power());

    lv_obj_t *title_lbl = make_label(card, "CURRENT POWER", FONT_BODY, text_dim(), 20, 10);

    lv_obj_t *pill = make_pill(card, "Last updated: --:--", solar_amber(), opa_tint_14,
                               false, amber_tint());
    lv_obj_align_to(pill, title_lbl, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    s_power_upd_lbl = lv_obj_get_child(pill, 0);   /* the pill's label */

    lv_obj_t *val = make_label(card, "3.84", FONT_HERO, color_power(), 20, 36);
    s_power_val_lbl = val;
    lv_obj_t *unit = make_label(card, "kW", FONT_TITLE, text_dim(), 0, 0);
    lv_obj_align_to(unit, val, LV_ALIGN_OUT_RIGHT_BOTTOM, 12, -14);
    s_power_unit_lbl = unit;

    lv_obj_t *tri = make_tri(card, green(), 10, 8);
    lv_obj_align(tri, LV_ALIGN_BOTTOM_LEFT, 20, -12);
    lv_obj_t *peak = make_label(card, "Peak -.- kW", FONT_BODY, green(), 0, 0);
    s_power_peak_lbl = peak;
    lv_obj_align_to(peak, tri, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
}

static void build_yield_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_panel(parent, 384, 132);
    lv_obj_set_pos(card, 404, 10);
    card_accent(card, color_yield());

    make_label(card, "TOTAL YIELD TODAY", FONT_BODY, text_dim(), 20, 10);

    lv_obj_t *val = make_label(card, "21.65", FONT_HERO, text(), 20, 36);
    s_yield_val_lbl = val;
    lv_obj_t *unit = make_label(card, "kWh", FONT_TITLE, color_yield(), 0, 0);
    lv_obj_align_to(unit, val, LV_ALIGN_OUT_RIGHT_BOTTOM, 12, -14);
    s_yield_unit_lbl = unit;
}

static void build_chart_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_panel(parent, 776, 266);
    lv_obj_set_pos(card, 12, 154);

    make_label(card, "SOLAR GENERATION CURVE - TODAY", FONT_TITLE, text(), 18, 10);

    /* legend */
    lv_obj_t *sw = lv_obj_create(card);
    lv_obj_set_size(sw, 18, 9);
    lv_obj_set_pos(sw, 445, 18);
    lv_obj_set_style_pad_all(sw, 0, 0);
    lv_obj_set_style_radius(sw, 4, 0);
    lv_obj_set_style_bg_color(sw, color_power(), 0);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sw, 0, 0);
    lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *leg_t = make_label(card, "Actual (kWh)", FONT_BODY, text_dim(), 0, 0);
    lv_obj_align_to(leg_t, sw, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

    lv_obj_t *peak_pill = make_pill(card, "Peak: 4.85 kWh", solar_amber(),
                                    opa_tint_14, false, amber_tint(), FONT_BODY);
    lv_obj_align(peak_pill, LV_ALIGN_TOP_RIGHT, -18, 6);

    /* area canvas — created BEFORE the chart so it sits under it */
    lv_obj_t *area = lv_canvas_create(card);
    void *abuf = ui_alloc_bytes((size_t)PLOT_W * PLOT_H * 2);
    lv_canvas_set_buffer(area, abuf, PLOT_W, PLOT_H, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(area, panel(), LV_OPA_COVER);
    for (int cx = 0; cx < PLOT_W; cx++) {
        double hcol = H0 + (double)cx / (PLOT_W - 1) * (H1 - H0);
        double v = curve_at(hcol) * 10.0;              /* 0..5000 */
        int ytop = (int)((1.0 - v / Y_MAX) * PLOT_H);
        for (int cy = ytop; cy < PLOT_H; cy++) {
            double t = (double)(cy - ytop) / (PLOT_H - ytop + 1);
            double fade = 1.0 - t;
            int mix = (int)(fade * fade * 95);         /* max ~37% amber */
            if (mix < 2) mix = 2;
            lv_canvas_set_px_color(area, cx, cy,
                                   lv_color_mix(color_power(), panel(), (lv_opa_t)mix));
        }
    }
    lv_obj_set_pos(area, 14 + PAD_L, 38 + PAD_T);
    lv_obj_set_style_pad_all(area, 0, 0);
    lv_obj_clear_flag(area, LV_OBJ_FLAG_CLICKABLE);

    /* grid lines: children of the canvas => under the chart's amber line */
    for (int k = 0; k <= 5; k++) {
        make_dash(area, grid(), 1, 0, k * PLOT_H / 5, PLOT_W, 0, 3, 5);
    }
    for (double h = 7.0; h <= 19.0; h += 2.0) {
        int x = hour_to_px(h) - PAD_L;
        make_dash(area, grid(), 1, x, 0, 0, PLOT_H, 4, 6);
    }

    /* the chart */
    lv_obj_t *chart = lv_chart_create(card);
    lv_obj_set_size(chart, CH_W, CH_H);
    lv_obj_set_pos(chart, 14, 38);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, N_PTS);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, Y_MAX);
    lv_chart_set_div_line_count(chart, 0, 0);   /* grid drawn on the canvas */
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_radius(chart, 0, 0);
    lv_obj_set_style_line_width(chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(chart, PAD_L, 0);
    lv_obj_set_style_pad_right(chart, PAD_R, 0);
    lv_obj_set_style_pad_top(chart, PAD_T, 0);
    lv_obj_set_style_pad_bottom(chart, PAD_B, 0);
    lv_chart_series_t *ser =
        lv_chart_add_series(chart, color_power(), LV_CHART_AXIS_PRIMARY_Y);
    for (int i = 0; i < N_PTS; i++) {
        double h = H0 + (double)i / (N_PTS - 1) * (H1 - H0);
        ser->y_points[i] = (int32_t)curve_at(h) * 10;   /* 0.01kW -> 0..5000 */
    }
    ser->y_points[25] = (int32_t)(PEAK_KW * 1000);   /* exact peak @ ~13:00 */
    lv_chart_refresh(chart);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_SCROLLABLE);

    /* y-axis labels (card children, left gutter) */
    for (int k = 0; k <= 5; k++) {
        char yl[8];
        snprintf(yl, sizeof(yl), "%d.0", k);
        lv_obj_t *l = lv_label_create(card);
        lv_label_set_text(l, yl);
        lv_obj_set_style_text_font(l, font(FONT_BODY), 0);
        lv_obj_set_style_text_color(l, text_faint(), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_size(l, 30, LV_SIZE_CONTENT);
        lv_obj_set_pos(l, 14 + PAD_L - 36,
                       38 + PAD_T + PLOT_H - k * PLOT_H / 5 - 7);
    }

    /* x-axis labels (card children, bottom gutter) */
    struct XTick { double h; const char *txt; int role; /* 0 dim 1 amber 2 cyan */ };
    XTick xt[] = {
        {5.0,  "05:00", 0},
        {7.0,  "07:00", 0},
        {9.0,  "09:00", 0},
        {11.0, "11:00", 0},
        {13.0, "13:00 (Solar Noon)", 1},
        {15.0, "15:00", 2},
        {17.0, "17:00", 0},
        {19.0, "19:00", 0},
        {20.0, "20:00", 0},
    };
    for (auto &t : xt) {
        lv_color_t c = t.role == 1 ? solar_amber()
                     : t.role == 2 ? cyan() : text_faint();
        lv_obj_t *l = lv_label_create(card);
        lv_label_set_text(l, t.txt);
        lv_obj_set_style_text_font(l, font(t.role == 1 ? FONT_BODY : FONT_SMALL), 0);
        lv_obj_set_style_text_color(l, c, 0);
        int px = 14 + hour_to_px(t.h);
        lv_point_t sz;
        lv_txt_get_size(&sz, t.txt, font(t.role == 1 ? FONT_BODY : FONT_SMALL),
                        0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        lv_obj_set_pos(l, px - sz.x / 2, 38 + PAD_T + PLOT_H + 7);
    }

    /* peak marker: dot is a chart child (drawn over the series); the
     * "Max" caption is a card child so it can poke into the top pad. */
    int px_peak = hour_to_px(PEAK_H);
    int py_peak = kw_to_py(PEAK_KW);
    make_dot(chart, 10, panel(), solar_amber(),
             px_peak - PAD_L - 5, py_peak - PAD_T - 5);
    lv_obj_t *maxl = make_label(card, "Max 4.85", FONT_BODY, solar_amber(),
                                14 + px_peak - 24, 38 + py_peak - 30);

    /* "now" cursor */
    int px_now = hour_to_px(NOW_H);
    int py_now = kw_to_py(NOW_KW);
    make_dash(chart, cyan(), 2, px_now - PAD_L, 0, 0, PLOT_H, 6, 5);
    make_dot(chart, 12, cyan(), panel(), px_now - PAD_L - 6, py_now - PAD_T - 6);

    lv_obj_t *tip = lv_obj_create(chart);
    lv_obj_set_size(tip, 122, 46);
    lv_obj_set_pos(tip, px_now + 10 - PAD_L, py_now - PAD_T - 34);
    lv_obj_set_style_bg_color(tip, cyan_tint(), 0);
    lv_obj_set_style_bg_opa(tip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tip, 8, 0);
    lv_obj_set_style_border_color(tip, cyan(), 0);
    lv_obj_set_style_border_width(tip, 1, 0);
    lv_obj_set_style_pad_all(tip, 6, 0);
    lv_obj_clear_flag(tip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *tt1 = lv_label_create(tip);
    lv_label_set_text(tt1, "14:38 NOW");
    lv_obj_set_style_text_font(tt1, font(FONT_SMALL), 0);
    lv_obj_set_style_text_color(tt1, cyan(), 0);
    lv_obj_align(tt1, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *tt2 = lv_label_create(tip);
    lv_label_set_text(tt2, "3.84 kWh");
    lv_obj_set_style_text_font(tt2, font(FONT_BODY), 0);
    lv_obj_set_style_text_color(tt2, text(), 0);
    lv_obj_align_to(tt2, tt1, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 3);

    (void)maxl;
}

/* ------------------------------------------------------------------ */
/* clock + entry points                                                */
/* ------------------------------------------------------------------ */
static void clock_timer_cb(lv_timer_t *)
{
    time_t now = time(nullptr);
    struct tm tmbuf;
    localtime_r(&now, &tmbuf);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tmbuf);
    lv_label_set_text(clock_label, buf);
}

void dashboard_ui_init(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, background(), 0);

    build_header(scr);

    panel_solar = lv_obj_create(scr);
    lv_obj_set_size(panel_solar, 800, 428);
    lv_obj_set_pos(panel_solar, 0, 52);
    lv_obj_set_style_bg_opa(panel_solar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(panel_solar, 0, 0);
    lv_obj_set_style_pad_all(panel_solar, 0, 0);
    lv_obj_clear_flag(panel_solar, LV_OBJ_FLAG_SCROLLABLE);

#if GITHUB_TAB_ENABLED
    panel_github = lv_obj_create(scr);
    lv_obj_set_size(panel_github, 800, 428);
    lv_obj_set_pos(panel_github, 0, 52);
    lv_obj_set_style_bg_opa(panel_github, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(panel_github, 0, 0);
    lv_obj_set_style_pad_all(panel_github, 0, 0);
    lv_obj_clear_flag(panel_github, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(panel_github, LV_OBJ_FLAG_HIDDEN);
#endif

    build_power_card(panel_solar);
    build_yield_card(panel_solar);
    build_chart_card(panel_solar);

    /* GitHub tab intentionally empty (design only) */

    select_tab(0);
    lv_timer_create(clock_timer_cb, 1000, NULL);
}

/* switch tabs programmatically (used by the host simulator; harmless on HW) */
void dashboard_ui_select_tab(int idx)
{
    if (idx == 0 || (GITHUB_TAB_ENABLED && idx == 1)) select_tab(idx);
}

/* data layer hook — the GitHub tab is empty, so keep the value console-side */
void dashboard_ui_set_pr_count(uint32_t count, const char *note)
{
    (void)count;
    (void)note;
}

void dashboard_ui_set_solar(uint32_t watts_now, uint32_t wh_today,
                             uint32_t watts_peak, const char *updated)
{
    char buf[20];
    snprintf(buf, sizeof(buf), "%.2f", watts_now / 1000.0);
    lv_label_set_text(s_power_val_lbl, buf);
    snprintf(buf, sizeof(buf), "%.2f", wh_today / 1000.0);
    lv_label_set_text(s_yield_val_lbl, buf);
    snprintf(buf, sizeof(buf), "Peak %.2f kW", watts_peak / 1000.0);
    lv_label_set_text(s_power_peak_lbl, buf);
    char upd[40];
    snprintf(upd, sizeof(upd), "Last updated: %s", updated ? updated : "--:--");
    lv_label_set_text(s_power_upd_lbl, upd);

    /* lv_obj_align_to positions are one-shot: the value labels resize when
     * their text changes, so re-run the unit alignment every update or the
     * unit drifts into the digits (seen with kWh at "10.00"). */
    lv_obj_align_to(s_power_unit_lbl, s_power_val_lbl,
                    LV_ALIGN_OUT_RIGHT_BOTTOM, 12, -14);
    lv_obj_align_to(s_yield_unit_lbl, s_yield_val_lbl,
                    LV_ALIGN_OUT_RIGHT_BOTTOM, 12, -14);
}
