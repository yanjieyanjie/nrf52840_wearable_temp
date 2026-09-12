#include "display_manager.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/display/cfb.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>


static const struct device *display;


/* =========================================================
 * Display Init
 * =========================================================
 */

int display_manager_init(void)
{
    int ret;


    display =
        DEVICE_DT_GET(
            DT_CHOSEN(zephyr_display)
        );


    if (!device_is_ready(display)) {

        printk(
            "DISPLAY: OLED not ready\n"
        );

        return -ENODEV;
    }


    ret =
        display_set_pixel_format(
            display,
            PIXEL_FORMAT_MONO10
        );


    if (ret != 0) {

        ret =
            display_set_pixel_format(
                display,
                PIXEL_FORMAT_MONO01
            );


        if (ret != 0) {

            printk(
                "DISPLAY: pixel format failed\n"
            );

            return ret;
        }
    }


    ret =
        cfb_framebuffer_init(
            display
        );


    if (ret != 0) {

        printk(
            "DISPLAY: CFB init failed: %d\n",
            ret
        );

        return ret;
    }


    ret =
        cfb_framebuffer_set_font(
            display,
            0
        );


    if (ret != 0) {

        printk(
            "DISPLAY: set font failed: %d\n",
            ret
        );

        return ret;
    }


    cfb_framebuffer_clear(
        display,
        true
    );


    ret =
        display_blanking_off(
            display
        );


    if (ret != 0) {

        printk(
            "DISPLAY: blanking off failed: %d\n",
            ret
        );

        return ret;
    }


    printk(
        "DISPLAY: ready\n"
    );


    return 0;
}


/* =========================================================
 * Display Update
 * =========================================================
 */

int display_manager_update(
    int16_t temp_tenths,
    bool ble_connected,
    bool sensor_ok,
    int sensor_error)
{
    char temp_text[32];

    char sensor_text[32];

    int ret;


    /*
     * 清 framebuffer
     */
    ret =
        cfb_framebuffer_clear(
            display,
            false
        );


    if (ret != 0) {
        return ret;
    }


    /* =====================================================
     * 第一行
     * =====================================================
     */

    cfb_print(
        display,
        "WEARABLE TEMP",
        0,
        0
    );


    /* =====================================================
     * 第二行
     *
     * Sensor 正常：
     * TEMP: 28.6 C
     *
     * Sensor 异常：
     * SENSOR ERROR
     * =====================================================
     */

    if (sensor_ok) {

        int32_t temp =
            temp_tenths;


        const char *sign =
            temp < 0
                ? "-"
                : "";


        int32_t absolute_temp =
            temp < 0
                ? -temp
                : temp;


        snprintf(
            temp_text,
            sizeof(temp_text),

            "TEMP: %s%d.%d C",

            sign,

            (int)(
                absolute_temp / 10
            ),

            (int)(
                absolute_temp % 10
            )
        );


        cfb_print(
            display,
            temp_text,
            0,
            20
        );

    } else {

        cfb_print(
            display,
            "SENSOR ERROR",
            0,
            20
        );
    }


    /* =====================================================
     * 第三行
     * =====================================================
     */

    if (sensor_ok) {

        if (ble_connected) {

            cfb_print(
                display,
                "BLE: CONNECTED",
                0,
                40
            );

        } else {

            cfb_print(
                display,
                "BLE: ADV",
                0,
                40
            );
        }

    } else {

        snprintf(
            sensor_text,
            sizeof(sensor_text),

            "ERR: %d",

            sensor_error
        );


        cfb_print(
            display,
            sensor_text,
            0,
            40
        );
    }


    return
        cfb_framebuffer_finalize(
            display
        );
}