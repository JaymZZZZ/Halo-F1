/**
 * @file lv_bb_spi_lcd_h
 *
 */

#ifndef LV_BB_SPI_LCD_H
#define LV_BB_SPI_LCD_H

#include <Arduino.h>
#include <stdint.h>
#include <bb_spi_lcd.h>

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include <display/lv_display.h>

/*********************
 *      DEFINES
 *********************/

/**********************
 *      TYPEDEFS
 **********************/
typedef struct {
    BB_SPI_LCD * lcd;
    uint8_t * rotate_buf;
    size_t rotate_buf_size;
} lv_bb_spi_lcd_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/
lv_display_t * lv_bb_spi_lcd_create(int iType);

/**********************
 *      MACROS
 **********************/

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LV_BB_SPI_LCD_H */
