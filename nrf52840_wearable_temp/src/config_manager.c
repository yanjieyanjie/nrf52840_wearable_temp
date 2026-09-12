#include "config_manager.h"

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

#include <errno.h>
#include <stdint.h>
#include <stdbool.h>


/*
 * Flash 中使用的 Settings Key
 *
 * 最终类似：
 *
 * wearable/sampling_interval
 */
#define SETTINGS_KEY_INTERVAL \
    "wearable/sampling_interval"


/*
 * 用户停止修改参数 1 秒后，
 * 再真正写 Flash。
 */
#define SETTINGS_SAVE_DELAY_MS 1000


/*
 * 当前运行时采样周期。
 *
 * Sensor Thread 和 BLE callback
 * 都可能访问，所以使用 atomic。
 */
static atomic_t sampling_interval_ms;


/*
 * Settings 是否已经成功初始化。
 */
static bool settings_ready = false;


/* =========================================================
 * Delayed Flash Save
 * =========================================================
 */

static void save_work_handler(
    struct k_work *work)
{
    ARG_UNUSED(work);


    if (!settings_ready) {

        printk(
            "CONFIG: settings not ready\n"
        );

        return;
    }


    uint16_t interval =
        (uint16_t)atomic_get(
            &sampling_interval_ms
        );


    /*
     * Flash 中明确使用 Little Endian。
     */
    uint16_t stored_value =
        sys_cpu_to_le16(
            interval
        );


    int ret =
        settings_save_one(
            SETTINGS_KEY_INTERVAL,

            &stored_value,

            sizeof(stored_value)
        );


    if (ret != 0) {

        printk(
            "CONFIG: save failed: %d\n",
            ret
        );

        return;
    }


    printk(
        "CONFIG: persisted interval=%u ms\n",
        interval
    );
}


K_WORK_DELAYABLE_DEFINE(
    config_save_work,
    save_work_handler
);


/* =========================================================
 * Config Init
 * =========================================================
 */

int config_manager_init(void)
{
    int ret;


    /*
     * 先设置默认值。
     *
     * 如果 Flash 没有保存配置，
     * 就继续使用 2000 ms。
     */
    atomic_set(
        &sampling_interval_ms,
        CONFIG_INTERVAL_DEFAULT_MS
    );


    /*
     * 初始化 Settings + NVS backend。
     */
    ret =
        settings_subsys_init();


    if (ret != 0) {

        printk(
            "CONFIG: settings init failed: %d\n",
            ret
        );

        return ret;
    }


    settings_ready = true;


    /*
     * 尝试从 Flash 加载。
     */
    uint16_t stored_value = 0;


    ssize_t len =
        settings_load_one(
            SETTINGS_KEY_INTERVAL,

            &stored_value,

            sizeof(stored_value)
        );


    /*
     * 找到了正确长度的数据。
     */
    if (len == sizeof(stored_value)) {

        uint16_t loaded_interval =
            sys_le16_to_cpu(
                stored_value
            );


        /*
         * Flash 数据也必须进行合法性检查。
         *
         * 不能因为 Flash 中有数据，
         * 就无条件相信。
         */
        if (
            loaded_interval >=
                CONFIG_INTERVAL_MIN_MS

            &&

            loaded_interval <=
                CONFIG_INTERVAL_MAX_MS
        ) {

            atomic_set(
                &sampling_interval_ms,
                loaded_interval
            );


            printk(
                "CONFIG: restored interval=%u ms\n",
                loaded_interval
            );


            return 0;
        }


        /*
         * Flash 里有数据，
         * 但是数值非法。
         */
        printk(
            "CONFIG: invalid stored interval=%u, "
            "using default=%u ms\n",

            loaded_interval,
            CONFIG_INTERVAL_DEFAULT_MS
        );


        return 0;
    }


    /*
     * 第一次运行通常没有保存值。
     */
    if (
        len == 0 ||
        len == -ENOENT
    ) {

        printk(
            "CONFIG: no saved interval, "
            "using default=%u ms\n",

            CONFIG_INTERVAL_DEFAULT_MS
        );


        return 0;
    }


    /*
     * 长度不对。
     *
     * 例如以后配置结构发生变化。
     */
    if (len > 0) {

        printk(
            "CONFIG: stored interval size invalid: %d, "
            "using default\n",

            (int)len
        );


        return 0;
    }


    /*
     * 真正的 Settings backend 错误。
     */
    printk(
        "CONFIG: load failed: %d\n",
        (int)len
    );


    return (int)len;
}


/* =========================================================
 * Get Sampling Interval
 * =========================================================
 */

uint16_t
config_manager_get_sampling_interval_ms(void)
{
    return
        (uint16_t)atomic_get(
            &sampling_interval_ms
        );
}


/* =========================================================
 * Set Sampling Interval
 * =========================================================
 */

int config_manager_set_sampling_interval_ms(
    uint16_t interval_ms)
{
    /*
     * 参数检查。
     */
    if (
        interval_ms <
            CONFIG_INTERVAL_MIN_MS

        ||

        interval_ms >
            CONFIG_INTERVAL_MAX_MS
    ) {

        return -ERANGE;
    }


    uint16_t current =
        config_manager_get_sampling_interval_ms();


    /*
     * 没发生变化，
     * 什么都不用做。
     */
    if (current == interval_ms) {

        return 0;
    }


    /*
     * RAM 中立即修改。
     *
     * Sensor Thread 下一轮马上会使用新值。
     */
    atomic_set(
        &sampling_interval_ms,
        interval_ms
    );


    printk(
        "CONFIG: runtime interval=%u ms\n",
        interval_ms
    );


    /*
     * 不直接写 Flash。
     *
     * 延迟 1 秒。
     *
     * 如果 1 秒内又修改一次，
     * k_work_reschedule 会重新计时，
     * 最终只保存最新值。
     */
    k_work_reschedule(
        &config_save_work,
        K_MSEC(
            SETTINGS_SAVE_DELAY_MS
        )
    );


    return 0;
}