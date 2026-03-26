#pragma once

// JC8048W550 single-board configuration (ESP32_Display_Panel stack)
#define HALO_BOARD_PROFILE "JC8048W550"

// Physical LCD geometry (RGB panel native timing)
#define HALO_LCD_PHYSICAL_WIDTH 800
#define HALO_LCD_PHYSICAL_HEIGHT 480

// Render UI in portrait, then rotate during flush.
// 1 = rotate clockwise 90 deg, 0 = rotate counter-clockwise 90 deg.
#define HALO_UI_ROTATION_CW_90 1

// LVGL draw buffer divisor: 4 = 1/4 screen, 8 = 1/8 screen.
// Larger buffers reduce partial-flush churn and improve scroll smoothness.
#define HALO_LCD_DRAW_BUF_DIV 12

// 1 = compose and present full logical frame each flush, 0 = direct partial flush path.
// Keep disabled for JC8048W550 because full-frame compose path can white-screen on some boots.
#define HALO_LCD_FULL_PRESENT 0

// RGB anti-tear tuning for ESP32_Display_Panel driver stack.
#define HALO_RGB_FRAME_BUFFERS 1
#define HALO_RGB_BOUNCE_LINES 40
#define HALO_RGB_PCLK_HZ (12 * 1000 * 1000)
#define HALO_RGB_USE_FB_SWAP 0

// Logical UI geometry (portrait)
#define SCREEN_WIDTH 480
#define SCREEN_HEIGHT 800

// Personal default timezone behavior.
#define HALO_FORCE_CHICAGO_TZ 1
#define HALO_DEFAULT_UTC_OFFSET_SECONDS (-6 * 3600)

// Runtime diagnostics for long-run drift/tearing analysis.
#define HALO_PANEL_DIAG_LOG 1
#define HALO_PANEL_DIAG_INTERVAL_MS (15000UL)

// Reduce periodic heavy redraws on the race tab to limit tearing on RGB partial flush.
// 0 disables auto animation cycling for standings/results.
#define HALO_STANDINGS_ANIMATION_MS 0
