/**
 * dashboard_ui.cpp — two-tab demo dashboard (solar + web stats)
 *
 * Tab 1 "Solar":     power gauge (arc), 24 h production chart, battery bar,
 *                    energy-today / peak labels.
 * Tab 2 "Web Stats": status cards (open PRs, issues, CI) + weekly commits bar chart.
 *
 * All data is mock, updated by a 2 s timer so the UI visibly refreshes
 * (and the FPS overlay has something to report).
 */
#include "dashboard_ui.h"
#include "ui_tokens.h"

#include <stdio.h>
#include <stdlib.h>

/* ---------- module state ---------- */
static lv_obj_t *gauge;           /* solar power arc         */
static lv_obj_t *gauge_label;     /* W value inside the arc  */
static lv_obj_t *prod_chart;      /* 24h production chart    */
static lv_chart_series_t *prod_series;
static lv_obj_t *batt_bar;        /* battery SoC bar         */
static lv_obj_t *batt_label;
static lv_obj_t *kwh_label;
static lv_obj_t *pr_card_value;   /* web tab: open-PR number  */

static int32_t power_w = 2450;    /* current production (mock walk) */
static int32_t batt_soc = 87;     /* battery % (mock walk)          */

/* ---------- helpers ---------- */
static lv_obj_t *make_card(lv_obj_t *parent, const char *title,
                           const char *value, lv_color_t accent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, LV_PCT(31), 170);
    lv_obj_align_to(card, NULL, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(card, ui::radius_card, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, ui::border(), 0);
    lv_obj_set_style_border_opa(card, ui::opa_border, 0);
    lv_obj_set_style_pad_all(card, ui::space_m, 0);
    lv_obj_set_style_bg_color(card, ui::surface(), 0);
    lv_obj_set_style_bg_opa(card, ui::opa_surf, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *t = lv_label_create(card);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_color(t, ui::text_dim(), 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *v = lv_label_create(card);
    lv_label_set_text(v, value);
    lv_obj_set_style_text_font(v, ui::font(ui::FONT_NUMBER), 0);
    lv_obj_set_style_text_color(v, accent, 0);
    lv_obj_align(v, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    return v; /* value label so caller can update later */
}

static void refresh_timer_cb(lv_timer_t *t)
{
    (void)t;
    /* random walk, clamped */
    power_w += (int32_t)(rand() % 301) - 150;
    if (power_w < 300) power_w = 300;
    if (power_w > 4800) power_w = 4800;

    batt_soc += (rand() % 3) - 1;
    if (batt_soc < 40) batt_soc = 40;
    if (batt_soc > 100) batt_soc = 100;

    lv_arc_set_value(gauge, power_w);

    char buf[32];
    snprintf(buf, sizeof(buf), "%d.%d kW", power_w / 1000, (power_w % 1000) / 100);
    lv_label_set_text(gauge_label, buf);

    lv_bar_set_value(batt_bar, batt_soc, LV_ANIM_ON);
    snprintf(buf, sizeof(buf), "Battery  %d%%", (int)batt_soc);
    lv_label_set_text(batt_label, buf);

    snprintf(buf, sizeof(buf), "Today: %d.%d kWh   Peak: 4.2 kW",
             power_w / 50, ((power_w * 2) % 100) / 10);
    lv_label_set_text(kwh_label, buf);
}

/* ---------- tabs ---------- */
static void build_solar_tab(lv_obj_t *content)
{
    /* power gauge */
    gauge = lv_arc_create(content);
    lv_obj_set_size(gauge, 300, 300);
    lv_obj_align(gauge, LV_ALIGN_TOP_LEFT, 30, 40);
    lv_arc_set_rotation(gauge, 135);
    lv_arc_set_bg_angles(gauge, 0, 270);
    lv_arc_set_range(gauge, 0, 5000);
    lv_arc_set_value(gauge, power_w);
    lv_obj_remove_style(gauge, NULL, LV_PART_KNOB);   /* no center knob */
    lv_obj_clear_flag(gauge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_color(gauge, ui::color_power(), LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(gauge, 22, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(gauge, 22, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(gauge, ui::opa_track, LV_PART_MAIN);

    gauge_label = lv_label_create(gauge);
    lv_obj_set_style_text_font(gauge_label, ui::font(ui::FONT_NUMBER), 0);
    lv_label_set_text(gauge_label, "2.4 kW");
    lv_obj_center(gauge_label);

    /* 24h production chart */
    prod_chart = lv_chart_create(content);
    lv_obj_set_size(prod_chart, 430, 280);
    lv_obj_align(prod_chart, LV_ALIGN_TOP_RIGHT, -20, 30);
    lv_chart_set_type(prod_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(prod_chart, 24);
    lv_chart_set_range(prod_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 5000);
    lv_chart_set_update_mode(prod_chart, LV_CHART_UPDATE_MODE_CIRCULAR);
    lv_obj_set_style_border_width(prod_chart, 0, 0);
    lv_obj_set_style_pad_all(prod_chart, 0, 0);
    prod_series = lv_chart_add_series(prod_chart, ui::color_power(),
                                      LV_CHART_AXIS_PRIMARY_Y);
    /* mock day-curve */
    static const int32_t day[24] = {0,0,0,0,0,80,600,1500,2600,3600,4200,4600,
                                    4700,4500,4100,3500,2600,1600,800,200,0,0,0,0};
    for (int i = 0; i < 24; i++)
        prod_series->y_points[i] = day[i];
    lv_chart_refresh(prod_chart);

    /* battery */
    batt_label = lv_label_create(content);
    lv_label_set_text(batt_label, "Battery  87%");
    lv_obj_set_style_text_font(batt_label, ui::font(ui::FONT_UI), 0);
    lv_obj_align(batt_label, LV_ALIGN_BOTTOM_LEFT, 40, -70);

    batt_bar = lv_bar_create(content);
    lv_obj_set_size(batt_bar, 240, 18);
    lv_obj_align(batt_bar, LV_ALIGN_BOTTOM_LEFT, 40, -40);
    lv_bar_set_range(batt_bar, 0, 100);
    lv_bar_set_value(batt_bar, batt_soc, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(batt_bar, ui::color_battery(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(batt_bar, ui::opa_track, LV_PART_MAIN);

    kwh_label = lv_label_create(content);
    lv_label_set_text(kwh_label, "Today: 21.4 kWh   Peak: 4.2 kW");
    lv_obj_set_style_text_font(kwh_label, ui::font(ui::FONT_UI), 0);
    lv_obj_align(kwh_label, LV_ALIGN_BOTTOM_RIGHT, -20, -55);

    lv_timer_create(refresh_timer_cb, 2000, NULL);
}

static void build_web_tab(lv_obj_t *content)
{
    /* three status cards in a flex row */
    lv_obj_t *row = lv_obj_create(content);
    lv_obj_set_size(row, LV_PCT(96), 190);
    lv_obj_align(row, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    pr_card_value = make_card(row, "Open PRs",  "—", ui::color_pr());
    lv_label_set_text(make_card(row, "Issues",    "4",  ui::color_issues()),
                      "4");
    lv_label_set_text(make_card(row, "CI status", "passing",
                                ui::color_ci()),
                      "passing");

    /* weekly commits */
    lv_obj_t *chart = lv_chart_create(content);
    lv_obj_set_size(chart, LV_PCT(90), 200);
    lv_obj_align(chart, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_chart_set_type(chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(chart, 7);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 20);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_pad_all(chart, 4, 0);
    lv_chart_series_t *commits = lv_chart_add_series(chart, ui::color_commits(),
                                                     LV_CHART_AXIS_PRIMARY_Y);
    static const int32_t week[7] = {6, 12, 3, 9, 14, 1, 0};
    for (int i = 0; i < 7; i++)
        commits->y_points[i] = week[i];
    lv_chart_refresh(chart);
}

void dashboard_ui_set_pr_count(uint32_t count, const char *note)
{
    if (pr_card_value == nullptr) return;
    if (note != nullptr) {
        lv_label_set_text(pr_card_value, note);
    } else {
        char buf[16];
        snprintf(buf, sizeof(buf), "%lu", (unsigned long)count);
        lv_label_set_text(pr_card_value, buf);
    }
}

/* ---------- entry ---------- */
void dashboard_ui_init(void)
{
    lv_disp_t *disp = lv_disp_get_default();
    /* dark theme, fits a dashboard better */
    lv_theme_default_init(disp,
                          ui::accent(),
                          ui::solar_amber(),
                          true /* dark */,
                          ui::font(ui::FONT_BODY));

    lv_obj_set_style_bg_color(lv_scr_act(), ui::background(), LV_PART_MAIN);

    lv_obj_t *tv = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 46);
    lv_obj_set_size(tv, 800, 480);

    build_solar_tab(lv_tabview_add_tab(tv, "Solar"));
    build_web_tab(lv_tabview_add_tab(tv, "Web Stats"));
}
