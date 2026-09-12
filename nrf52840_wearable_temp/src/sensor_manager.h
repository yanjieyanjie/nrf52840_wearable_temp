#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H


#include <stdint.h>


/*
 * 初始化传感器模块
 *
 * 返回：
 *
 * 0  成功
 * <0 失败
 */
int sensor_manager_init(void);


/*
 * 读取当前温度
 *
 * temp_tenths：
 *
 * 单位 = 0.1°C
 *
 * 例如：
 *
 * 278
 *
 * 表示：
 *
 * 27.8°C
 */
int sensor_manager_read_temperature(
    int16_t *temp_tenths
);


#endif