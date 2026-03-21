/**
 * @file lv_bb_spi_lcd.cpp
 *
 */

/*********************
 *      INCLUDES
 *********************/
#include "lv_bb_spi_lcd.h"
#include <draw/lv_draw_buf.h>
#include <draw/sw/lv_draw_sw_utils.h>

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/

/**********************
 *  STATIC PROTOTYPES
 **********************/
static void flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map);

/**********************
 *  STATIC VARIABLES
 **********************/

/**********************
 *      MACROS
 **********************/

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

lv_display_t * lv_bb_spi_lcd_create(int iType)
{
    void *draw_buf;
    int draw_buf_size;
    lv_bb_spi_lcd_t * dsc = (lv_bb_spi_lcd_t *)lv_malloc_zeroed(sizeof(lv_bb_spi_lcd_t));
    LV_ASSERT_MALLOC(dsc);
    if(dsc == NULL) return NULL;

    dsc->lcd = new BB_SPI_LCD();
    dsc->rotate_buf = NULL;
    dsc->rotate_buf_size = 0;
    dsc->lcd->begin(iType);
    dsc->lcd->setBrightness(255);
    dsc->lcd->setRotation(3);

    lv_display_t * disp = lv_display_create(dsc->lcd->width(), dsc->lcd->height());
    if(disp == NULL) {
        lv_free(dsc);
        return NULL;
    }

/*LVGL draw into this buffer, 1/10 screen size usually works well. The size is in bytes*/
    draw_buf_size = ((dsc->lcd->width() * dsc->lcd->height()) / 10) * (LV_COLOR_DEPTH / 8);
    draw_buf = heap_caps_malloc(draw_buf_size, MALLOC_CAP_8BIT);
    if(draw_buf == NULL) {
        lv_display_delete(disp);
        lv_free(dsc);
        return NULL;
    }
    dsc->lcd->fillScreen(TFT_BLACK);
    lv_display_set_driver_data(disp, (void *)dsc);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_buffers(disp, (void *)draw_buf, NULL, draw_buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL); //LV_DISPLAY_RENDER_MODE_PARTIAL
    return disp;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/
static void flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map)
{
    lv_bb_spi_lcd_t * dsc = (lv_bb_spi_lcd_t *)lv_display_get_driver_data(disp);
    lv_area_t draw_area = *area;
    uint8_t * draw_px_map = px_map;
    lv_color_format_t cf = lv_display_get_color_format(disp);
    lv_display_rotation_t rotation = lv_display_get_rotation(disp);
    uint32_t px_size = lv_color_format_get_size(cf);
    uint32_t draw_stride = lv_draw_buf_width_to_stride(lv_area_get_width(&draw_area), cf);

    if(rotation != LV_DISPLAY_ROTATION_0) {
        int32_t src_w = lv_area_get_width(area);
        int32_t src_h = lv_area_get_height(area);
        uint32_t src_stride = lv_draw_buf_width_to_stride(src_w, cf);

        lv_display_rotate_area(disp, &draw_area);
        uint32_t dest_stride = lv_draw_buf_width_to_stride(lv_area_get_width(&draw_area), cf);
        size_t needed_buf = (size_t)dest_stride * (size_t)lv_area_get_height(&draw_area);

        if(dsc->rotate_buf == NULL || dsc->rotate_buf_size < needed_buf) {
            if(dsc->rotate_buf) {
                heap_caps_free(dsc->rotate_buf);
            }
            dsc->rotate_buf = (uint8_t *)heap_caps_malloc(needed_buf, MALLOC_CAP_8BIT);
            dsc->rotate_buf_size = dsc->rotate_buf ? needed_buf : 0;
        }

        if(dsc->rotate_buf) {
            lv_draw_sw_rotate(px_map, dsc->rotate_buf, src_w, src_h, src_stride, dest_stride, rotation, cf);
            draw_px_map = dsc->rotate_buf;
            draw_stride = dest_stride;
        } else {
            draw_area = *area;
            draw_px_map = px_map;
            draw_stride = src_stride;
        }
    }

    int32_t clip_x1 = LV_MAX(draw_area.x1, 0);
    int32_t clip_y1 = LV_MAX(draw_area.y1, 0);
    int32_t clip_x2 = LV_MIN(draw_area.x2, dsc->lcd->width() - 1);
    int32_t clip_y2 = LV_MIN(draw_area.y2, dsc->lcd->height() - 1);
    if(clip_x1 > clip_x2 || clip_y1 > clip_y2) {
        lv_display_flush_ready(disp);
        return;
    }

    if(clip_x1 != draw_area.x1 || clip_y1 != draw_area.y1) {
        size_t row_off = (size_t)(clip_y1 - draw_area.y1) * (size_t)draw_stride;
        size_t col_off = (size_t)(clip_x1 - draw_area.x1) * (size_t)px_size;
        draw_px_map += row_off + col_off;
    }

    uint32_t w = (uint32_t)(clip_x2 - clip_x1 + 1);
    uint32_t h = (uint32_t)(clip_y2 - clip_y1 + 1);
    uint32_t packed_stride = w * px_size;

    if(draw_stride == packed_stride) {
        uint16_t * p = (uint16_t *)draw_px_map;
        for (uint32_t i=0; i<w*h; i++) {
            p[i] = __builtin_bswap16(p[i]);
        }
        dsc->lcd->setAddrWindow(clip_x1, clip_y1, w, h);
        dsc->lcd->pushPixels((uint16_t *)draw_px_map, w * h); //, DRAW_TO_LCD | DRAW_WITH_DMA);
    } else {
        for(uint32_t row = 0; row < h; row++) {
            uint16_t * p = (uint16_t *)(draw_px_map + ((size_t)row * draw_stride));
            for(uint32_t col = 0; col < w; col++) {
                p[col] = __builtin_bswap16(p[col]);
            }
            dsc->lcd->setAddrWindow(clip_x1, clip_y1 + row, w, 1);
            dsc->lcd->pushPixels(p, w);
        }
    }

    lv_display_flush_ready(disp);
}
