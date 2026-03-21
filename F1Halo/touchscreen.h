// TOUCH SCREEN
#ifdef TOUCH_CAPACITIVE
#include <bb_captouch.h>

BBCapTouch bbct;
#else
BB_SPI_LCD * lcd;
#endif

uint16_t touchMinX = TOUCH_MIN_X, touchMaxX = TOUCH_MAX_X, touchMinY = TOUCH_MIN_Y, touchMaxY = TOUCH_MAX_Y;
TOUCHINFO ti;

static inline int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

void touch_read( lv_indev_t * indev, lv_indev_data_t * data ) {
  lv_display_t * disp = lv_indev_get_display(indev);
  if (disp == NULL) {
    disp = lv_display_get_default();
  }
  int32_t raw_w = lv_display_get_original_horizontal_resolution(disp);
  int32_t raw_h = lv_display_get_original_vertical_resolution(disp);
  if (raw_w <= 0) raw_w = lv_display_get_horizontal_resolution(disp);
  if (raw_h <= 0) raw_h = lv_display_get_vertical_resolution(disp);

#ifdef TOUCH_CAPACITIVE
  // Capacitive touch needs to be mapped to display pixels
  if (bbct.getSamples(&ti) && ti.count > 0) {
    /*Serial.print("raw touch x: ");
    Serial.print(ti.x[0]);
    Serial.print(" y: ");
    Serial.println(ti.y[0]);*/

#if HALO_BOARD_MODEL == HALO_BOARD_JC8048W550
    // For JC8048: feed LVGL coordinates in the display's *original* (unrotated) space.
    // LVGL will apply display rotation once in indev processing.
    int32_t rx = clamp_i32((int32_t)ti.x[0], TOUCH_MIN_X, TOUCH_MAX_X);
    int32_t ry = clamp_i32((int32_t)ti.y[0], TOUCH_MIN_Y, TOUCH_MAX_Y);
    data->point.x = map(rx, TOUCH_MIN_X, TOUCH_MAX_X, 0, raw_w - 1);
    data->point.y = map(ry, TOUCH_MIN_Y, TOUCH_MAX_Y, 0, raw_h - 1);
#else
    if(ti.x[0] < touchMinX) touchMinX = ti.x[0];
    if(ti.x[0] > touchMaxX) touchMaxX = ti.x[0];
    if(ti.y[0] < touchMinY) touchMinY = ti.y[0];
    if(ti.y[0] > touchMaxY) touchMaxY = ti.y[0];

    // Map this to the pixel position
    data->point.y = raw_h - map(ti.x[0], touchMinX, touchMaxX, 1, raw_h); // X touch mapping
    data->point.x = map(ti.y[0], touchMinY, touchMaxY, 1, raw_w);          // Y touch mapping
#endif
    data->point.x = clamp_i32(data->point.x, 0, raw_w - 1);
    data->point.y = clamp_i32(data->point.y, 0, raw_h - 1);
    data->state = LV_INDEV_STATE_PRESSED;
#else
  // Resistive touch is already mapped by the bb_spi_lcd library
  if(lcd->rtReadTouch(&ti)) {
    if(ti.x[0] < touchMinX) touchMinX = ti.x[0];
    if(ti.x[0] > touchMaxX) touchMaxX = ti.x[0];
    if(ti.y[0] < touchMinY) touchMinY = ti.y[0];
    if(ti.y[0] > touchMaxY) touchMaxY = ti.y[0];

    data->point.x = raw_w - ti.x[0];
    data->point.y = map(ti.y[0], touchMinY, touchMaxY, 1, raw_h);
    data->point.x = clamp_i32(data->point.x, 0, raw_w - 1);
    data->point.y = clamp_i32(data->point.y, 0, raw_h - 1);
    data->state = LV_INDEV_STATE_PRESSED;
#endif


    /*Serial.print("mapped touch x: ");
    Serial.print(data->point.x);
    Serial.print(" y: ");
    Serial.println(data->point.y);*/
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}
// END TOUCH SCREEN
