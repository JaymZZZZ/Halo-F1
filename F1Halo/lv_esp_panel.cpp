#include "lv_esp_panel.h"

#include "board_config.h"

#include <Arduino.h>
#include <cstring>
#include <esp_heap_caps.h>
#include <esp_display_panel.hpp>

#if LV_COLOR_DEPTH != 16
#error "This project currently requires LV_COLOR_DEPTH == 16."
#endif

#ifndef HALO_PANEL_DIAG_LOG
#define HALO_PANEL_DIAG_LOG 0
#endif

#ifndef HALO_PANEL_DIAG_INTERVAL_MS
#define HALO_PANEL_DIAG_INTERVAL_MS (15000UL)
#endif

using namespace esp_panel::board;
using namespace esp_panel::drivers;

typedef struct {
    Board *board;
    LCD *lcd;
    Touch *touch;
    Backlight *backlight;
    void *draw_buf;
    uint16_t *rotate_buf;
    uint32_t rotate_buf_pixels;
    uint32_t diag_last_ms;
    uint32_t diag_flush_calls;
    uint32_t diag_frame_starts;
    uint32_t diag_frame_switches;
    uint32_t diag_switch_timeouts;
    uint32_t diag_rotate_skips;
    uint32_t diag_draw_fails;
    uint32_t diag_oob_writes;
    uint32_t diag_max_clip_w;
    uint32_t diag_max_clip_h;
    uint32_t diag_max_wait_ms;
    uint32_t diag_boot_free_heap;
    uint32_t diag_boot_free_psram;
    uint32_t diag_boot_free_internal;
    int32_t physical_width;
    int32_t physical_height;
    bool portrait_mode;
    bool fb_swap_enabled;
    bool fb_frame_open;
    uint32_t refresh_gen;
    uint16_t *fb_front;
    uint16_t *fb_back;
    uint32_t fb_pixels;
} halo_panel_ctx_t;

static halo_panel_ctx_t *s_ctx = nullptr;

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
static bool init_board(halo_panel_ctx_t *ctx);
static void maybe_log_diag(halo_panel_ctx_t *ctx);
static bool IRAM_ATTR on_refresh_finish(void *user_data);

static inline int32_t clamp_i32(int32_t value, int32_t min_v, int32_t max_v)
{
    if (value < min_v) return min_v;
    if (value > max_v) return max_v;
    return value;
}

static bool init_board(halo_panel_ctx_t *ctx)
{
    ctx->board = new Board();
    if (ctx->board == nullptr) return false;
    if (!ctx->board->init()) return false;

    ctx->lcd = ctx->board->getLCD();
    if (ctx->lcd == nullptr) return false;

    auto *lcd_bus = ctx->lcd->getBus();
    if ((lcd_bus != nullptr) && (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB)) {
        ctx->lcd->configFrameBufferNumber(HALO_RGB_FRAME_BUFFERS);
#if CONFIG_IDF_TARGET_ESP32S3
        auto *rgb_bus = static_cast<BusRGB *>(lcd_bus);
        rgb_bus->configRGB_BounceBufferSize(ctx->lcd->getFrameWidth() * HALO_RGB_BOUNCE_LINES);
        rgb_bus->configRGB_FreqHz(HALO_RGB_PCLK_HZ);
#endif
    }

    if (!ctx->board->begin()) return false;

    ctx->touch = ctx->board->getTouch();
    ctx->backlight = ctx->board->getBacklight();

    // Keep physical orientation; portrait UI is handled in software flush.
    ctx->lcd->swapXY(false);
    ctx->lcd->mirrorX(false);
    ctx->lcd->mirrorY(false);
    if (ctx->touch != nullptr) {
        ctx->touch->swapXY(false);
        ctx->touch->mirrorX(false);
        ctx->touch->mirrorY(false);
    }

    ctx->physical_width = ctx->lcd->getFrameWidth();
    ctx->physical_height = ctx->lcd->getFrameHeight();
    ctx->portrait_mode = true;

    // Clear RGB framebuffers once at boot so partial LVGL flushes never expose
    // panel power-on garbage/test patterns.
    int fb_cleared = 0;
    const uint32_t fb_pixels = (uint32_t)ctx->physical_width * (uint32_t)ctx->physical_height;
    if ((lcd_bus != nullptr) && (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) && (fb_pixels > 0)) {
        void *last_fb = nullptr;
        for (int i = 0; i < HALO_RGB_FRAME_BUFFERS; i++) {
            void *fb = ctx->lcd->getFrameBufferByIndex((uint8_t)i);
            if ((fb == nullptr) || (fb == last_fb)) {
                break;
            }
            memset(fb, 0x00, fb_pixels * sizeof(uint16_t));
            last_fb = fb;
            fb_cleared++;
        }
        if (last_fb != nullptr) {
            (void)ctx->lcd->switchFrameBufferTo(last_fb);
        }
    }

    if (ctx->backlight != nullptr) {
        ctx->backlight->setBrightness(100);
    }

    ctx->diag_last_ms = millis();
    ctx->diag_flush_calls = 0;
    ctx->diag_frame_starts = 0;
    ctx->diag_frame_switches = 0;
    ctx->diag_switch_timeouts = 0;
    ctx->diag_rotate_skips = 0;
    ctx->diag_draw_fails = 0;
    ctx->diag_oob_writes = 0;
    ctx->diag_max_clip_w = 0;
    ctx->diag_max_clip_h = 0;
    ctx->diag_max_wait_ms = 0;
    ctx->diag_boot_free_heap = ESP.getFreeHeap();
    ctx->diag_boot_free_psram = ESP.getFreePsram();
    ctx->diag_boot_free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    ctx->fb_swap_enabled = false;
    ctx->fb_frame_open = false;
    ctx->refresh_gen = 0;
    ctx->fb_front = nullptr;
    ctx->fb_back = nullptr;
    ctx->fb_pixels = 0;

    if ((lcd_bus != nullptr) && (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) && HALO_RGB_USE_FB_SWAP) {
        auto *fb0 = (uint16_t *)ctx->lcd->getFrameBufferByIndex(0);
        auto *fb1 = (uint16_t *)ctx->lcd->getFrameBufferByIndex(1);
        if ((fb0 != nullptr) && (fb1 != nullptr) && (fb0 != fb1)) {
            ctx->fb_front = fb0;
            ctx->fb_back = fb1;
            ctx->fb_pixels = (uint32_t)ctx->physical_width * (uint32_t)ctx->physical_height;
            ctx->fb_swap_enabled = true;

            // Keep both framebuffers in sync before first swap.
            memcpy(ctx->fb_back, ctx->fb_front, ctx->fb_pixels * sizeof(uint16_t));
            (void)ctx->lcd->attachRefreshFinishCallback(on_refresh_finish, ctx);
        }
    }

#if HALO_PANEL_DIAG_LOG
    Serial.printf(
        "[Panel] cfg: fb_num=%d bounce_lines=%d pclk=%luHz swap=%d fb_swap=%d phys=%ldx%ld clear=%d\n",
        HALO_RGB_FRAME_BUFFERS,
        HALO_RGB_BOUNCE_LINES,
        (unsigned long)HALO_RGB_PCLK_HZ,
        0,
        ctx->fb_swap_enabled ? 1 : 0,
        (long)ctx->physical_width,
        (long)ctx->physical_height,
        fb_cleared
    );
    Serial.printf("[Panel] build: %s %s\n", __DATE__, __TIME__);
#else
    LV_UNUSED(fb_cleared);
#endif

    return true;
}

static bool IRAM_ATTR on_refresh_finish(void *user_data)
{
    auto *ctx = static_cast<halo_panel_ctx_t *>(user_data);
    if (ctx != nullptr) {
        ctx->refresh_gen++;
    }
    return false;
}

static void maybe_log_diag(halo_panel_ctx_t *ctx)
{
#if HALO_PANEL_DIAG_LOG
    if (ctx == nullptr) return;

    const uint32_t now = millis();
    if ((uint32_t)(now - ctx->diag_last_ms) < HALO_PANEL_DIAG_INTERVAL_MS) return;
    ctx->diag_last_ms = now;

    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);

    const uint32_t free_heap = ESP.getFreeHeap();
    const uint32_t min_heap = ESP.getMinFreeHeap();
    const uint32_t free_psram = ESP.getFreePsram();
    const uint32_t min_psram = ESP.getMinFreePsram();
    const uint32_t free_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    const uint32_t min_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);

    Serial.printf(
        "[Diag] t=%lu flush=%lu starts=%lu swaps=%lu to=%lu oob=%lu clip_max=%lux%lu wait_max=%lums "
        "skip=%lu draw_fail=%lu "
        "heap=%u(min=%u,delta=%ld) int=%u(min=%u,delta=%ld) psram=%u(min=%u,delta=%ld) "
        "lv_used=%u%% lv_frag=%u%%\n",
        (unsigned long)now,
        (unsigned long)ctx->diag_flush_calls,
        (unsigned long)ctx->diag_frame_starts,
        (unsigned long)ctx->diag_frame_switches,
        (unsigned long)ctx->diag_switch_timeouts,
        (unsigned long)ctx->diag_oob_writes,
        (unsigned long)ctx->diag_max_clip_w,
        (unsigned long)ctx->diag_max_clip_h,
        (unsigned long)ctx->diag_max_wait_ms,
        (unsigned long)ctx->diag_rotate_skips,
        (unsigned long)ctx->diag_draw_fails,
        free_heap,
        min_heap,
        (long)free_heap - (long)ctx->diag_boot_free_heap,
        free_internal,
        min_internal,
        (long)free_internal - (long)ctx->diag_boot_free_internal,
        free_psram,
        min_psram,
        (long)free_psram - (long)ctx->diag_boot_free_psram,
        (unsigned int)mon.used_pct,
        (unsigned int)mon.frag_pct
    );
#else
    LV_UNUSED(ctx);
#endif
}

lv_display_t *halo_panel_display_create(void)
{
    if (s_ctx != nullptr) {
        return (lv_display_t *)lv_display_get_default();
    }

    halo_panel_ctx_t *ctx = (halo_panel_ctx_t *)lv_malloc_zeroed(sizeof(halo_panel_ctx_t));
    LV_ASSERT_MALLOC(ctx);
    if (ctx == nullptr) return nullptr;

    if (!init_board(ctx)) {
        lv_free(ctx);
        return nullptr;
    }

    const int32_t lv_w = SCREEN_WIDTH;
    const int32_t lv_h = SCREEN_HEIGHT;
    const uint32_t draw_buf_size = ((uint32_t)lv_w * (uint32_t)lv_h / HALO_LCD_DRAW_BUF_DIV) * sizeof(lv_color_t);

    ctx->draw_buf = heap_caps_malloc(draw_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx->draw_buf == nullptr) {
        ctx->draw_buf = heap_caps_malloc(draw_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    LV_ASSERT_MALLOC(ctx->draw_buf);
    if (ctx->draw_buf == nullptr) {
        lv_free(ctx);
        return nullptr;
    }

    // Keep a full logical frame for rotation workspace.
    // LVGL can emit clipped areas larger than the draw buffer chunk, and a smaller rotate
    // buffer causes skipped flushes (white/garbled screen regions on RGB panels).
    uint32_t rotate_buf_size = (uint32_t)SCREEN_WIDTH * (uint32_t)SCREEN_HEIGHT * sizeof(uint16_t);

    ctx->rotate_buf = (uint16_t *)heap_caps_malloc(rotate_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx->rotate_buf == nullptr) {
        ctx->rotate_buf = (uint16_t *)heap_caps_malloc(rotate_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    LV_ASSERT_MALLOC(ctx->rotate_buf);
    if (ctx->rotate_buf == nullptr) {
        lv_free(ctx->draw_buf);
        lv_free(ctx);
        return nullptr;
    }
    ctx->rotate_buf_pixels = rotate_buf_size / sizeof(uint16_t);

    lv_display_t *disp = lv_display_create(lv_w, lv_h);
    if (disp == nullptr) {
        lv_free(ctx->rotate_buf);
        lv_free(ctx->draw_buf);
        lv_free(ctx);
        return nullptr;
    }

    lv_display_set_driver_data(disp, (void *)ctx);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, ctx->draw_buf, nullptr, draw_buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_ctx = ctx;
    return disp;
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    halo_panel_ctx_t *ctx = (halo_panel_ctx_t *)lv_display_get_driver_data(disp);
    if ((ctx == nullptr) || (ctx->lcd == nullptr) || (px_map == nullptr)) {
        lv_display_flush_ready(disp);
        return;
    }

    const int32_t log_w = lv_display_get_horizontal_resolution(disp);
    const int32_t log_h = lv_display_get_vertical_resolution(disp);

    int32_t src_x1 = area->x1;
    int32_t src_y1 = area->y1;
    int32_t src_x2 = area->x2;
    int32_t src_y2 = area->y2;
    if (src_x2 < 0 || src_y2 < 0 || src_x1 >= log_w || src_y1 >= log_h) {
        lv_display_flush_ready(disp);
        return;
    }

    const int32_t clip_x1 = LV_MAX(src_x1, 0);
    const int32_t clip_y1 = LV_MAX(src_y1, 0);
    const int32_t clip_x2 = LV_MIN(src_x2, log_w - 1);
    const int32_t clip_y2 = LV_MIN(src_y2, log_h - 1);
    const int32_t clip_w = clip_x2 - clip_x1 + 1;
    const int32_t clip_h = clip_y2 - clip_y1 + 1;
    const int32_t src_full_w = src_x2 - src_x1 + 1;
    if (clip_w <= 0 || clip_h <= 0) {
        lv_display_flush_ready(disp);
        return;
    }

    ctx->diag_flush_calls++;
    if ((uint32_t)clip_w > ctx->diag_max_clip_w) ctx->diag_max_clip_w = (uint32_t)clip_w;
    if ((uint32_t)clip_h > ctx->diag_max_clip_h) ctx->diag_max_clip_h = (uint32_t)clip_h;

    uint16_t *src = (uint16_t *)px_map;
    src += (clip_y1 - src_y1) * src_full_w;
    src += (clip_x1 - src_x1);

    if (ctx->portrait_mode) {
        const uint32_t needed = (uint32_t)(clip_w * clip_h);
        if ((ctx->rotate_buf == nullptr) || (ctx->rotate_buf_pixels < needed)) {
            ctx->diag_rotate_skips++;
            lv_display_flush_ready(disp);
            return;
        }

        const int32_t dst_w = clip_h;
        for (int32_t y = 0; y < clip_h; y++) {
            for (int32_t x = 0; x < clip_w; x++) {
                const uint16_t px = src[y * src_full_w + x];
                if (HALO_UI_ROTATION_CW_90) {
                    const int32_t dx = clip_h - 1 - y;
                    const int32_t dy = x;
                    ctx->rotate_buf[dy * dst_w + dx] = px;
                } else {
                    const int32_t dx = y;
                    const int32_t dy = clip_w - 1 - x;
                    ctx->rotate_buf[dy * dst_w + dx] = px;
                }
            }
        }

        const int32_t phys_x = HALO_UI_ROTATION_CW_90 ? (ctx->physical_width - clip_y2 - 1) : clip_y1;
        const int32_t phys_y = HALO_UI_ROTATION_CW_90 ? clip_x1 : (ctx->physical_height - clip_x2 - 1);
        const int32_t phys_w = clip_h;
        const int32_t phys_h = clip_w;

        const int32_t safe_x1 = LV_MAX(phys_x, 0);
        const int32_t safe_y1 = LV_MAX(phys_y, 0);
        const int32_t safe_x2 = LV_MIN(phys_x + phys_w, ctx->physical_width);
        const int32_t safe_y2 = LV_MIN(phys_y + phys_h, ctx->physical_height);

        const int32_t safe_w = safe_x2 - safe_x1;
        const int32_t safe_h = safe_y2 - safe_y1;
        if (safe_w <= 0 || safe_h <= 0) {
            ctx->diag_oob_writes++;
            lv_display_flush_ready(disp);
            return;
        }

        const int32_t src_clip_x = safe_x1 - phys_x;
        const int32_t src_clip_y = safe_y1 - phys_y;
        if ((safe_w != phys_w) || (safe_h != phys_h) || (src_clip_x != 0) || (src_clip_y != 0)) {
            ctx->diag_oob_writes++;
        }

        if (ctx->fb_swap_enabled && (ctx->fb_front != nullptr) && (ctx->fb_back != nullptr) && (ctx->fb_pixels > 0)) {
            if (!ctx->fb_frame_open) {
                // Start a new composition frame from the currently shown framebuffer.
                memcpy(ctx->fb_back, ctx->fb_front, ctx->fb_pixels * sizeof(uint16_t));
                ctx->fb_frame_open = true;
                ctx->diag_frame_starts++;
            }

            // Write rotated dirty rect into the back buffer with framebuffer stride.
            uint16_t *dst = ctx->fb_back + (uint32_t)safe_y1 * (uint32_t)ctx->physical_width + (uint32_t)safe_x1;
            for (int32_t row = 0; row < safe_h; row++) {
                const uint16_t *src_row =
                    ctx->rotate_buf + (uint32_t)(src_clip_y + row) * (uint32_t)phys_w + (uint32_t)src_clip_x;
                memcpy(dst + (uint32_t)row * (uint32_t)ctx->physical_width, src_row, (size_t)safe_w * sizeof(uint16_t));
            }

            if (lv_display_flush_is_last(disp)) {
                if (ctx->lcd->switchFrameBufferTo(ctx->fb_back)) {
                    ctx->diag_frame_switches++;
                    // Non-blocking swap: never stall the LVGL loop waiting for VSYNC.
                    uint16_t *tmp = ctx->fb_front;
                    ctx->fb_front = ctx->fb_back;
                    ctx->fb_back = tmp;
                } else {
                    ctx->diag_draw_fails++;
                }
                ctx->fb_frame_open = false;
            }
        } else {
            bool ok = true;
            if ((safe_w == phys_w) && (safe_h == phys_h) && (src_clip_x == 0) && (src_clip_y == 0)) {
                ok = ctx->lcd->drawBitmap(safe_x1, safe_y1, safe_w, safe_h, (const uint8_t *)ctx->rotate_buf, 0);
            } else {
                for (int32_t row = 0; row < safe_h; row++) {
                    const uint16_t *src_row =
                        ctx->rotate_buf + (uint32_t)(src_clip_y + row) * (uint32_t)phys_w + (uint32_t)src_clip_x;
                    if (!ctx->lcd->drawBitmap(safe_x1, safe_y1 + row, safe_w, 1, (const uint8_t *)src_row, 0)) {
                        ok = false;
                        break;
                    }
                }
            }
            if (!ok) {
                ctx->diag_draw_fails++;
            }
        }
    } else {
        if (!ctx->lcd->drawBitmap(clip_x1, clip_y1, clip_w, clip_h, (const uint8_t *)src, 0)) {
            ctx->diag_draw_fails++;
        }
    }

    lv_display_flush_ready(disp);
}

void halo_panel_touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);

    if ((s_ctx == nullptr) || (s_ctx->touch == nullptr)) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    TouchPoint point_buf[1];
    int count = s_ctx->touch->readPoints(point_buf, 1, 0);
    if (count > 0) {
        int32_t phys_x = point_buf[0].x;
        int32_t phys_y = point_buf[0].y;
        int32_t x = 0;
        int32_t y = 0;

        if (HALO_UI_ROTATION_CW_90) {
            x = phys_y;
            y = s_ctx->physical_width - 1 - phys_x;
        } else {
            x = s_ctx->physical_height - 1 - phys_y;
            y = phys_x;
        }

        x = clamp_i32(x, 0, SCREEN_WIDTH - 1);
        y = clamp_i32(y, 0, SCREEN_HEIGHT - 1);

        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

void halo_panel_set_brightness(uint8_t brightness_255)
{
    if ((s_ctx == nullptr) || (s_ctx->backlight == nullptr)) return;

    int percent = ((int)brightness_255 * 100 + 127) / 255;
    if ((brightness_255 > 0) && (percent == 0)) percent = 1;
    s_ctx->backlight->setBrightness(percent);
}

bool halo_panel_is_ready(void)
{
    return (s_ctx != nullptr) && (s_ctx->lcd != nullptr);
}

void halo_panel_diag_tick(void)
{
    if (s_ctx == nullptr) return;
    maybe_log_diag(s_ctx);
}
