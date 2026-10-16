// SPDX-License-Identifier: GPL-3.0-or-later
//
// Bootloader hook (W-1046). The factory-reset check holds the bootloader for
// up to 5 s while the test button is down, and until app_main runs nothing
// drives the pins that must not float (docs/traps.md): IO48 floating feeds
// the LED chain noise, which can brownout-loop the board on a 500 mA port.
// So the bootloader clamps them first, to the levels app_main's first
// statements set. The numbers are the S3 board's: the bootloader build does
// not see main's Kconfig.
#include <stdbool.h>
#include "sdkconfig.h"
#include "esp_rom_gpio.h"
#include "hal/gpio_ll.h"
#include "soc/gpio_sig_map.h"

// The bootloader's link pulls this file in by this symbol; its hooks are weak.
void bootloader_hooks_include(void)
{
}

#if CONFIG_IDF_TARGET_ESP32S3
static void drive(uint32_t pin, uint32_t level)
{
    esp_rom_gpio_pad_select_gpio(pin);
    gpio_ll_set_level(&GPIO, pin, level);
    esp_rom_gpio_connect_out_signal(pin, SIG_GPIO_OUT_IDX, false, false);
    gpio_ll_output_enable(&GPIO, pin);
}
#endif

void bootloader_after_init(void)
{
#if CONFIG_IDF_TARGET_ESP32S3
    drive(48, 0);       // LED data into U9 (AIRWRITER_LED_DATA_PIN)
    drive(35, 0);       // SP3232 T1IN (AIRWRITER_HSKI_DRV_PIN)
    drive(4, 1);        // the PIC's RA1, TashTalk's UART receive: idle high (AIRWRITER_PIC_CLK_PIN)
#endif
}
