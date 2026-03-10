// i2s
// External I2S DAC
#define I2S_BCK_PIN 7
#define I2S_WS_PIN 6 
#define I2S_DATA_OUT_PIN 15
// Internal I2S DAC mono  
#define I2S_BCK_PIN2 42
#define I2S_WS_PIN2 2
#define I2S_DATA_OUT_PIN2 41

#include "ESP_I2S.h"
I2SClass I2S;

#include <incbin.h>
INCBIN(NotificationSound, "notification.mp3");

void playNotificationSound() {
  I2S.setPins(I2S_BCK_PIN2, I2S_WS_PIN2, I2S_DATA_OUT_PIN2, -1, -1); //SCK, WS, SDOUT, SDIN, MCLK
  I2S.begin(I2S_MODE_STD, 16000, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  I2S.playMP3((uint8_t*)gNotificationSoundData,gNotificationSoundSize);
  I2S.end();
}