#pragma once

#include <driver/gpio.h>
#include <driver/i2c.h>

// ===== LCD RGB Interface (16-bit RGB565, 800×480) =====
#define LCD_H_RES    800
#define LCD_V_RES    480

#define LCD_PIN_DATA0  GPIO_NUM_8   // B3 (Blue LSB)
#define LCD_PIN_DATA1  GPIO_NUM_9   // B4
#define LCD_PIN_DATA2  GPIO_NUM_10  // B5
#define LCD_PIN_DATA3  GPIO_NUM_11  // B6
#define LCD_PIN_DATA4  GPIO_NUM_12  // B7 (Blue MSB)
#define LCD_PIN_DATA5  GPIO_NUM_13  // G2 (Green LSB)
#define LCD_PIN_DATA6  GPIO_NUM_14  // G3
#define LCD_PIN_DATA7  GPIO_NUM_15  // G4
#define LCD_PIN_DATA8  GPIO_NUM_16  // G5
#define LCD_PIN_DATA9  GPIO_NUM_17  // G6
#define LCD_PIN_DATA10 GPIO_NUM_18  // G7 (Green MSB)
#define LCD_PIN_DATA11 GPIO_NUM_19  // R3 (Red LSB)
#define LCD_PIN_DATA12 GPIO_NUM_33  // R4
#define LCD_PIN_DATA13 GPIO_NUM_34  // R5
#define LCD_PIN_DATA14 GPIO_NUM_35  // R6
#define LCD_PIN_DATA15 GPIO_NUM_36  // R7 (Red MSB)
#define LCD_PIN_PCLK   GPIO_NUM_40  // Pixel Clock
#define LCD_PIN_DE     GPIO_NUM_43  // Data Enable
#define LCD_PIN_HSYNC  GPIO_NUM_44  // Horizontal Sync
#define LCD_PIN_VSYNC  GPIO_NUM_45  // Vertical Sync
#define LCD_PIN_DISP   -1           // Display Enable (not used)

// LCD SPI (init sequence)
#define LCD_PIN_CS    GPIO_NUM_38
#define LCD_PIN_MOSI  GPIO_NUM_60
#define LCD_PIN_SCLK  GPIO_NUM_61

// ===== I2C Bus (shared: GT1151 + ES8389 + OV3660) =====
#define I2C_SDA_PIN   GPIO_NUM_0
#define I2C_SCL_PIN   GPIO_NUM_1
#define I2C_PORT      I2C_NUM_0
#define I2C_CLK_HZ    (400 * 1000)

// GT1151 Touch
#define TOUCH_I2C_ADDR 0x14

// ES8389 Audio Codec
#define ES8389_I2C_ADDR    0x20
#define ES8389_PA_PIN      GPIO_NUM_7

// I2S (ES8389) - Pins match official BSP
#define I2S_MCLK_PIN  GPIO_NUM_2
#define I2S_BCLK_PIN  GPIO_NUM_3
#define I2S_WS_PIN    GPIO_NUM_4
#define I2S_DOUT_PIN  GPIO_NUM_5   // ESP32 data out → ES8389 DIN (was incorrectly GPIO6)
#define I2S_DIN_PIN   GPIO_NUM_6   // ESP32 data in ← ES8389 DOUT (was incorrectly GPIO5)

// ===== Camera DVP =====
#define CAM_PIN_D2    GPIO_NUM_46
#define CAM_PIN_D3    GPIO_NUM_47
#define CAM_PIN_D4    GPIO_NUM_48
#define CAM_PIN_D5    GPIO_NUM_49
#define CAM_PIN_D6    GPIO_NUM_50
#define CAM_PIN_D7    GPIO_NUM_51
#define CAM_PIN_D8    GPIO_NUM_52
#define CAM_PIN_D9    GPIO_NUM_53
#define CAM_PIN_PCLK  GPIO_NUM_54
#define CAM_PIN_XCLK  GPIO_NUM_55
#define CAM_PIN_VSYNC GPIO_NUM_56
#define CAM_PIN_HREF  GPIO_NUM_57

// ===== Others =====
#define RGB_LED_PIN   GPIO_NUM_37
#define SD_CTRL_PIN   GPIO_NUM_39
#define ADC_BTN_PIN   GPIO_NUM_42
