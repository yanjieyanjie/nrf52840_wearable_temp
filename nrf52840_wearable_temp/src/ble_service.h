#ifndef BLE_SERVICE_H
#define BLE_SERVICE_H


#include <stdint.h>
#include <stdbool.h>


/*
 * 初始化 BLE。
 *
 * 包括：
 *
 * Bluetooth Stack
 * Bond Settings 恢复
 * Security
 * Advertising
 */
int ble_service_init(void);


/*
 * 更新当前温度。
 *
 * 单位：
 * 0.1°C
 *
 * 278 = 27.8°C
 */
void ble_service_set_temperature(
    int16_t temp_tenths
);


/*
 * 如果：
 *
 * 已连接
 * +
 * 手机开启 Notification
 *
 * 则发送当前温度。
 */
int ble_service_notify_temperature(void);


/*
 * 是否存在 BLE Connection。
 */
bool ble_service_is_connected(void);


/*
 * 手机是否已经开启
 * Temperature Notification。
 */
bool ble_service_is_notify_enabled(void);


#endif