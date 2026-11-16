/**
 * hostsim/main.cpp — macOS host simulator for the Waveshare dashboard.
 *
 * Renders the SAME dashboard_ui.cpp in an SDL2 window (800x480), then writes
 * raw RGB565 dumps (framebuffer + LVGL snapshot) of each tab for PNG review.
 *
 * Build & run:  ./hostsim/build.sh
 */
#include <lvgl.h>
#include <SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "dashboard_ui.h"

/* Pin the wall clock for screenshot renders: a README screenshot must
 * be internally consistent (now-line inside the current bar, header clock
 * matching the data), regardless of when the sim runs. Interposing
 * libc time() for the whole executable does that; the fixed epoch is
 * 2026-11-15 14:35:00 PST (1794782100). Sim-only; the firmware binary
 * is untouched by this file. extern "C" is required: without it the
 * definition name-mangles and dashboard_ui.o binds to libc's time. */
extern "C" time_t time(time_t *t)
{
    time_t fixed = 1794782100;
    if (t) *t = fixed;
    return fixed;
}

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static const int W = 800, H = 480;

static lv_color_t screen_fb[W * H];
static bool fb_ready = false;

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    static int n = 0;
    char path[64];
    if (n < 20) {
        snprintf(path, sizeof(path), "hostsim/frame_%02d.raw", n);
        FILE *f = fopen(path, "wb");
        int w = 800, h = 480, z = 0;
        fwrite(&z, 1, 4, f); fwrite(&w, 1, 4, f); fwrite(&h, 1, 4, f);
        fwrite(color_p, 1, (size_t)800 * 480 * 2, f);
        fclose(f);
    }
    n++;
    /* keep our own composited copy of the framebuffer */
    lv_coord_t w = area->x2 - area->x1 + 1;
    for (lv_coord_t y = area->y1; y <= area->y2; y++) {
        memcpy(&screen_fb[y * W + area->x1], color_p, w * sizeof(lv_color_t));
        color_p += w;
    }
    SDL_Rect r = {area->x1, area->y1, area->x2 - area->x1 + 1, area->y2 - area->y1 + 1};
    SDL_UpdateTexture(texture, &r, screen_fb + area->y1 * W + area->x1, W * sizeof(lv_color_t));
    lv_disp_flush_ready(drv);
    fb_ready = true;
}

static void dump_fb(const char *path)
{
    FILE *f = fopen(path, "wb");
    int w = W, h = H, zero = 0;
    fwrite(&zero, 1, 4, f);
    fwrite(&w, 1, 4, f);
    fwrite(&h, 1, 4, f);
    fwrite(screen_fb, 1, (size_t)W * H * 2, f);
    fclose(f);
    printf("wrote %s\n", path);
}

static void snap(const char *path)
{
    lv_img_dsc_t *img = lv_snapshot_take(lv_scr_act(), LV_IMG_CF_TRUE_COLOR);
    if (!img) { fprintf(stderr, "snapshot failed\n"); exit(1); }
    FILE *f = fopen(path, "wb");
    fwrite(&img->header, 1, sizeof(img->header), f);   /* 4-byte header */
    int w = W, h = H;
    fwrite(&w, 1, 4, f);
    fwrite(&h, 1, 4, f);
    fwrite(img->data, 1, (size_t)W * H * 2, f);
    fclose(f);
    lv_snapshot_free(img);
    printf("wrote %s\n", path);
}

static void pump_frames(int n)
{
    for (int i = 0; i < n; i++) {
        lv_tick_inc(15);
        lv_timer_handler();
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
    }
}

static const char *cls_name(const lv_obj_class_t *c); /* fwd */

static void dump_tree(const lv_obj_t *o, int depth)
{
    for (int i = 0; i < depth; i++) printf("  ");
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    printf("%-6s abs=(%d,%d)-(%d,%d)\n", cls_name(o->class_p),
           a.x1, a.y1, a.x2, a.y2);
    uint32_t n = lv_obj_get_child_cnt(o);
    for (uint32_t i = 0; i < n; i++) dump_tree(lv_obj_get_child(o, i), depth + 1);
}

static const char *cls_name(const lv_obj_class_t *c)
{
    if (c == &lv_obj_class) return "obj";
    if (c == &lv_label_class) return "label";
    if (c == &lv_btn_class) return "btn";
    if (c == &lv_line_class) return "line";
    if (c == &lv_canvas_class) return "canvas";
    if (c == &lv_arc_class) return "arc";
    if (c == &lv_bar_class) return "bar";
    if (c == &lv_chart_class) return "chart";
    if (c == &lv_img_class) return "image";
    return "?";
}

int main(int argc, char **argv)
{
    lv_init();

    SDL_Init(SDL_INIT_VIDEO);
    SDL_CreateWindowAndRenderer(W, H, 0, &window, &renderer);
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565,
                                SDL_TEXTUREACCESS_STREAMING, W, H);
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf1[W * H];
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, W * H);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = W;
    disp_drv.ver_res = H;
    disp_drv.draw_buf = &draw_buf;
    disp_drv.full_refresh = 1;
    disp_drv.flush_cb = flush_cb;
    lv_disp_drv_register(&disp_drv);

    /* run the UI */
    dashboard_ui_init();

    /* canned mid-afternoon sample so screenshots show a POPULATED
     * dashboard (plausible values, not real): full buckets 6..13,
     * partial current hour 14, totals consistent with the bars */
    {
        float kwh[24];
        for (int i = 0; i < 24; i++) kwh[i] = -1.0f;   /* -1 = missing */
        static const float afternoon[9] = { 0.2f, 0.9f, 1.6f, 2.2f, 2.6f,
                                           2.9f, 2.8f, 1.6f, 0.9f };
        for (int i = 0; i < 9; i++) kwh[6 + i] = afternoon[i];
        dashboard_ui_set_hourly(kwh);
        dashboard_ui_set_solar(2400, 15700, 4900, 138500, "02:35 PM");
    }

    pump_frames(40);

    if (argc > 1) dump_tree(lv_scr_act(), 0);

    dump_fb("hostsim/out_tab1.raw");
    snap("hostsim/snap_tab1.raw");

    dashboard_ui_select_tab(1);
    pump_frames(10);
    dump_fb("hostsim/out_tab2.raw");

    SDL_Quit();
    printf("done\n");
    return 0;
}
