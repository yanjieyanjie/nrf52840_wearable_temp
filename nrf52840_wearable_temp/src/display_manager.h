#ifndef DISPLAY_MANAGER_H
#define DISPLAY_MANAGER_H

#include <stdint.h>
#include <stdbool.h>


/*
 * 初始化 OLED
 */
int display_manager_init(void);


/*
 * 更新 OLED
 *
 * temp_tenths:
 * 278 = 27.8°C
 *
 * ble_connected:
 * true  = BLE 已连接
 * false = Advertising
 *
 * sensor_ok:
 * true  = 传感器正常
 * false = 传感器异常
 *
 * sensor_error:
 * 传感器错误码
 */
int display_manager_update(
    int16_t temp_tenths,
    bool ble_connected,
    bool sensor_ok,
    int sensor_error
);


#endif