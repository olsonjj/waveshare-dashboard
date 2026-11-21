/**
 * dashboard_ui.cpp — v2 "Solar Ops" dashboard, built to the design mockup.
 *
 * Screen 800x480, dark theme, two functional tabs:
 *   Tab 1 "SOLAR TELEMETRY" — current power card + donut, total yield card
 *                             with target bar, 96-bucket generation bar
 *                             chart (15-minute kWh; live data via
 *                             dashboard_ui_set_chart).
 *   Tab 2 "GITHUB OPS"      — intentionally empty for now (design only).
 *
 * Static mock values only fill the cards until the first data poll;
 * a 1 s timer ticks the header clock from the system clock (TZ set in
 * main.cpp). Hand-rolled tab header for full badge/pill styling.
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

/* ------------------------------------------------------------------ */
/* module state & chart geometry                                       */
/* ------------------------------------------------------------------ */
static lv_obj_t *tab_btn[2];
static lv_obj_t *tab_underline[2];
static lv_obj_t *s_power_val_lbl = nullptr;   /* CURRENT POWER card value (kW) */
static lv_obj_t *s_power_peak_lbl = nullptr;  /* CURRENT POWER card peak row (kW) */
static lv_obj_t *s_power_unit_lbl = nullptr;  /* kW unit; re-aligned each update */
static lv_obj_t *s_yield_val_lbl = nullptr;   /* TOTAL YIELD card value (kWh) */
static lv_obj_t *s_yield_7d_lbl = nullptr;    /* 7 Day Yield bottom stat (kWh) */
static lv_obj_t *s_yield_unit_lbl = nullptr;  /* kWh unit; re-aligned each update */
static lv_obj_t *s_power_upd_lbl = nullptr;  /* last-updated pill label */
static lv_obj_t *s_link_pill = nullptr;      /* header NETWORK LOST pill */

/* ---- coalesced update buffers ---------------------------------------
 * Full-refresh RGB mode: EVERY invalidation costs a full 800x480 PSRAM
 * re-render + scanout wait inside lv_timer_handler (see flush_cb). The
 * push stream (1 Hz), the clock (1 Hz), the now-line (1 Hz) and the perf
 * monitor (≈3 Hz) would otherwise dirty the screen several times per
 * second — each change paying the full-screen price. All public setters
 * therefore only buffer; a single 1 Hz timer applies everything pending
 * (clock, now-line, solar, chart, link pill) in one invalidated frame. */
static struct {
    bool pending;
    uint32_t watts_now, wh_today, watts_peak, wh_7day;
    char updated[16];                 /* caller-built pill text, copied */
} s_pend_solar;
static bool s_pend_chart_pending;
static float s_pend_chart[96];        /* -1.0f = no bar */
static struct { bool pending; bool up; } s_pend_link;

static void apply_chart(const float kwh[96]);   /* defined below; used at build */

/* GitHub tab is design-only for now — hidden until it gets real data. */
#define GITHUB_TAB_ENABLED 0
static lv_obj_t *tab_title[2];
static lv_obj_t *panel_solar, *panel_github;
static lv_obj_t *clock_label;
static int sel_tab = 0;

/* generation-chart card: plot is placed at (14,38) inside the card */
static constexpr int CH_W = 748, CH_H = 214;
static constexpr int PAD_L = 40, PAD_R = 12, PAD_T = 8, PAD_B = 24;
static constexpr int PLOT_W = CH_W - PAD_L - PAD_R;   /* 696 */
static constexpr int PLOT_H = CH_H - PAD_T - PAD_B;   /* 182 */

/* 15-minute bar chart: one bar per quarter hour, index 0 = 12:00 AM
 * (index = hour*4 + min/15; 4 bars per hour). The push server sends all
 * 96 buckets every second — see spec.md / dashboard_ui_set_chart. */
static constexpr int N_BUCKETS  = 96;
static constexpr int BAR_SLOT   = PLOT_W / N_BUCKETS;  /* 7 px per bucket */
static constexpr int BAR_W      = 5;                   /* bar width, px  */
static constexpr int N_YLBL     = 5;                   /* 0..ymax, 4 intervals */

static lv_obj_t *s_bucket_bar[N_BUCKETS];   /* bottom-anchored bar objects */
static lv_obj_t *s_y_lbl[N_YLBL];           /* y-axis labels (text only)  */
static lv_obj_t *s_now_line;               /* current-time marker (2 px) */

/* Position the current-time line so it tracks the current 15-minute
 * bucket's amber bar, using the same geometry as build_chart_card().
 * The marker is 2 px and left-anchored; it sweeps flush inside the 5-px
 * bar — its left edge on the bar's left edge at m = 0, its right edge on
 * the bar's right edge at m = 14. Called from a 1-s lv_timer and once
 * at chart build. */
static void update_now_line(void)
{
    if (!s_now_line) return;
    time_t now = time(nullptr);
    struct tm tmbuf;
    localtime_r(&now, &tmbuf);
    int tod = tmbuf.tm_hour * 60 + tmbuf.tm_min;   /* minutes since midnight */
    int bucket = tod / 15;                          /* bucket index 0..95 */
    int m = tod % 15;                               /* minutes into block */
    int bar_left = PAD_L + bucket * BAR_SLOT + (BAR_SLOT - BAR_W) / 2;
    int x = bar_left + m * (BAR_W - 2) / 14;        /* flush inside the bar */
    lv_obj_set_pos(s_now_line, x, PAD_T);
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

    /* --- network-lost alert pill: shown whenever the push link is down,
     * left of the (decorative) signal bars; hidden once LINKED --- */
    s_link_pill = make_pill(header, "NETWORK LOST", color_alert(), opa_tint_14,
                            true, danger_tint(), FONT_BODY);
    lv_obj_align(s_link_pill, LV_ALIGN_RIGHT_MID, -170, 0);  /* right edge x≈630; bars start 635 */
    lv_obj_add_flag(s_link_pill, LV_OBJ_FLAG_HIDDEN);

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

    /* bottom stat, mirroring the power card's Peak row */
    lv_obj_t *tri = make_tri(card, green(), 10, 8);
    lv_obj_align(tri, LV_ALIGN_BOTTOM_LEFT, 20, -12);
    lv_obj_t *seven = make_label(card, "7 Day Yield --.-- kWh", FONT_BODY, green(), 0, 0);
    s_yield_7d_lbl = seven;
    lv_obj_align_to(seven, tri, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
}

/* Default chart grid before the first data arrives (also what set_chart
 * falls back to when every bucket is missing): 0..2 kWh in 0.5 steps. */
static float chart_ymax(float peak_kwh)
{
    /* Round the peak bucket UP to the next multiple of 2.0 kWh so the
     * 4-interval label grid (0..ymax) always lands on clean 0.5 steps
     * (0/0.5/1.0/1.5/2.0 for ymax=2, 0/1.5/3.0/4.5/6.0 for ymax=6, ...). */
    float ymax = 2.0f * ceilf(peak_kwh / 2.0f);
    return ymax < 2.0f ? 2.0f : ymax;
}

static void build_chart_card(lv_obj_t *parent)
{
    lv_obj_t *card = make_panel(parent, 776, 266);
    lv_obj_set_pos(card, 12, 154);

    make_label(card, "SOLAR GENERATION - TODAY", FONT_TITLE, text(), 18, 10);

    /* plot container: children use chart-local coordinates */
    lv_obj_t *plot = lv_obj_create(card);
    lv_obj_set_size(plot, CH_W, CH_H);
    lv_obj_set_pos(plot, 14, 38);
    lv_obj_set_style_pad_all(plot, 0, 0);
    lv_obj_set_style_bg_opa(plot, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(plot, 0, 0);
    lv_obj_set_style_radius(plot, 0, 0);
    lv_obj_clear_flag(plot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* horizontal gridlines at the 4 label intervals (bottom is the baseline) */
    for (int k = 1; k < N_YLBL; k++) {
        make_dash(plot, grid(), 1, PAD_L,
                  PAD_T + PLOT_H - k * PLOT_H / 4, PLOT_W, 0, 3, 5);
    }
    lv_obj_t *base = lv_obj_create(plot);
    lv_obj_set_size(base, PLOT_W, 1);
    lv_obj_set_pos(base, PAD_L, PAD_T + PLOT_H);
    lv_obj_set_style_pad_all(base, 0, 0);
    lv_obj_set_style_bg_color(base, border(), 0);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(base, 0, 0);
    lv_obj_set_style_border_width(base, 0, 0);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* current-time marker: thin full-height line, positioned by the
     * 60-s now-timer; green reads as "live" against amber bars */
    s_now_line = lv_obj_create(plot);
    lv_obj_set_size(s_now_line, 2, PLOT_H);
    lv_obj_set_pos(s_now_line, PAD_L, PAD_T);
    lv_obj_set_style_pad_all(s_now_line, 0, 0);
    lv_obj_set_style_bg_color(s_now_line, green(), 0);
    lv_obj_set_style_bg_opa(s_now_line, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_now_line, 0, 0);
    lv_obj_set_style_border_width(s_now_line, 0, 0);
    lv_obj_clear_flag(s_now_line, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    update_now_line();

    /* one bar per 15-min bucket, bottom-anchored; heights set by set_chart */
    for (int i = 0; i < N_BUCKETS; i++) {
        lv_obj_t *b = lv_obj_create(plot);
        lv_obj_set_size(b, BAR_W, 0);
        lv_obj_set_pos(b, PAD_L + i * BAR_SLOT + (BAR_SLOT - BAR_W) / 2,
                       PAD_T + PLOT_H);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_set_style_radius(b, 3, 0);
        lv_obj_set_style_bg_color(b, yellow(), 0);            /* bright top  */
        lv_obj_set_style_bg_grad_color(b, color_power(), 0);  /* amber bottom */
        lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(b, 0, 0);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);   /* no data yet */
        s_bucket_bar[i] = b;
    }

    /* y-axis labels (card children, left gutter, right-aligned) */
    for (int k = 0; k < N_YLBL; k++) {
        lv_obj_t *l = lv_label_create(card);
        lv_obj_set_style_text_font(l, font(FONT_BODY), 0);
        lv_obj_set_style_text_color(l, text_faint(), 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_size(l, 30, LV_SIZE_CONTENT);
        lv_obj_set_pos(l, 14 + PAD_L - 36,
                       38 + PAD_T + PLOT_H - k * PLOT_H / 4 - 7);
        s_y_lbl[k] = l;
    }

    /* x-axis labels: one every 3 hours, centered on that hour's first
     * bucket — 3 hours = 12 quarter-bucket slots (the old hourly chart
     * used 3 slots; the multiplier was the 96-bucket conversion miss) */
    static const char *const XTXT[8] =
        {"12a", "3a", "6a", "9a", "12p", "3p", "6p", "9p"};
    for (int k = 0; k < 8; k++) {
        lv_point_t sz;
        lv_txt_get_size(&sz, XTXT[k], font(FONT_SMALL),
                        0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        int cx = 14 + PAD_L + k * 12 * BAR_SLOT + BAR_SLOT / 2;
        make_label(card, XTXT[k], FONT_SMALL, text_faint(),
                   cx - sz.x / 2, 38 + PAD_T + PLOT_H + 7);
    }

    /* pre-data state: empty grid with the default 0..2 kWh labels */
    apply_chart(nullptr);
}

/* Internal: applies chart state to the live objects. Called by the 1 Hz
 * coalesce timer (and once at build for the empty-axis default). */
static void apply_chart(const float kwh[N_BUCKETS])
{
    if (!s_bucket_bar[0] || !s_y_lbl[0]) return;   /* init not run yet */

    float peak = 0.0f;
    if (kwh) {
        for (int i = 0; i < N_BUCKETS; i++) {
            if (kwh[i] > peak) peak = kwh[i];
        }
    }
    float ymax = chart_ymax(peak);

    for (int i = 0; i < N_BUCKETS; i++) {
        lv_obj_t *b = s_bucket_bar[i];
        if (!kwh || kwh[i] < 0.0f) {            /* missing bucket: no bar */
            lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        int h = (int)(kwh[i] / ymax * (float)PLOT_H + 0.5f);
        if (h < 2) h = 2;                        /* visible stub, even at 0.0 */
        if (h > PLOT_H) h = PLOT_H;
        lv_obj_set_height(b, h);
        lv_obj_set_y(b, PAD_T + PLOT_H - h);
        lv_obj_clear_flag(b, LV_OBJ_FLAG_HIDDEN);
    }

    for (int k = 0; k < N_YLBL; k++) {
        float v = ymax * (float)k / 4.0f;
        char yl[8];
        if (v == floorf(v)) snprintf(yl, sizeof(yl), "%d", (int)v);
        else                snprintf(yl, sizeof(yl), "%.1f", v);
        lv_label_set_text(s_y_lbl[k], yl);
    }
}

/* Public setter: buffers only — applied by the 1 Hz coalesce timer, so a
 * push line costs at most one invalidated frame (shared with the clock,
 * now-line and solar updates). Call under the LVGL adapter lock. */
void dashboard_ui_set_chart(const float kwh[N_BUCKETS])
{
    for (int i = 0; i < N_BUCKETS; i++)
        s_pend_chart[i] = (kwh && kwh[i] >= 0.0f) ? kwh[i] : -1.0f;
    s_pend_chart_pending = true;
}

/* ------------------------------------------------------------------ */
/* clock + entry points                                                */
/* ------------------------------------------------------------------ */
/* Applies solar values to the live labels. Called by the 1 Hz coalesce
 * timer only. */
static void apply_solar(uint32_t watts_now, uint32_t wh_today,
                        uint32_t watts_peak, uint32_t wh_7day,
                        const char *updated)
{
    /* 32 covers the longest possible string over the full uint32 range:
     * "7 Day Yield 4294967.29 kWh" (27) — exact-fit buffers clipped the
     * unit before (buf[20] with "7 Day Yield 142.73 kWh"), so this is a
     * class-kill, not another exact fit */
    char buf[32];
    snprintf(buf, sizeof(buf), "%.2f", watts_now / 1000.0);
    lv_label_set_text(s_power_val_lbl, buf);
    snprintf(buf, sizeof(buf), "%.2f", wh_today / 1000.0);
    lv_label_set_text(s_yield_val_lbl, buf);
    snprintf(buf, sizeof(buf), "Peak %.2f kW", watts_peak / 1000.0);
    lv_label_set_text(s_power_peak_lbl, buf);
    snprintf(buf, sizeof(buf), "7 Day Yield %.2f kWh", wh_7day / 1000.0);
    lv_label_set_text(s_yield_7d_lbl, buf);
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

/* Single 1 Hz apply tick: clock, chart now-line and everything the data
 * layer buffered since the last tick — one dirty region set, one frame. */
static void coalesce_timer_cb(lv_timer_t *)
{
    time_t now = time(nullptr);
    struct tm tmbuf;
    localtime_r(&now, &tmbuf);
    char buf[16];
    strftime(buf, sizeof(buf), "%H:%M:%S", &tmbuf);
    lv_label_set_text(clock_label, buf);

    update_now_line();

    if (s_pend_solar.pending) {
        apply_solar(s_pend_solar.watts_now, s_pend_solar.wh_today,
                    s_pend_solar.watts_peak, s_pend_solar.wh_7day,
                    s_pend_solar.updated);
        s_pend_solar.pending = false;
    }
    if (s_pend_chart_pending) {
        apply_chart(s_pend_chart);
        s_pend_chart_pending = false;
    }
    if (s_pend_link.pending) {
        if (s_link_pill) {
            if (s_pend_link.up)
                lv_obj_add_flag(s_link_pill, LV_OBJ_FLAG_HIDDEN);
            else
                lv_obj_clear_flag(s_link_pill, LV_OBJ_FLAG_HIDDEN);
        }
        s_pend_link.pending = false;
    }
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
    lv_timer_create(coalesce_timer_cb, 1000, NULL);
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

/* link-state hook from push_client: up=true hides the pill (stream live),
 * up=false shows it. Buffers only — applied by the 1 Hz coalesce timer,
 * so a transition costs no extra frame. Must be called with the LVGL
 * adapter lock held (same contract as dashboard_ui_set_solar). */
void dashboard_ui_set_link(bool up)
{
    s_pend_link.pending = true;
    s_pend_link.up = up;
}

/* Public setter: buffers only — applied by the 1 Hz coalesce timer, so
 * a push line invalidates the screen at most once per second total.
 * Call under the LVGL adapter lock (see dashboard_ui.h). */
void dashboard_ui_set_solar(uint32_t watts_now, uint32_t wh_today,
                             uint32_t watts_peak, uint32_t wh_7day,
                             const char *updated)
{
    s_pend_solar.watts_now  = watts_now;
    s_pend_solar.wh_today   = wh_today;
    s_pend_solar.watts_peak = watts_peak;
    s_pend_solar.wh_7day    = wh_7day;
    snprintf(s_pend_solar.updated, sizeof(s_pend_solar.updated), "%s",
             updated ? updated : "--:--");
    s_pend_solar.pending = true;
}
