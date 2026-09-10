#include <zephyr/kernel.h>
#include <zephyr/device.h>

#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/display.h>
#include <zephyr/display/cfb.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>

#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>


/* =========================================================
 * BLE UUID
 * =========================================================
 *
 * Service:
 * 8e400001-f315-4f60-9fb8-838830daea50
 *
 * Temperature:
 * 8e400002-f315-4f60-9fb8-838830daea50
 *
 * Sampling Interval:
 * 8e400003-f315-4f60-9fb8-838830daea50
 */

#define BT_UUID_WEARABLE_SERVICE_VAL \
    BT_UUID_128_ENCODE(0x8e400001, 0xf315, 0x4f60, 0x9fb8, 0x838830daea50)

#define BT_UUID_WEARABLE_TEMP_VAL \
    BT_UUID_128_ENCODE(0x8e400002, 0xf315, 0x4f60, 0x9fb8, 0x838830daea50)

#define BT_UUID_WEARABLE_INTERVAL_VAL \
    BT_UUID_128_ENCODE(0x8e400003, 0xf315, 0x4f60, 0x9fb8, 0x838830daea50)


static struct bt_uuid_128 wearable_service_uuid =
    BT_UUID_INIT_128(BT_UUID_WEARABLE_SERVICE_VAL);

static struct bt_uuid_128 wearable_temp_uuid =
    BT_UUID_INIT_128(BT_UUID_WEARABLE_TEMP_VAL);

static struct bt_uuid_128 wearable_interval_uuid =
    BT_UUID_INIT_128(BT_UUID_WEARABLE_INTERVAL_VAL);


/* =========================================================
 * 系统状态
 * =========================================================
 */

/*
 * 当前温度
 *
 * 单位：0.1°C
 *
 * 例如：
 * 278 = 27.8°C
 */
static volatile int16_t current_temp_tenths = 0;


/*
 * BLE 是否已经连接
 */
static volatile bool ble_connected = false;


/*
 * 手机是否打开 Temperature Notification
 */
static volatile bool notify_enabled = false;


/*
 * 温度采样周期
 *
 * 单位：ms
 *
 * 默认：
 * 2000 ms = 2 秒
 */
static volatile uint16_t sampling_interval_ms = 2000;


/* =========================================================
 * Temperature READ
 * =========================================================
 */

static ssize_t read_temperature(
    struct bt_conn *conn,
    const struct bt_gatt_attr *attr,
    void *buf,
    uint16_t len,
    uint16_t offset)
{
    /*
     * BLE 中发送 uint16/int16
     * 使用 Little Endian
     */
    int16_t value =
        sys_cpu_to_le16(current_temp_tenths);


    printk(
        "GATT READ temperature: %d.%d C\n",
        current_temp_tenths / 10,
        abs(current_temp_tenths % 10)
    );


    return bt_gatt_attr_read(
        conn,
        attr,
        buf,
        len,
        offset,
        &value,
        sizeof(value)
    );
}


/* =========================================================
 * Notification CCCD
 * =========================================================
 */

static void temp_ccc_cfg_changed(
    const struct bt_gatt_attr *attr,
    uint16_t value)
{
    notify_enabled =
        (value == BT_GATT_CCC_NOTIFY);


    if (notify_enabled) {

        printk(
            "Temperature notification ENABLED\n"
        );

    } else {

        printk(
            "Temperature notification DISABLED\n"
        );
    }
}


/* =========================================================
 * Sampling Interval READ
 * =========================================================
 */

static ssize_t read_sampling_interval(
    struct bt_conn *conn,
    const struct bt_gatt_attr *attr,
    void *buf,
    uint16_t len,
    uint16_t offset)
{
    uint16_t value =
        sys_cpu_to_le16(sampling_interval_ms);


    printk(
        "GATT READ interval: %u ms\n",
        sampling_interval_ms
    );


    return bt_gatt_attr_read(
        conn,
        attr,
        buf,
        len,
        offset,
        &value,
        sizeof(value)
    );
}


/* =========================================================
 * Sampling Interval WRITE
 * =========================================================
 */

static ssize_t write_sampling_interval(
    struct bt_conn *conn,
    const struct bt_gatt_attr *attr,
    const void *buf,
    uint16_t len,
    uint16_t offset,
    uint8_t flags)
{
    /*
     * 不允许 offset write
     */
    if (offset != 0) {

        return BT_GATT_ERR(
            BT_ATT_ERR_INVALID_OFFSET
        );
    }


    /*
     * Sampling Interval
     * 使用 uint16_t
     *
     * 所以必须收到 2 Bytes
     */
    if (len != sizeof(uint16_t)) {

        return BT_GATT_ERR(
            BT_ATT_ERR_INVALID_ATTRIBUTE_LEN
        );
    }


    /*
     * BLE：
     * Little Endian → CPU uint16_t
     */
    uint16_t new_interval =
        sys_get_le16(buf);


    /*
     * 合法范围：
     *
     * 最短：1000 ms
     * 最长：10000 ms
     */
    if (new_interval < 1000 ||
        new_interval > 10000) {

        printk(
            "Invalid interval: %u ms\n",
            new_interval
        );

        return BT_GATT_ERR(
            BT_ATT_ERR_VALUE_NOT_ALLOWED
        );
    }


    /*
     * 更新采样周期
     */
    sampling_interval_ms =
        new_interval;


    printk(
        "Sampling interval changed to %u ms\n",
        sampling_interval_ms
    );


    return len;
}


/* =========================================================
 * 自定义 GATT Service
 * =========================================================
 */

BT_GATT_SERVICE_DEFINE(
    wearable_service,

    /*
     * Attribute 0
     *
     * Primary Service
     */
    BT_GATT_PRIMARY_SERVICE(
        &wearable_service_uuid
    ),


    /*
     * Attribute 1：
     * Temperature Characteristic Declaration
     *
     * Attribute 2：
     * Temperature Value
     */
    BT_GATT_CHARACTERISTIC(
        &wearable_temp_uuid.uuid,

        BT_GATT_CHRC_READ |
        BT_GATT_CHRC_NOTIFY,

        BT_GATT_PERM_READ,

        read_temperature,

        NULL,

        NULL
    ),


    /*
     * Attribute 3
     *
     * CCCD
     *
     * 手机通过它打开/关闭 Notification
     */
    BT_GATT_CCC(
        temp_ccc_cfg_changed,

        BT_GATT_PERM_READ |
        BT_GATT_PERM_WRITE
    ),


    /*
     * Attribute 4：
     * Sampling Interval Characteristic Declaration
     *
     * Attribute 5：
     * Sampling Interval Value
     */
    BT_GATT_CHARACTERISTIC(
        &wearable_interval_uuid.uuid,

        BT_GATT_CHRC_READ |
        BT_GATT_CHRC_WRITE,

        BT_GATT_PERM_READ |
        BT_GATT_PERM_WRITE,

        read_sampling_interval,

        write_sampling_interval,

        NULL
    )
);


/* =========================================================
 * Advertising Data
 * =========================================================
 */

/*
 * Advertising Packet：
 *
 * Flags
 * +
 * 128-bit Service UUID
 */
static const struct bt_data ad[] = {

    BT_DATA_BYTES(
        BT_DATA_FLAGS,

        BT_LE_AD_GENERAL |
        BT_LE_AD_NO_BREDR
    ),


    BT_DATA_BYTES(
        BT_DATA_UUID128_ALL,

        BT_UUID_WEARABLE_SERVICE_VAL
    ),
};


/*
 * Scan Response：
 *
 * Complete Local Name
 *
 * WearableTemp
 */
static const struct bt_data sd[] = {

    BT_DATA(
        BT_DATA_NAME_COMPLETE,

        CONFIG_BT_DEVICE_NAME,

        sizeof(CONFIG_BT_DEVICE_NAME) - 1
    ),
};


/* =========================================================
 * 启动 BLE Advertising
 * =========================================================
 */

static int start_advertising(void)
{
    int ret;


    ret = bt_le_adv_start(
        BT_LE_ADV_CONN_FAST_1,

        ad,
        ARRAY_SIZE(ad),

        sd,
        ARRAY_SIZE(sd)
    );


    if (ret != 0) {

        printk(
            "Advertising failed: %d\n",
            ret
        );

        return ret;
    }


    printk(
        "Advertising started: %s\n",
        CONFIG_BT_DEVICE_NAME
    );


    return 0;
}


/* =========================================================
 * BLE Connected Callback
 * =========================================================
 */

static void connected(
    struct bt_conn *conn,
    uint8_t err)
{
    if (err != 0) {

        printk(
            "BLE connection failed: %u\n",
            err
        );

        return;
    }


    ble_connected = true;


    printk(
        "BLE CONNECTED\n"
    );
}


/* =========================================================
 * BLE Disconnected Callback
 * =========================================================
 */

static void disconnected(
    struct bt_conn *conn,
    uint8_t reason)
{
    ble_connected = false;

    notify_enabled = false;


    printk(
        "BLE DISCONNECTED, reason: 0x%02X\n",
        reason
    );


    /*
     * BLE 断开之后
     * 自动重新开始广播
     */
    int ret = start_advertising();


    if (ret != 0) {

        printk(
            "Restart advertising failed: %d\n",
            ret
        );
    }
}


/*
 * 注册 BLE Connection Callback
 */
BT_CONN_CB_DEFINE(conn_callbacks) = {

    .connected =
        connected,

    .disconnected =
        disconnected,
};


/* =========================================================
 * main
 * =========================================================
 */

int main(void)
{
    /*
     * 获取 DS18B20
     */
    const struct device *sensor =
        DEVICE_DT_GET_ANY(maxim_ds18b20);


    /*
     * 获取 OLED
     */
    const struct device *display =
        DEVICE_DT_GET(
            DT_CHOSEN(zephyr_display)
        );


    struct sensor_value temp;


    char temp_text[32];


    int ret;


    printk("\n");

    printk(
        "================================\n"
    );

    printk(
        " Wearable Temperature Monitor\n"
    );

    printk(
        "================================\n"
    );


    /* =====================================================
     * DS18B20 初始化
     * =====================================================
     */

    if (sensor == NULL) {

        printk(
            "ERROR: DS18B20 not found\n"
        );

        return 0;
    }


    if (!device_is_ready(sensor)) {

        printk(
            "ERROR: DS18B20 not ready\n"
        );

        return 0;
    }


    printk(
        "DS18B20 READY\n"
    );


    /* =====================================================
     * OLED 初始化
     * =====================================================
     */

    if (!device_is_ready(display)) {

        printk(
            "ERROR: OLED not ready\n"
        );

        return 0;
    }


    printk(
        "OLED READY\n"
    );


    /*
     * 设置 OLED pixel format
     */
    ret = display_set_pixel_format(
        display,
        PIXEL_FORMAT_MONO10
    );


    if (ret != 0) {

        /*
         * 如果 MONO10 不支持
         * 尝试 MONO01
         */
        ret = display_set_pixel_format(
            display,
            PIXEL_FORMAT_MONO01
        );


        if (ret != 0) {

            printk(
                "ERROR: pixel format failed\n"
            );

            return 0;
        }
    }


    /*
     * 初始化 Character Framebuffer
     */
    ret = cfb_framebuffer_init(
        display
    );


    if (ret != 0) {

        printk(
            "ERROR: CFB init failed: %d\n",
            ret
        );

        return 0;
    }


    printk(
        "CFB READY\n"
    );


    /*
     * 使用 Zephyr 内置第 0 个字体
     */
    ret = cfb_framebuffer_set_font(
        display,
        0
    );


    if (ret != 0) {

        printk(
            "Set font failed: %d\n",
            ret
        );
    }


    /*
     * 清屏
     */
    cfb_framebuffer_clear(
        display,
        true
    );


    /*
     * 打开 OLED
     */
    ret = display_blanking_off(
        display
    );


    if (ret != 0) {

        printk(
            "display_blanking_off failed: %d\n",
            ret
        );
    }


    /* =====================================================
     * Bluetooth 初始化
     * =====================================================
     */

    ret = bt_enable(NULL);


    if (ret != 0) {

        printk(
            "ERROR: Bluetooth init failed: %d\n",
            ret
        );

        return 0;
    }


    printk(
        "Bluetooth READY\n"
    );


    /* =====================================================
     * BLE Advertising
     * =====================================================
     */

    ret = start_advertising();


    if (ret != 0) {

        return 0;
    }


    /* =====================================================
     * 主循环
     * =====================================================
     */

    while (1) {

        /*
         * ===============================================
         * 1. DS18B20 获取新的温度样本
         * ===============================================
         */

        ret = sensor_sample_fetch(
            sensor
        );


        if (ret != 0) {

            printk(
                "sensor_sample_fetch failed: %d\n",
                ret
            );


            /*
             * 如果读取失败
             * 等一下再重新尝试
             */
            k_sleep(
                K_MSEC(sampling_interval_ms)
            );

            continue;
        }


        /*
         * 获取实际温度
         */
        ret = sensor_channel_get(
            sensor,

            SENSOR_CHAN_AMBIENT_TEMP,

            &temp
        );


        if (ret != 0) {

            printk(
                "sensor_channel_get failed: %d\n",
                ret
            );


            k_sleep(
                K_MSEC(sampling_interval_ms)
            );

            continue;
        }


        /*
         * ===============================================
         * 2. 转换温度
         * ===============================================
         *
         * sensor_value:
         *
         * val1 = 整数部分
         * val2 = 1 / 1000000
         */


        int64_t temp_micro =
            ((int64_t)temp.val1 * 1000000LL)
            +
            temp.val2;


        /*
         * 转成 0.1°C
         *
         * 例如：
         *
         * 29.0°C
         *
         * →
         *
         * 290
         */
        current_temp_tenths =
            (int16_t)(
                temp_micro /
                100000LL
            );


        /*
         * ===============================================
         * 3. 转成 OLED 字符串
         * ===============================================
         */

        snprintf(
            temp_text,

            sizeof(temp_text),

            "TEMP: %d.%d C",

            current_temp_tenths / 10,

            abs(
                current_temp_tenths % 10
            )
        );


        /*
         * 串口输出
         */
        printk(
            "%s\n",
            temp_text
        );


        /* =================================================
         * 4. BLE Notification
         * =================================================
         */

        if (ble_connected &&
            notify_enabled) {

            /*
             * CPU格式
             * →
             * BLE Little Endian
             */
            int16_t notify_value =
                sys_cpu_to_le16(
                    current_temp_tenths
                );


            /*
             * wearable_service.attrs[2]
             *
             * 就是 Temperature Value Attribute
             */
            ret = bt_gatt_notify(
                NULL,

                &wearable_service.attrs[2],

                &notify_value,

                sizeof(notify_value)
            );


            if (ret == 0) {

                printk(
                    "NOTIFY temperature: %d.%d C\n",

                    current_temp_tenths / 10,

                    abs(
                        current_temp_tenths % 10
                    )
                );

            } else {

                printk(
                    "Notification failed: %d\n",
                    ret
                );
            }
        }


        /* =================================================
         * 5. OLED 刷新
         * =================================================
         */

        cfb_framebuffer_clear(
            display,
            false
        );


        /*
         * 第一行
         */
        cfb_print(
            display,

            "WEARABLE TEMP",

            0,
            0
        );


        /*
         * 第二行
         */
        cfb_print(
            display,

            temp_text,

            0,
            20
        );


        /*
         * 第三行 BLE 状态
         */
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


        /*
         * 真正刷新 OLED
         */
        ret = cfb_framebuffer_finalize(
            display
        );


        if (ret != 0) {

            printk(
                "OLED refresh failed: %d\n",
                ret
            );
        }


        /* =================================================
         * 6. 等待下一次采样
         * =================================================
         *
         * 这个值现在可以通过 BLE WRITE 动态修改
         */

        k_sleep(
            K_MSEC(
                sampling_interval_ms
            )
        );
    }


    return 0;
}