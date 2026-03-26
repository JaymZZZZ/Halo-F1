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

#ifndef HALO_LCD_FULL_PRESENT
#define HALO_LCD_FULL_PRESENT 0
#endif

#ifndef HALO_PANEL_BOOT_LOG
#define HALO_PANEL_BOOT_LOG 1
#endif

#if HALO_PANEL_BOOT_LOG
#define HALO_BOOT_PRINTLN(msg) do { Serial.println(msg); Serial.flush(); } while (0)
#else
#define HALO_BOOT_PRINTLN(msg) do { } while (0)
#endif

using namespace esp_panel::board;
using namespace esp_panel::drivers;

typedef struct {
    Board *board;
    LCD *lcd;
    Touch *touch;
    Backlight *backlight;
    void *draw_buf;
    uint16_t *logical_fb;
    uint32_t logical_fb_pixels;
    bool full_present_enabled;
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
    HALO_BOOT_PRINTLN("[PanelBoot] init_board begin");
    ctx->board = new Board();
    if (ctx->board == nullptr) return false;
    HALO_BOOT_PRINTLN("[PanelBoot] board allocated");
    if (!ctx->board->init()) return false;
    HALO_BOOT_PRINTLN("[PanelBoot] board init ok");

    ctx->lcd = ctx->board->getLCD();
    if (ctx->lcd == nullptr) return false;
    HALO_BOOT_PRINTLN("[PanelBoot] lcd acquired");

    auto *lcd_bus = ctx->lcd->getBus();
    if ((lcd_bus != nullptr) && (lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB)) {
        HALO_BOOT_PRINTLN("[PanelBoot] rgb bus config");
        ctx->lcd->configFrameBufferNumber(HALO_RGB_FRAME_BUFFERS);
#if CONFIG_IDF_TARGET_ESP32S3
        auto *rgb_bus = static_cast<BusRGB *>(lcd_bus);
        rgb_bus->configRGB_BounceBufferSize(ctx->lcd->getFrameWidth() * HALO_RGB_BOUNCE_LINES);
        rgb_bus->configRGB_FreqHz(HALO_RGB_PCLK_HZ);
#endif
    }

    HALO_BOOT_PRINTLN("[PanelBoot] board begin...");
    if (!ctx->board->begin()) return false;
    HALO_BOOT_PRINTLN("[PanelBoot] board begin ok");

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
    }
    HALO_BOOT_PRINTLN("[PanelBoot] fb clear complete");

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
    ctx->logical_fb = nullptr;
    ctx->logical_fb_pixels = 0;
    ctx->full_present_enabled = false;
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
        "skip=%lu draw_fail=%lu mode=%d full=%d phys=%ldx%ld "
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
        ctx->portrait_mode ? 1 : 0,
        ctx->full_present_enabled ? 1 : 0,
        (long)ctx->physical_width,
        (long)ctx->physical_height,
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
    HALO_BOOT_PRINTLN("[PanelBoot] display_create begin");

    halo_panel_ctx_t *ctx = (halo_panel_ctx_t *)lv_malloc_zeroed(sizeof(halo_panel_ctx_t));
    LV_ASSERT_MALLOC(ctx);
    if (ctx == nullptr) return nullptr;
    HALO_BOOT_PRINTLN("[PanelBoot] ctx allocated");

    if (!init_board(ctx)) {
        lv_free(ctx);
        return nullptr;
    }
    HALO_BOOT_PRINTLN("[PanelBoot] init_board done");

    const int32_t lv_w = SCREEN_WIDTH;
    const int32_t lv_h = SCREEN_HEIGHT;
    const uint32_t draw_buf_size = ((uint32_t)lv_w * (uint32_t)lv_h / HALO_LCD_DRAW_BUF_DIV) * sizeof(lv_color_t);
    const uint32_t logical_fb_size = (uint32_t)lv_w * (uint32_t)lv_h * sizeof(uint16_t);

    ctx->draw_buf = heap_caps_malloc(draw_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx->draw_buf == nullptr) {
        ctx->draw_buf = heap_caps_malloc(draw_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    LV_ASSERT_MALLOC(ctx->draw_buf);
    if (ctx->draw_buf == nullptr) {
        lv_free(ctx);
        return nullptr;
    }
    HALO_BOOT_PRINTLN("[PanelBoot] draw_buf allocated");

    // Maintain a full logical frame so every presented frame is complete (not only dirty rects).
    // This avoids showing panel power-on garbage/white when only small areas are invalidated.
    ctx->logical_fb = (uint16_t *)heap_caps_malloc(logical_fb_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ctx->logical_fb == nullptr) {
        ctx->logical_fb = (uint16_t *)heap_caps_malloc(logical_fb_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
#if HALO_LCD_FULL_PRESENT
    if (ctx->logical_fb != nullptr) {
        memset(ctx->logical_fb, 0x00, logical_fb_size);
        ctx->logical_fb_pixels = logical_fb_size / sizeof(uint16_t);
        ctx->full_present_enabled = true;
    } else {
        ctx->logical_fb_pixels = 0;
        ctx->full_present_enabled = false;
    }
#else
    if (ctx->logical_fb != nullptr) {
        lv_free(ctx->logical_fb);
        ctx->logical_fb = nullptr;
    }
    ctx->logical_fb_pixels = 0;
    ctx->full_present_enabled = false;
#endif

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
        if (ctx->logical_fb != nullptr) lv_free(ctx->logical_fb);
        lv_free(ctx->draw_buf);
        lv_free(ctx);
        return nullptr;
    }
    HALO_BOOT_PRINTLN("[PanelBoot] rotate_buf allocated");
    ctx->rotate_buf_pixels = rotate_buf_size / sizeof(uint16_t);

    lv_display_t *disp = lv_display_create(lv_w, lv_h);
    if (disp == nullptr) {
        lv_free(ctx->rotate_buf);
        if (ctx->logical_fb != nullptr) lv_free(ctx->logical_fb);
        lv_free(ctx->draw_buf);
        lv_free(ctx);
        return nullptr;
    }

    lv_display_set_driver_data(disp, (void *)ctx);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, ctx->draw_buf, nullptr, draw_buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);

    s_ctx = ctx;
    HALO_BOOT_PRINTLN("[PanelBoot] display_create complete");
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

    if (ctx->full_present_enabled && (ctx->logical_fb != nullptr)) {
        // Compose dirty area into a persistent logical frame.
        uint16_t *dst_log = ctx->logical_fb + (uint32_t)clip_y1 * (uint32_t)log_w + (uint32_t)clip_x1;
        for (int32_t row = 0; row < clip_h; row++) {
            memcpy(dst_log + (uint32_t)row * (uint32_t)log_w,
                   src + (uint32_t)row * (uint32_t)src_full_w,
                   (size_t)clip_w * sizeof(uint16_t));
        }

        // Present every flush. This is heavier, but avoids depending on `flush_is_last`
        // semantics that can vary between LVGL integration paths.
        if (ctx->portrait_mode) {
            const uint32_t phys_pixels = (uint32_t)ctx->physical_width * (uint32_t)ctx->physical_height;
            if ((ctx->rotate_buf == nullptr) || (ctx->rotate_buf_pixels < phys_pixels)) {
                ctx->diag_rotate_skips++;
            } else {
                memset(ctx->rotate_buf, 0x00, phys_pixels * sizeof(uint16_t));

                for (int32_t y = 0; y < log_h; y++) {
                    const uint16_t *src_row = ctx->logical_fb + (uint32_t)y * (uint32_t)log_w;
                    for (int32_t x = 0; x < log_w; x++) {
                        int32_t px;
                        int32_t py;
                        if (HALO_UI_ROTATION_CW_90) {
                            px = ctx->physical_width - 1 - y;
                            py = x;
                        } else {
                            px = y;
                            py = ctx->physical_height - 1 - x;
                        }

                        if ((uint32_t)px < (uint32_t)ctx->physical_width &&
                            (uint32_t)py < (uint32_t)ctx->physical_height) {
                            ctx->rotate_buf[(uint32_t)py * (uint32_t)ctx->physical_width + (uint32_t)px] = src_row[x];
                        } else {
                            ctx->diag_oob_writes++;
                        }
                    }
                }

                if (!ctx->lcd->drawBitmap(
                        0, 0, ctx->physical_width, ctx->physical_height,
                        (const uint8_t *)ctx->rotate_buf, 0)) {
                    ctx->diag_draw_fails++;
                }
            }
        } else {
            const int32_t out_w = LV_MIN(log_w, ctx->physical_width);
            const int32_t out_h = LV_MIN(log_h, ctx->physical_height);
            if (!ctx->lcd->drawBitmap(0, 0, out_w, out_h, (const uint8_t *)ctx->logical_fb, 0)) {
                ctx->diag_draw_fails++;
            }
        }

        lv_display_flush_ready(disp);
        return;
    }

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

        if (ctx->fb_swap_enabled && (ctx->fb_front != nullptr) && (ctx->fb_back != nullptr) && (ctx->fb_pixels > 0)) {
            if (!ctx->fb_frame_open) {
                // Start a new composition frame from the currently shown framebuffer.
                memcpy(ctx->fb_back, ctx->fb_front, ctx->fb_pixels * sizeof(uint16_t));
                ctx->fb_frame_open = true;
                ctx->diag_frame_starts++;
            }

            // Write rotated dirty rect into the back buffer with framebuffer stride.
            uint16_t *dst = ctx->fb_back + (uint32_t)phys_y * (uint32_t)ctx->physical_width + (uint32_t)phys_x;
            for (int32_t row = 0; row < phys_h; row++) {
                memcpy(dst + (uint32_t)row * (uint32_t)ctx->physical_width, ctx->rotate_buf + (uint32_t)row * (uint32_t)phys_w, (size_t)phys_w * sizeof(uint16_t));
            }

            if (lv_display_flush_is_last(disp)) {
                const uint32_t prev_gen = ctx->refresh_gen;
                if (ctx->lcd->switchFrameBufferTo(ctx->fb_back)) {
                    ctx->diag_frame_switches++;

                    // Wait for the refresh completion callback to ensure we're not writing into the active buffer.
                    const uint32_t start_wait = millis();
                    while (ctx->refresh_gen == prev_gen) {
                        if ((uint32_t)(millis() - start_wait) > 30U) {
                            ctx->diag_switch_timeouts++;
                            break;
                        }
                        delay(1);
                    }
                    const uint32_t waited = (uint32_t)(millis() - start_wait);
                    if (waited > ctx->diag_max_wait_ms) ctx->diag_max_wait_ms = waited;

                    uint16_t *tmp = ctx->fb_front;
                    ctx->fb_front = ctx->fb_back;
                    ctx->fb_back = tmp;
                } else {
                    ctx->diag_draw_fails++;
                }
                ctx->fb_frame_open = false;
            }
        } else {
            if (!ctx->lcd->drawBitmap(phys_x, phys_y, phys_w, phys_h, (const uint8_t *)ctx->rotate_buf, 0)) {
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
