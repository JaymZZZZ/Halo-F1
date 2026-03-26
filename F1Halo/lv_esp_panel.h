#pragma once

#include <stdint.h>
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

lv_display_t *halo_panel_display_create(void);
void halo_panel_touch_read(lv_indev_t *indev, lv_indev_data_t *data);
void halo_panel_set_brightness(uint8_t brightness_255);
bool halo_panel_is_ready(void);
void halo_panel_diag_tick(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
