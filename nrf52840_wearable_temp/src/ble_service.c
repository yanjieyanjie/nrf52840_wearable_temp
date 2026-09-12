#include "ble_service.h"
#include "config_manager.h"

#include <zephyr/kernel.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <zephyr/settings/settings.h>

#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <stdint.h>
#include <stdbool.h>


/* =========================================================
 * UUID
 * =========================================================
 *
 * Service:
 *
 * 8e400001-f315-4f60-9fb8-838830daea50
 *
 *
 * Temperature:
 *
 * 8e400002-f315-4f60-9fb8-838830daea50
 *
 *
 * Sampling Interval:
 *
 * 8e400003-f315-4f60-9fb8-838830daea50
 * =========================================================
 */


#define BT_UUID_WEARABLE_SERVICE_VAL \
    BT_UUID_128_ENCODE( \
        0x8e400001, \
        0xf315, \
        0x4f60, \
        0x9fb8, \
        0x838830daea50 \
    )


#define BT_UUID_WEARABLE_TEMP_VAL \
    BT_UUID_128_ENCODE( \
        0x8e400002, \
        0xf315, \
        0x4f60, \
        0x9fb8, \
        0x838830daea50 \
    )


#define BT_UUID_WEARABLE_INTERVAL_VAL \
    BT_UUID_128_ENCODE( \
        0x8e400003, \
        0xf315, \
        0x4f60, \
        0x9fb8, \
        0x838830daea50 \
    )


/* =========================================================
 * UUID Objects
 * =========================================================
 */


static struct bt_uuid_128 wearable_service_uuid =
    BT_UUID_INIT_128(
        BT_UUID_WEARABLE_SERVICE_VAL
    );


static struct bt_uuid_128 wearable_temp_uuid =
    BT_UUID_INIT_128(
        BT_UUID_WEARABLE_TEMP_VAL
    );


static struct bt_uuid_128 wearable_interval_uuid =
    BT_UUID_INIT_128(
        BT_UUID_WEARABLE_INTERVAL_VAL
    );


/* =========================================================
 * BLE Runtime State
 * =========================================================
 */


/*
 * 当前温度
 *
 * 单位：
 *
 * 0.1°C
 *
 * 例如：
 *
 * 286 = 28.6°C
 */
static atomic_t current_temp_tenths;


/*
 * BLE 是否已经连接。
 */
static atomic_t ble_connected;


/*
 * Temperature Notification
 * 是否开启。
 */
static atomic_t notify_enabled;


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
    int16_t temperature =
        (int16_t)atomic_get(
            &current_temp_tenths
        );


    /*
     * CPU ->
     * BLE Little Endian
     */
    int16_t value =
        sys_cpu_to_le16(
            temperature
        );


    printk(
        "BLE: GATT READ temperature=%d\n",
        temperature
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
 * Temperature CCCD
 * =========================================================
 */


static void temp_ccc_cfg_changed(
    const struct bt_gatt_attr *attr,
    uint16_t value)
{
    ARG_UNUSED(attr);


    bool enabled =
        (value & BT_GATT_CCC_NOTIFY)
        != 0;


    atomic_set(
        &notify_enabled,
        enabled
    );


    printk(
        "BLE: Temperature notification %s\n",

        enabled
            ? "ENABLED"
            : "DISABLED"
    );
}


/* =========================================================
 * Sampling Interval READ
 * =========================================================
 *
 * Sampling Interval 现在由
 * config_manager 管理。
 *
 * BLE Service 只是访问它。
 * =========================================================
 */


static ssize_t read_sampling_interval(
    struct bt_conn *conn,
    const struct bt_gatt_attr *attr,
    void *buf,
    uint16_t len,
    uint16_t offset)
{
    uint16_t interval =
        config_manager_get_sampling_interval_ms();


    uint16_t value =
        sys_cpu_to_le16(
            interval
        );


    printk(
        "BLE: GATT READ interval=%u ms\n",
        interval
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
    ARG_UNUSED(conn);
    ARG_UNUSED(attr);
    ARG_UNUSED(flags);


    /* =====================================================
     * Offset 必须为 0
     * =====================================================
     */


    if (offset != 0) {

        return BT_GATT_ERR(
            BT_ATT_ERR_INVALID_OFFSET
        );
    }


    /* =====================================================
     * 必须正好 2 Byte
     * =====================================================
     */


    if (len != sizeof(uint16_t)) {

        printk(
            "BLE: invalid interval length=%u\n",
            len
        );


        return BT_GATT_ERR(
            BT_ATT_ERR_INVALID_ATTRIBUTE_LEN
        );
    }


    /* =====================================================
     * Little Endian -> CPU
     * =====================================================
     */


    uint16_t new_interval =
        sys_get_le16(
            (const uint8_t *)buf
        );


    printk(
        "BLE: WRITE requested interval=%u ms\n",
        new_interval
    );


    /* =====================================================
     * 交给 Config Manager
     * =====================================================
     */


    int ret =
        config_manager_set_sampling_interval_ms(
            new_interval
        );


    /*
     * 参数范围错误。
     */
    if (ret == -ERANGE) {

        printk(
            "BLE: invalid interval=%u ms\n",
            new_interval
        );


        return BT_GATT_ERR(
            BT_ATT_ERR_VALUE_NOT_ALLOWED
        );
    }


    /*
     * 其他 Config 错误。
     */
    if (ret != 0) {

        printk(
            "BLE: config update failed=%d\n",
            ret
        );


        return BT_GATT_ERR(
            BT_ATT_ERR_UNLIKELY
        );
    }


    printk(
        "BLE: sampling interval changed to %u ms\n",
        new_interval
    );


    return len;
}


/* =========================================================
 * GATT Service
 * =========================================================
 *
 * Attribute Table:
 *
 * attrs[0]
 * Primary Service
 *
 * attrs[1]
 * Temperature Characteristic Declaration
 *
 * attrs[2]
 * Temperature Value
 *
 * attrs[3]
 * Temperature CCCD
 *
 * attrs[4]
 * Sampling Interval Characteristic Declaration
 *
 * attrs[5]
 * Sampling Interval Value
 * =========================================================
 */


BT_GATT_SERVICE_DEFINE(
    wearable_service,


    /* =====================================================
     * Primary Service
     * =====================================================
     */

    BT_GATT_PRIMARY_SERVICE(
        &wearable_service_uuid
    ),


    /* =====================================================
     * Temperature
     *
     * READ
     * NOTIFY
     *
     * 目前保持公开访问。
     * =====================================================
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


    /* =====================================================
     * Temperature CCCD
     * =====================================================
     */

    BT_GATT_CCC(
        temp_ccc_cfg_changed,

        BT_GATT_PERM_READ |
        BT_GATT_PERM_WRITE
    ),


    /* =====================================================
     * Sampling Interval
     *
     * READ
     * WRITE
     *
     * 关键：
     *
     * 现在要求 BLE Link 必须加密。
     * =====================================================
     */

    BT_GATT_CHARACTERISTIC(
        &wearable_interval_uuid.uuid,

        BT_GATT_CHRC_READ |
        BT_GATT_CHRC_WRITE,

        BT_GATT_PERM_READ_ENCRYPT |
        BT_GATT_PERM_WRITE_ENCRYPT,

        read_sampling_interval,

        write_sampling_interval,

        NULL
    )
);


/* =========================================================
 * Advertising Data
 * =========================================================
 */


static const struct bt_data ad[] = {

    /*
     * Flags
     */
    BT_DATA_BYTES(
        BT_DATA_FLAGS,

        BT_LE_AD_GENERAL |
        BT_LE_AD_NO_BREDR
    ),


    /*
     * Custom Service UUID
     */
    BT_DATA_BYTES(
        BT_DATA_UUID128_ALL,

        BT_UUID_WEARABLE_SERVICE_VAL
    ),
};


/*
 * Scan Response
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
 * Start Advertising
 * =========================================================
 */


static int start_advertising(void)
{
    int ret;


    ret =
        bt_le_adv_start(
            BT_LE_ADV_CONN_FAST_1,

            ad,
            ARRAY_SIZE(ad),

            sd,
            ARRAY_SIZE(sd)
        );


    if (ret != 0) {

        printk(
            "BLE: advertising failed=%d\n",
            ret
        );


        return ret;
    }


    printk(
        "BLE: advertising started, name=%s\n",
        CONFIG_BT_DEVICE_NAME
    );


    return 0;
}


/* =========================================================
 * Pairing Complete
 * =========================================================
 */


static void pairing_complete(
    struct bt_conn *conn,
    bool bonded)
{
    ARG_UNUSED(conn);


    printk(
        "BLE: pairing complete, bonded=%s\n",

        bonded
            ? "YES"
            : "NO"
    );
}


/* =========================================================
 * Pairing Failed
 * =========================================================
 */


static void pairing_failed(
    struct bt_conn *conn,
    enum bt_security_err reason)
{
    ARG_UNUSED(conn);


    printk(
        "BLE: pairing failed, reason=%d\n",
        reason
    );
}


/* =========================================================
 * Authentication Information Callback
 * =========================================================
 *
 * 用于观察：
 *
 * Pairing 成功
 * Pairing 失败
 * 是否创建 Bond
 * =========================================================
 */


static struct bt_conn_auth_info_cb auth_info_callbacks = {

    .pairing_complete =
        pairing_complete,

    .pairing_failed =
        pairing_failed,
};


/* =========================================================
 * Connected
 * =========================================================
 */


static void connected(
    struct bt_conn *conn,
    uint8_t err)
{
    if (err != 0) {

        printk(
            "BLE: connection failed=%u\n",
            err
        );


        return;
    }


    atomic_set(
        &ble_connected,
        true
    );


    printk(
        "BLE: CONNECTED\n"
    );


    /* =====================================================
     * 主动要求 Security Level 2
     * =====================================================
     *
     * L2：
     *
     * 加密连接
     *
     * 当前使用：
     *
     * Just Works
     *
     * 不提供 MITM Authentication。
     * =====================================================
     */


    int ret =
        bt_conn_set_security(
            conn,
            BT_SECURITY_L2
        );


    /*
     * 某些情况下连接已经处于要求的
     * Security Level。
     */
    if (
        ret != 0 &&
        ret != -EALREADY
    ) {

        printk(
            "BLE: security request failed=%d\n",
            ret
        );

    } else {

        printk(
            "BLE: security requested L2\n"
        );
    }
}


/* =========================================================
 * Disconnected
 * =========================================================
 */


static void disconnected(
    struct bt_conn *conn,
    uint8_t reason)
{
    ARG_UNUSED(conn);


    atomic_set(
        &ble_connected,
        false
    );


    atomic_set(
        &notify_enabled,
        false
    );


    printk(
        "BLE: DISCONNECTED reason=0x%02X\n",
        reason
    );


    /*
     * 断开连接后重新广播。
     */
    int ret =
        start_advertising();


    if (ret != 0) {

        printk(
            "BLE: restart advertising failed=%d\n",
            ret
        );
    }
}


/* =========================================================
 * Security Changed
 * =========================================================
 */


static void security_changed(
    struct bt_conn *conn,
    bt_security_t level,
    enum bt_security_err err)
{
    ARG_UNUSED(conn);


    /*
     * Security Procedure 失败。
     */
    if (err != BT_SECURITY_ERR_SUCCESS) {

        printk(
            "BLE: security failed "
            "level=%u err=%d\n",

            level,
            err
        );


        return;
    }


    /*
     * Security 成功。
     */
    printk(
        "BLE: security changed level=%u\n",
        level
    );


    /*
     * 当前目标：
     *
     * level=2
     *
     * 表示连接已经加密。
     */
    if (level >= BT_SECURITY_L2) {

        printk(
            "BLE: encrypted link ready\n"
        );
    }
}


/* =========================================================
 * Connection Callbacks
 * =========================================================
 */


BT_CONN_CB_DEFINE(
    connection_callbacks
) = {

    .connected =
        connected,

    .disconnected =
        disconnected,

    .security_changed =
        security_changed,
};


/* =========================================================
 * BLE Init
 * =========================================================
 */


int ble_service_init(void)
{
    int ret;


    /* =====================================================
     * Runtime State
     * =====================================================
     */


    atomic_set(
        &current_temp_tenths,
        0
    );


    atomic_set(
        &ble_connected,
        false
    );


    atomic_set(
        &notify_enabled,
        false
    );


    /* =====================================================
     * Bluetooth Stack
     * =====================================================
     */


    ret =
        bt_enable(NULL);


    if (ret != 0) {

        printk(
            "BLE: Bluetooth init failed=%d\n",
            ret
        );


        return ret;
    }


    printk(
        "BLE: Bluetooth ready\n"
    );


    /* =====================================================
     * Restore Bluetooth Settings
     * =====================================================
     *
     * Config Manager 已经在 main() 中：
     *
     * settings_subsys_init()
     *
     * 所以这里不用重复初始化 Settings。
     *
     *
     * bt_enable() 之后，
     * Bluetooth Settings Handler 已经注册。
     *
     * 这里只加载 "bt" 子树，
     * 恢复：
     *
     * Identity
     * Keys
     * Bond
     * =====================================================
     */


    ret =
        settings_load_subtree(
            "bt"
        );


    if (ret != 0) {

        printk(
            "BLE: Bluetooth settings load failed=%d\n",
            ret
        );


        return ret;
    }


    printk(
        "BLE: Bluetooth settings loaded\n"
    );


    /* =====================================================
     * Bondable
     * =====================================================
     *
     * 明确告诉 Bluetooth：
     *
     * Pairing 完成以后保存 Bond。
     * =====================================================
     */


    bt_set_bondable(
        true
    );


    printk(
        "BLE: bonding enabled\n"
    );


    /* =====================================================
     * Pairing Information Callback
     * =====================================================
     */


    ret =
        bt_conn_auth_info_cb_register(
            &auth_info_callbacks
        );


    if (ret != 0) {

        printk(
            "BLE: auth info callback register failed=%d\n",
            ret
        );


        return ret;
    }


    /* =====================================================
     * Advertising
     * =====================================================
     */


    ret =
        start_advertising();


    if (ret != 0) {

        return ret;
    }


    return 0;
}


/* =========================================================
 * Set Temperature
 * =========================================================
 */


void ble_service_set_temperature(
    int16_t temp_tenths)
{
    atomic_set(
        &current_temp_tenths,
        temp_tenths
    );
}


/* =========================================================
 * Temperature Notification
 * =========================================================
 */


int ble_service_notify_temperature(void)
{
    /*
     * 没有连接。
     */
    if (!ble_service_is_connected()) {

        return 0;
    }


    /*
     * 手机没有订阅 Notification。
     */
    if (!ble_service_is_notify_enabled()) {

        return 0;
    }


    /*
     * 最新温度。
     */
    int16_t temperature =
        (int16_t)atomic_get(
            &current_temp_tenths
        );


    /*
     * CPU ->
     * BLE Little Endian
     */
    int16_t value =
        sys_cpu_to_le16(
            temperature
        );


    /*
     * attrs[2]
     *
     * Temperature Value Attribute
     */
    int ret =
        bt_gatt_notify(
            NULL,

            &wearable_service.attrs[2],

            &value,

            sizeof(value)
        );


    if (ret != 0) {

        printk(
            "BLE: notification failed=%d\n",
            ret
        );


        return ret;
    }


    printk(
        "BLE: NOTIFY temperature=%d\n",
        temperature
    );


    return 0;
}


/* =========================================================
 * Connected?
 * =========================================================
 */


bool ble_service_is_connected(void)
{
    return
        atomic_get(
            &ble_connected
        )
        != 0;
}


/* =========================================================
 * Notification Enabled?
 * =========================================================
 */


bool ble_service_is_notify_enabled(void)
{
    return
        atomic_get(
            &notify_enabled
        )
        != 0;
}