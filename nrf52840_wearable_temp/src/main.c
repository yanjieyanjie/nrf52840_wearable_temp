#include <zephyr/kernel.h>

#include "sensor_manager.h"
#include "display_manager.h"
#include "ble_service.h"
#include "config_manager.h"

#include <stdint.h>
#include <stdbool.h>


/* =========================================================
 * Reliability Configuration
 * =========================================================
 */

/*
 * 一次采样失败后，
 * 最多尝试 3 次。
 */
#define SENSOR_MAX_RETRIES       3


/*
 * 每次重试之间等待 100 ms。
 */
#define SENSOR_RETRY_DELAY_MS    100


/* =========================================================
 * Sensor Message
 * =========================================================
 *
 * Sensor Thread
 *      ↓
 * sensor_msgq
 *      ↓
 * Main / Application Thread
 * =========================================================
 */

struct sensor_message
{
    /*
     * 温度
     *
     * 单位：
     * 0.1°C
     *
     * 例如：
     *
     * 286 = 28.6°C
     */
    int16_t temperature;


    /*
     * 采样时间戳
     *
     * 单位：
     * ms
     *
     * 从系统启动开始计算。
     */
    uint32_t timestamp_ms;


    /*
     * 当前传感器状态。
     */
    bool sensor_ok;


    /*
     * 最后一次传感器错误码。
     *
     * 正常：
     * 0
     *
     * 异常示例：
     * -5
     */
    int error_code;
};


/* =========================================================
 * Display Message
 * =========================================================
 *
 * Main / Application Thread
 *      ↓
 * display_msgq
 *      ↓
 * Display Thread
 * =========================================================
 */

struct display_message
{
    int16_t temperature;

    bool ble_connected;

    bool sensor_ok;

    int sensor_error;
};


/* =========================================================
 * Message Queues
 * =========================================================
 */


/*
 * Sensor Thread
 *      ↓
 * Main Thread
 *
 * 最多缓存 4 条 Sensor Message。
 */
K_MSGQ_DEFINE(
    sensor_msgq,
    sizeof(struct sensor_message),
    4,
    4
);


/*
 * Main Thread
 *      ↓
 * Display Thread
 *
 * 最多缓存 4 条 Display Message。
 */
K_MSGQ_DEFINE(
    display_msgq,
    sizeof(struct display_message),
    4,
    4
);


/* =========================================================
 * Thread Configuration
 * =========================================================
 */


/*
 * Sensor Thread
 */
#define SENSOR_THREAD_STACK_SIZE 1536
#define SENSOR_THREAD_PRIORITY   5


/*
 * Display Thread
 *
 * Zephyr 中：
 *
 * 数字越小，
 * 优先级越高。
 *
 * 所以 Sensor Thread 5
 * 优先级高于 Display Thread 7。
 */
#define DISPLAY_THREAD_STACK_SIZE 1536
#define DISPLAY_THREAD_PRIORITY   7


/* =========================================================
 * Thread Stack
 * =========================================================
 */


K_THREAD_STACK_DEFINE(
    sensor_thread_stack,
    SENSOR_THREAD_STACK_SIZE
);


K_THREAD_STACK_DEFINE(
    display_thread_stack,
    DISPLAY_THREAD_STACK_SIZE
);


/* =========================================================
 * Thread Control Blocks
 * =========================================================
 */


static struct k_thread sensor_thread_data;

static struct k_thread display_thread_data;


/* =========================================================
 * Sensor Thread
 * =========================================================
 *
 * 职责：
 *
 * 1. 读取 DS18B20
 * 2. 读取失败自动重试
 * 3. 判断 Sensor Fault
 * 4. 判断 Sensor Recovery
 * 5. 把结果发送给 Main Thread
 * 6. 根据当前配置进入 sleep
 *
 * Sensor Thread 不直接操作：
 *
 * OLED
 * BLE Notification
 * =========================================================
 */


static void sensor_thread_entry(
    void *p1,
    void *p2,
    void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);


    /*
     * 初始化为 0，
     * 防止第一次读取就失败时 temperature
     * 中存在未初始化数据。
     */
    struct sensor_message msg = {0};


    /*
     * 连续失败周期计数。
     */
    uint32_t consecutive_failures = 0;


    /*
     * 用于判断：
     *
     * FAULT
     *   ↓
     * RECOVERED
     */
    bool sensor_was_faulted = false;


    int ret;


    printk(
        "SENSOR_THREAD: started\n"
    );


    while (1) {

        bool read_success = false;

        int last_error = 0;


        /* =================================================
         * 1. Sensor Read + Retry
         * =================================================
         */


        for (
            int attempt = 1;
            attempt <= SENSOR_MAX_RETRIES;
            attempt++
        ) {

            ret =
                sensor_manager_read_temperature(
                    &msg.temperature
                );


            /*
             * 读取成功。
             */
            if (ret == 0) {

                read_success = true;

                break;
            }


            /*
             * 保存最后一个错误码。
             */
            last_error = ret;


            printk(
                "SENSOR_THREAD: "
                "read failed attempt %d/%d, err=%d\n",

                attempt,
                SENSOR_MAX_RETRIES,
                ret
            );


            /*
             * 如果还有下一次重试，
             * 等待 100 ms。
             */
            if (
                attempt <
                SENSOR_MAX_RETRIES
            ) {

                k_sleep(
                    K_MSEC(
                        SENSOR_RETRY_DELAY_MS
                    )
                );
            }
        }


        /* =================================================
         * 2. Sensor 正常
         * =================================================
         */


        if (read_success) {

            msg.sensor_ok = true;

            msg.error_code = 0;


            msg.timestamp_ms =
                k_uptime_get_32();


            /*
             * 如果上一周期处于 Fault，
             * 现在读取成功，
             * 就认为 Sensor 已恢复。
             */
            if (sensor_was_faulted) {

                printk(
                    "SENSOR_THREAD: "
                    "SENSOR RECOVERED "
                    "after %u failed cycles\n",

                    consecutive_failures
                );


                sensor_was_faulted = false;
            }


            /*
             * 恢复以后清零连续失败计数。
             */
            consecutive_failures = 0;


            printk(
                "SENSOR_THREAD: "
                "temp=%d timestamp=%u\n",

                msg.temperature,
                msg.timestamp_ms
            );

        } else {

            /* =============================================
             * 3. Sensor Fault
             * =============================================
             *
             * 连续 3 次读取都失败。
             */


            consecutive_failures++;


            sensor_was_faulted = true;


            msg.sensor_ok = false;

            msg.error_code =
                last_error;


            msg.timestamp_ms =
                k_uptime_get_32();


            printk(
                "SENSOR_THREAD: "
                "SENSOR FAULT "
                "cycle=%u error=%d\n",

                consecutive_failures,
                last_error
            );
        }


        /* =================================================
         * 4. Send Sensor Message
         * =================================================
         */


        ret =
            k_msgq_put(
                &sensor_msgq,
                &msg,
                K_NO_WAIT
            );


        if (ret != 0) {

            /*
             * Main Thread 消费速度过慢，
             * Queue 已满。
             *
             * Sensor Thread 不阻塞。
             */
            printk(
                "SENSOR_THREAD: "
                "sensor queue full, "
                "message dropped\n"
            );
        }


        /* =================================================
         * 5. 获取当前 Sampling Interval
         * =================================================
         *
         * 现在不再从 ble_service 获取。
         *
         * Sampling Interval 属于：
         *
         * config_manager
         *
         * 它可能来自：
         *
         * 默认值
         * 或
         * Flash 中恢复的值
         * 或
         * 手机 BLE WRITE 的新值
         */


        uint16_t interval =
            config_manager_get_sampling_interval_ms();


        printk(
            "SENSOR_THREAD: "
            "next cycle in %u ms\n",

            interval
        );


        /* =================================================
         * 6. Sleep
         * =================================================
         */


        k_sleep(
            K_MSEC(interval)
        );
    }
}


/* =========================================================
 * Display Thread
 * =========================================================
 *
 * Display Thread 唯一职责：
 *
 * OLED 更新。
 *
 * 没有消息时：
 *
 * BLOCKED
 *
 * 不轮询。
 * =========================================================
 */


static void display_thread_entry(
    void *p1,
    void *p2,
    void *p3)
{
    ARG_UNUSED(p1);
    ARG_UNUSED(p2);
    ARG_UNUSED(p3);


    struct display_message msg;


    int ret;


    printk(
        "DISPLAY_THREAD: started\n"
    );


    while (1) {

        /* =================================================
         * 1. 等待 Display Message
         * =================================================
         */


        ret =
            k_msgq_get(
                &display_msgq,
                &msg,
                K_FOREVER
            );


        if (ret != 0) {

            printk(
                "DISPLAY_THREAD: "
                "queue receive failed: %d\n",

                ret
            );


            continue;
        }


        /* =================================================
         * 2. OLED Update
         * =================================================
         */


        ret =
            display_manager_update(
                msg.temperature,
                msg.ble_connected,
                msg.sensor_ok,
                msg.sensor_error
            );


        if (ret != 0) {

            printk(
                "DISPLAY_THREAD: "
                "update failed: %d\n",

                ret
            );


            continue;
        }


        /* =================================================
         * 3. Debug Log
         * =================================================
         */


        if (msg.sensor_ok) {

            printk(
                "DISPLAY_THREAD: "
                "temp=%d BLE=%s\n",

                msg.temperature,

                msg.ble_connected
                    ? "CONNECTED"
                    : "ADV"
            );

        } else {

            printk(
                "DISPLAY_THREAD: "
                "SENSOR ERROR=%d\n",

                msg.sensor_error
            );
        }
    }
}


/* =========================================================
 * main
 * =========================================================
 */


int main(void)
{
    int ret;


    struct sensor_message sensor_msg;

    struct display_message display_msg;


    printk("\n");

    printk(
        "================================\n"
    );

    printk(
        " Wearable Temperature Monitor\n"
    );

    printk(
        " Reliable RTOS + NVS Version\n"
    );

    printk(
        "================================\n"
    );


    /* =====================================================
     * 1. Sensor Manager Init
     * =====================================================
     */


    ret =
        sensor_manager_init();


    if (ret != 0) {

        printk(
            "APP: sensor init failed: %d\n",
            ret
        );


        return 0;
    }


    /* =====================================================
     * 2. Display Manager Init
     * =====================================================
     */


    ret =
        display_manager_init();


    if (ret != 0) {

        printk(
            "APP: display init failed: %d\n",
            ret
        );


        return 0;
    }


    /* =====================================================
     * 3. Config Manager Init
     * =====================================================
     *
     * 这里非常关键。
     *
     * Config Manager 会：
     *
     * 1. 初始化 Settings
     * 2. 初始化 NVS Backend
     * 3. 尝试从 Flash 恢复 Sampling Interval
     *
     * 如果 Flash 没有保存数据：
     *
     * 默认 = 2000 ms
     */


    ret =
        config_manager_init();


    if (ret != 0) {

        printk(
            "APP: config manager init failed: %d\n",
            ret
        );


        return 0;
    }


    printk(
        "APP: sampling interval=%u ms\n",

        config_manager_get_sampling_interval_ms()
    );


    /* =====================================================
     * 4. BLE Service Init
     * =====================================================
     */


    ret =
        ble_service_init();


    if (ret != 0) {

        printk(
            "APP: BLE init failed: %d\n",
            ret
        );


        return 0;
    }


    printk(
        "APP: initialization completed\n"
    );


    /* =====================================================
     * 5. Create Display Thread
     * =====================================================
     */


    k_thread_create(
        &display_thread_data,

        display_thread_stack,

        K_THREAD_STACK_SIZEOF(
            display_thread_stack
        ),

        display_thread_entry,

        NULL,
        NULL,
        NULL,

        DISPLAY_THREAD_PRIORITY,

        0,

        K_NO_WAIT
    );


    printk(
        "APP: display thread created\n"
    );


    /* =====================================================
     * 6. Create Sensor Thread
     * =====================================================
     */


    k_thread_create(
        &sensor_thread_data,

        sensor_thread_stack,

        K_THREAD_STACK_SIZEOF(
            sensor_thread_stack
        ),

        sensor_thread_entry,

        NULL,
        NULL,
        NULL,

        SENSOR_THREAD_PRIORITY,

        0,

        K_NO_WAIT
    );


    printk(
        "APP: sensor thread created\n"
    );


    printk(
        "APP: threads started\n"
    );


    /* =====================================================
     * Main / Application Thread
     * =====================================================
     *
     * Main Thread 现在负责：
     *
     * Sensor Message
     *       ↓
     * 数据判断
     *       ↓
     * BLE
     *       ↓
     * Display Message
     *
     * 它不直接读取 Sensor，
     * 也不直接操作 OLED。
     */


    while (1) {

        /* =================================================
         * 1. 等待 Sensor Message
         * =================================================
         *
         * Queue 没消息时，
         * Main Thread BLOCKED。
         */


        ret =
            k_msgq_get(
                &sensor_msgq,
                &sensor_msg,
                K_FOREVER
            );


        if (ret != 0) {

            printk(
                "APP: "
                "sensor queue receive failed: %d\n",

                ret
            );


            continue;
        }


        /* =================================================
         * 2. Sensor 正常
         * =================================================
         */


        if (sensor_msg.sensor_ok) {

            printk(
                "APP: valid temperature=%d\n",
                sensor_msg.temperature
            );


            /* =============================================
             * 更新 GATT Temperature Value
             * =============================================
             */


            ble_service_set_temperature(
                sensor_msg.temperature
            );


            /* =============================================
             * Temperature Notification
             * =============================================
             *
             * 没连接：
             * 什么都不做。
             *
             * 没订阅：
             * 什么都不做。
             *
             * 已连接 + 已订阅：
             * Notification。
             */


            ret =
                ble_service_notify_temperature();


            if (ret != 0) {

                printk(
                    "APP: "
                    "BLE notify failed: %d\n",

                    ret
                );
            }

        } else {

            /* =============================================
             * 3. Sensor Fault
             * =============================================
             *
             * 不把无效 Sensor 数据发送给 BLE。
             *
             * GATT 中仍保留上一次合法温度。
             */


            printk(
                "APP: sensor fault error=%d, "
                "BLE temperature update skipped\n",

                sensor_msg.error_code
            );
        }


        /* =================================================
         * 4. Build Display Message
         * =================================================
         */


        display_msg.temperature =
            sensor_msg.temperature;


        display_msg.ble_connected =
            ble_service_is_connected();


        display_msg.sensor_ok =
            sensor_msg.sensor_ok;


        display_msg.sensor_error =
            sensor_msg.error_code;


        /* =================================================
         * 5. Send Display Message
         * =================================================
         */


        ret =
            k_msgq_put(
                &display_msgq,
                &display_msg,
                K_NO_WAIT
            );


        /* =================================================
         * Display Queue Full
         * =================================================
         *
         * OLED 属于“状态型数据”。
         *
         * 我们只关心最新状态。
         *
         * 如果旧画面堆积：
         *
         * 删除旧数据
         * ↓
         * 保留最新状态
         */


        if (ret != 0) {

            printk(
                "APP: display queue full, "
                "dropping stale frames\n"
            );


            k_msgq_purge(
                &display_msgq
            );


            ret =
                k_msgq_put(
                    &display_msgq,
                    &display_msg,
                    K_NO_WAIT
                );


            if (ret != 0) {

                printk(
                    "APP: "
                    "display queue put failed: %d\n",

                    ret
                );
            }
        }
    }


    return 0;
}