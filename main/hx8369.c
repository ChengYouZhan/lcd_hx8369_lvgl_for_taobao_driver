/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <sys/cdefs.h>
#include "sdkconfig.h"
#if CONFIG_LCD_ENABLE_DEBUG_LOG
// The local log level must be defined before including esp_log.h
// Set the maximum log level for this source file
#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_commands.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_check.h"
// #include "lvgl_helpers.h"
// #include "hx8369.h"
static const char *TAG = "lcd_panel.hx8369";

// #define PIN_NUM_DATA0 46
// #define PIN_NUM_DATA1 3
// #define PIN_NUM_DATA2 8
// #define PIN_NUM_DATA3 18
// #define PIN_NUM_DATA4 17
// #define PIN_NUM_DATA5 16
// #define PIN_NUM_DATA6 15
// #define PIN_NUM_DATA7 7

// #define PIN_NUM_PCLK 10
// #define PIN_NUM_CS 12
// #define PIN_NUM_DC 11
// #define PIN_NUM_RST 9
// #define PIN_NUM_BK_LIGHT 6

// // Bit number used to represent command and parameter
// #define LCD_CMD_BITS 8
// #define LCD_PARAM_BITS 8

static esp_err_t panel_hx8369_del(esp_lcd_panel_t *panel);
static esp_err_t panel_hx8369_reset(esp_lcd_panel_t *panel);
static esp_err_t panel_hx8369_init(esp_lcd_panel_t *panel);
static esp_err_t panel_hx8369_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data);
static esp_err_t panel_hx8369_invert_color(esp_lcd_panel_t *panel, bool invert_color_data);
static esp_err_t panel_hx8369_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y);
static esp_err_t panel_hx8369_swap_xy(esp_lcd_panel_t *panel, bool swap_axes);
static esp_err_t panel_hx8369_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap);
// static
esp_err_t panel_hx8369_disp_on_off(esp_lcd_panel_t *panel, bool off);

typedef struct
{
    esp_lcd_panel_t base;
    esp_lcd_panel_io_handle_t io;
    int reset_gpio_num;
    bool reset_level;
    int x_gap;
    int y_gap;
    uint8_t fb_bits_per_pixel;
    uint8_t madctl_val; // save current value of LCD_CMD_MADCTL register
    uint8_t colmod_cal; // save surrent value of LCD_CMD_COLMOD register
} hx8369_panel_t;

esp_err_t esp_lcd_new_panel_hx8369(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel)
{
#if CONFIG_LCD_ENABLE_DEBUG_LOG
    esp_log_level_set(TAG, ESP_LOG_DEBUG);
#endif
    esp_err_t ret = ESP_OK;
    hx8369_panel_t *hx8369 = NULL;
    ESP_GOTO_ON_FALSE(io && panel_dev_config && ret_panel, ESP_ERR_INVALID_ARG, err, TAG, "invalid argument");
    hx8369 = calloc(1, sizeof(hx8369_panel_t));
    ESP_GOTO_ON_FALSE(hx8369, ESP_ERR_NO_MEM, err, TAG, "no mem for hx8369 panel");

    if (panel_dev_config->reset_gpio_num >= 0)
    {
        gpio_config_t io_conf = {
            .mode = GPIO_MODE_OUTPUT,
            .pin_bit_mask = 1ULL << panel_dev_config->reset_gpio_num,
        };
        ESP_GOTO_ON_ERROR(gpio_config(&io_conf), err, TAG, "configure GPIO for RST line failed");
    }

    switch (panel_dev_config->color_space)
    {
    case ESP_LCD_COLOR_SPACE_RGB:
        hx8369->madctl_val = 0;
        break;
    case ESP_LCD_COLOR_SPACE_BGR:
        hx8369->madctl_val |= LCD_CMD_BGR_BIT;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported color space");
        break;
    }

    uint8_t fb_bits_per_pixel = 0;
    switch (panel_dev_config->bits_per_pixel)
    {
    case 16:
        hx8369->colmod_cal = 0x55;
        fb_bits_per_pixel = 16;
        break;
    case 18:
        hx8369->colmod_cal = 0x66;
        fb_bits_per_pixel = 24;
        break;
    default:
        ESP_GOTO_ON_FALSE(false, ESP_ERR_NOT_SUPPORTED, err, TAG, "unsupported pixel width");
        break;
    }

    hx8369->io = io;
    hx8369->fb_bits_per_pixel = fb_bits_per_pixel;
    hx8369->reset_gpio_num = panel_dev_config->reset_gpio_num;
    hx8369->reset_level = panel_dev_config->flags.reset_active_high;
    hx8369->base.del = panel_hx8369_del;
    hx8369->base.reset = panel_hx8369_reset;
    hx8369->base.init = panel_hx8369_init;
    hx8369->base.draw_bitmap = panel_hx8369_draw_bitmap;
    hx8369->base.invert_color = panel_hx8369_invert_color;
    hx8369->base.set_gap = panel_hx8369_set_gap;
    hx8369->base.mirror = panel_hx8369_mirror;
    hx8369->base.swap_xy = panel_hx8369_swap_xy;
    hx8369->base.disp_on_off = panel_hx8369_disp_on_off;
    *ret_panel = &(hx8369->base);
    ESP_LOGD(TAG, "new hx8369 panel @%p", hx8369);

    return ESP_OK;

err:
    if (hx8369)
    {
        if (panel_dev_config->reset_gpio_num >= 0)
        {
            gpio_reset_pin(panel_dev_config->reset_gpio_num);
        }
        free(hx8369);
    }
    return ret;
}

static esp_err_t panel_hx8369_del(esp_lcd_panel_t *panel)
{
    hx8369_panel_t *hx8369 = __containerof(panel, hx8369_panel_t, base);

    if (hx8369->reset_gpio_num >= 0)
    {
        gpio_reset_pin(hx8369->reset_gpio_num);
    }
    ESP_LOGD(TAG, "del hx8369 panel @%p", hx8369);
    free(hx8369);
    return ESP_OK;
}

static esp_err_t panel_hx8369_reset(esp_lcd_panel_t *panel)
{
    hx8369_panel_t *hx8369 = __containerof(panel, hx8369_panel_t, base);
    esp_lcd_panel_io_handle_t io = hx8369->io;

    // perform hardware reset
    if (hx8369->reset_gpio_num >= 0)
    {
        gpio_set_level(hx8369->reset_gpio_num, hx8369->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(hx8369->reset_gpio_num, !hx8369->reset_level);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    else
    { // perform software reset
        esp_lcd_panel_io_tx_param(io, LCD_CMD_SWRESET, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(20)); // spec, wait at least 5m before sending new command
    }

    return ESP_OK;
}

static esp_err_t panel_hx8369_init(esp_lcd_panel_t *panel)
{
    hx8369_panel_t *hx8369 = __containerof(panel, hx8369_panel_t, base);
    esp_lcd_panel_io_handle_t io = hx8369->io;
    // Init LCD

    // Set_EXTC
    esp_lcd_panel_io_tx_param(io, 0xB9, (uint8_t[]){0xFF, 0x83, 0x69}, 3);
    // Set Power
    esp_lcd_panel_io_tx_param(io, 0xB1, (uint8_t[]){0x01, 0x00, 0x34, 0x06, 0x00, 0x0f, 0x0f, 0x2a, 0x32, 0x3f, //
                                                    0x3f, 0x07, 0x23, 0x01, 0xe6, 0xe6, 0xe6, 0xe6, 0xe6},
                              19);

    // SET Display 480x800
    // 0x2b;0x20-MCU;0x29-DPI;RM,DM; RM=0:DPI IF;  RM=1:RGB IF;
    esp_lcd_panel_io_tx_param(io, 0xB2, (uint8_t[]){0x00, 0x20, 0x03, 0x03, 0x70, 0x00, 0xff, 0x00, 0x00, 0x00, //
                                                    0x00, 0x03, 0x03, 0x00, 0x01},
                              15);
    // SET Display CYC
    esp_lcd_panel_io_tx_param(io, 0xb4, (uint8_t[]){0x00, 0x0C, 0xA0, 0x0E, 0x06},
                              5);
    // SET VCOM
    esp_lcd_panel_io_tx_param(io, 0xb6, (uint8_t[]){0x2C, 0x2C},
                              2);

    // SET GIP
    esp_lcd_panel_io_tx_param(io, 0xD5, (uint8_t[]){0x00, 0x05, 0x03, 0x00, 0x01, 0x09, 0x10, 0x80, 0x37, 0x37, 0x20, 0x31, 0x46, //
                                                    0x8a, 0x57, 0x9b, 0x20, 0x31, 0x46, 0x8a, 0x57, 0x9b, 0x07, 0x0f, 0x02, 0x00},
                              26);

    //  SET GAMMA
    esp_lcd_panel_io_tx_param(io, 0xE0, (uint8_t[]){0x00, 0x08, 0x0d, 0x2d, 0x34, 0x3f, 0x19, 0x38, 0x09, 0x0e, 0x0e, 0x12, 0x14, 0x12, 0x14, 0x13, 0x19, //
                                                    0x00, 0x08, 0x0d, 0x2d, 0x34, 0x3f, 0x19, 0x38, 0x09, 0x0e, 0x0e, 0x12, 0x14, 0x12, 0x14, 0x13, 0x19},
                              34);

    // set DGC
    esp_lcd_panel_io_tx_param(io, 0xC1, (uint8_t[]){0x01, 0x02, 0x08, 0x12, 0x1a, 0x22, 0x2a, 0x31, 0x36, 0x3f, 0x48, 0x51, 0x58, 0x60, 0x68, 0x70, //
                                                    0x78, 0x80, 0x88, 0x90, 0x98, 0xa0, 0xa7, 0xaf, 0xb6, 0xbe, 0xc7, 0xce, 0xd6, 0xde, 0xe6, 0xef, //
                                                    0xf5, 0xfb, 0xfc, 0xfe, 0x8c, 0xa4, 0x19, 0xec, 0x1b, 0x4c, 0x40, 0x02, 0x08, 0x12, 0x1a, 0x22, //
                                                    0x2a, 0x31, 0x36, 0x3f, 0x48, 0x51, 0x58, 0x60, 0x68, 0x70, 0x78, 0x80, 0x88, 0x90, 0x98, 0xa0, //
                                                    0xa7, 0xaf, 0xb6, 0xbe, 0xc7, 0xce, 0xd6, 0xde, 0xe6, 0xef, 0xf5, 0xfb, 0xfc, 0xfe, 0x8c, 0xa4, //
                                                    0x19, 0xec, 0x1b, 0x4c, 0x40, 0x02, 0x08, 0x12, 0x1a, 0x22, 0x2a, 0x31, 0x36, 0x3f, 0x48, 0x51, //
                                                    0x58, 0x60, 0x68, 0x70, 0x78, 0x80, 0x88, 0x90, 0x98, 0xa0, 0xa7, 0xaf, 0xb6, 0xbe, 0xc7, 0xce, //
                                                    0xd6, 0xde, 0xe6, 0xef, 0xf5, 0xfb, 0xfc, 0xfe, 0x8c, 0xa4, 0x19, 0xec, 0x1b, 0x4c, 0x40},
                              127);

    //  Colour Set
    uint8_t cmd_192[192];
    for (size_t i = 0; i <= 63; i++)
    {
        cmd_192[i] = i * 8;
    }
    for (size_t i = 64; i <= 127; i++)
    {
        cmd_192[i] = i * 4;
    }
    for (size_t i = 128; i <= 191; i++)
    {
        cmd_192[i] = i * 8;
    }
    esp_lcd_panel_io_tx_param(io, 0x2D, cmd_192,
                              192);

    // LCD goes into sleep mode and display will be turned off after power on reset, exit sleep mode first
    esp_lcd_panel_io_tx_param(io, LCD_CMD_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]){
                                                      hx8369->madctl_val,
                                                  },
                              1);
    esp_lcd_panel_io_tx_param(io, LCD_CMD_COLMOD, (uint8_t[]){
                                                      hx8369->colmod_cal,
                                                  },
                              1);

    return ESP_OK;
}

static esp_err_t panel_hx8369_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    hx8369_panel_t *hx8369 = __containerof(panel, hx8369_panel_t, base);
    assert((x_start < x_end) && (y_start < y_end) && "start position must be smaller than end position");
    esp_lcd_panel_io_handle_t io = hx8369->io;

    x_start += hx8369->x_gap;
    x_end += hx8369->x_gap;
    y_start += hx8369->y_gap;
    y_end += hx8369->y_gap;

    // define an area of frame memory where MCU can access
    esp_lcd_panel_io_tx_param(io, LCD_CMD_CASET, (uint8_t[]){
                                                     (x_start >> 8) & 0xFF,
                                                     x_start & 0xFF,
                                                     ((x_end - 1) >> 8) & 0xFF,
                                                     (x_end - 1) & 0xFF,
                                                 },
                              4);
    esp_lcd_panel_io_tx_param(io, LCD_CMD_RASET, (uint8_t[]){
                                                     (y_start >> 8) & 0xFF,
                                                     y_start & 0xFF,
                                                     ((y_end - 1) >> 8) & 0xFF,
                                                     (y_end - 1) & 0xFF,
                                                 },
                              4);
    // transfer frame buffer
    size_t len = (x_end - x_start) * (y_end - y_start) * hx8369->fb_bits_per_pixel / 8;
    esp_lcd_panel_io_tx_color(io, LCD_CMD_RAMWR, color_data, len);

    return ESP_OK;
}

static esp_err_t panel_hx8369_invert_color(esp_lcd_panel_t *panel, bool invert_color_data)
{
    hx8369_panel_t *hx8369 = __containerof(panel, hx8369_panel_t, base);
    esp_lcd_panel_io_handle_t io = hx8369->io;
    int command = 0;
    if (invert_color_data)
    {
        command = LCD_CMD_INVON;
    }
    else
    {
        command = LCD_CMD_INVOFF;
    }
    esp_lcd_panel_io_tx_param(io, command, NULL, 0);
    return ESP_OK;
}

static esp_err_t panel_hx8369_mirror(esp_lcd_panel_t *panel, bool mirror_x, bool mirror_y)
{
    hx8369_panel_t *hx8369 = __containerof(panel, hx8369_panel_t, base);
    esp_lcd_panel_io_handle_t io = hx8369->io;
    if (mirror_x)
    {
        hx8369->madctl_val |= LCD_CMD_MX_BIT;
    }
    else
    {
        hx8369->madctl_val &= ~LCD_CMD_MX_BIT;
    }
    if (mirror_y)
    {
        hx8369->madctl_val |= LCD_CMD_MY_BIT;
    }
    else
    {
        hx8369->madctl_val &= ~LCD_CMD_MY_BIT;
    }
    esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]){hx8369->madctl_val}, 1);
    return ESP_OK;
}

static esp_err_t panel_hx8369_swap_xy(esp_lcd_panel_t *panel, bool swap_axes)
{
    hx8369_panel_t *hx8369 = __containerof(panel, hx8369_panel_t, base);
    esp_lcd_panel_io_handle_t io = hx8369->io;
    if (swap_axes)
    {
        hx8369->madctl_val |= LCD_CMD_MV_BIT;
    }
    else
    {
        hx8369->madctl_val &= ~LCD_CMD_MV_BIT;
    }
    esp_lcd_panel_io_tx_param(io, LCD_CMD_MADCTL, (uint8_t[]){hx8369->madctl_val}, 1);
    return ESP_OK;
}

static esp_err_t panel_hx8369_set_gap(esp_lcd_panel_t *panel, int x_gap, int y_gap)
{
    hx8369_panel_t *hx8369 = __containerof(panel, hx8369_panel_t, base);
    hx8369->x_gap = x_gap;
    hx8369->y_gap = y_gap;
    return ESP_OK;
}

// static
esp_err_t panel_hx8369_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    hx8369_panel_t *hx8369 = __containerof(panel, hx8369_panel_t, base);
    esp_lcd_panel_io_handle_t io = hx8369->io;
    int command = 0;
    if (on_off)
    {
        command = LCD_CMD_DISPON;
    }
    else
    {
        command = LCD_CMD_DISPOFF;
    }
    esp_lcd_panel_io_tx_param(io, command, NULL, 0);
    return ESP_OK;
}
// extern esp_err_t esp_lcd_new_panel_hx8369(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config, esp_lcd_panel_handle_t *ret_panel);
// extern esp_err_t panel_hx8369_disp_on_off(esp_lcd_panel_t *panel, bool on_off);
// static bool notify_lvgl_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
// {
//     lv_disp_drv_t *disp_driver = (lv_disp_drv_t *)user_ctx;
//     lv_disp_flush_ready(disp_driver);
//     return false;
// }

// void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
// {
//     esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;
//     int offsetx1 = area->x1;
//     int offsetx2 = area->x2;
//     int offsety1 = area->y1;
//     int offsety2 = area->y2;
//     // copy a buffer's content to a specific area of the display
//     esp_lcd_panel_draw_bitmap(panel_handle, offsetx1, offsety1, offsetx2 + 1, offsety2 + 1, color_map);
// }

// esp_lcd_panel_handle_t panel_handle;
// lv_disp_drv_t disp_drv; // contains callback functions
// void hx8369_init(void)
// {

//     ESP_LOGI(TAG, "Initialize Intel 8080 bus");
//     esp_lcd_i80_bus_handle_t i80_bus = NULL;
//     esp_lcd_i80_bus_config_t bus_config = {
//         //    .clk_src = LCD_CLK_SRC_DEFAULT,LCD_CLK_SRC_PLL160M
//         .clk_src = LCD_CLK_SRC_PLL160M,
//         .dc_gpio_num = PIN_NUM_DC,
//         .wr_gpio_num = PIN_NUM_PCLK,
//         .data_gpio_nums = {
//             PIN_NUM_DATA0,
//             PIN_NUM_DATA1,
//             PIN_NUM_DATA2,
//             PIN_NUM_DATA3,
//             PIN_NUM_DATA4,
//             PIN_NUM_DATA5,
//             PIN_NUM_DATA6,
//             PIN_NUM_DATA7,
//         },
//         .bus_width = 8,
//         .max_transfer_bytes = LCD_V_RES * 100 * sizeof(uint16_t),
//         //.max_transfer_bytes = DISP_BUF_SIZE,
//         .psram_trans_align = PSRAM_DATA_ALIGNMENT,
//         .sram_trans_align = 4,
//     };
//     ESP_ERROR_CHECK(esp_lcd_new_i80_bus(&bus_config, &i80_bus));
//     esp_lcd_panel_io_handle_t io_handle = NULL;
//     esp_lcd_panel_io_i80_config_t io_config = {
//         .cs_gpio_num = PIN_NUM_CS,
//         .pclk_hz = LCD_PIXEL_CLOCK_HZ,
//         .trans_queue_depth = 10,
//         .dc_levels = {
//             .dc_idle_level = 0,
//             .dc_cmd_level = 0,
//             .dc_dummy_level = 0,
//             .dc_data_level = 1,
//         },
//         .on_color_trans_done = notify_lvgl_flush_ready,
//         .user_ctx = &disp_drv,
//         .lcd_cmd_bits = LCD_CMD_BITS,
//         .lcd_param_bits = LCD_PARAM_BITS,
//         .flags.swap_color_bytes = true,
//     };
//     ESP_ERROR_CHECK(esp_lcd_new_panel_io_i80(i80_bus, &io_config, &io_handle));

//     // esp_lcd_panel_handle_t panel_handle = NULL;
//     panel_handle = NULL;

//     ESP_LOGI(TAG, "Install LCD driver of hx8369");
//     esp_lcd_panel_dev_config_t panel_config = {
//         .reset_gpio_num = PIN_NUM_RST,
//         .color_space = ESP_LCD_COLOR_SPACE_RGB,
//         .bits_per_pixel = 16,
//     };
//     ESP_ERROR_CHECK(esp_lcd_new_panel_hx8369(io_handle, &panel_config, &panel_handle));

//     esp_lcd_panel_reset(panel_handle);
//     esp_lcd_panel_init(panel_handle);

//     // Set inversion, x/y coordinate order, x/y mirror according to your LCD module spec
//     // esp_lcd_panel_invert_color(panel_handle, false);
//     esp_lcd_panel_swap_xy(panel_handle, true);
//     esp_lcd_panel_mirror(panel_handle, true, false);

//     // the gap is LCD panel specific, even panels with the same driver IC, can have different gap value
//     // esp_lcd_panel_set_gap(panel_handle, 0, 20);

//     // user can flush pre-defined pattern to the screen before we turn on the screen or backlight
//     // ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));panel_hx8369_disp_on_off
//     ESP_ERROR_CHECK(panel_hx8369_disp_on_off(panel_handle, true));
// }
