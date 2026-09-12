#include "sensor_manager.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#include <errno.h>
#include <stdint.h>


/*
 * DS18B20 Device
 */
static const struct device *ds18b20;


/*
 * 初始化 DS18B20
 */
int sensor_manager_init(void)
{
    ds18b20 =
        DEVICE_DT_GET_ANY(maxim_ds18b20);


    if (ds18b20 == NULL) {

        printk(
            "SENSOR: DS18B20 not found\n"
        );

        return -ENODEV;
    }


    if (!device_is_ready(ds18b20)) {

        printk(
            "SENSOR: DS18B20 not ready\n"
        );

        return -ENODEV;
    }


    printk(
        "SENSOR: DS18B20 ready\n"
    );


    return 0;
}


/*
 * 读取温度
 */
int sensor_manager_read_temperature(
    int16_t *temp_tenths)
{
    struct sensor_value temp;

    int ret;


    /*
     * 防止调用者传入 NULL
     */
    if (temp_tenths == NULL) {

        return -EINVAL;
    }


    /*
     * DS18B20 是否已经初始化
     */
    if (ds18b20 == NULL) {

        return -ENODEV;
    }


    /*
     * 获取新的温度样本
     */
    ret =
        sensor_sample_fetch(
            ds18b20
        );


    if (ret != 0) {

        printk(
            "SENSOR: sample fetch failed: %d\n",
            ret
        );

        return ret;
    }


    /*
     * 获取温度通道
     */
    ret =
        sensor_channel_get(
            ds18b20,

            SENSOR_CHAN_AMBIENT_TEMP,

            &temp
        );


    if (ret != 0) {

        printk(
            "SENSOR: channel get failed: %d\n",
            ret
        );

        return ret;
    }


    /*
     * Zephyr sensor_value
     *
     * val1:
     * 整数部分
     *
     * val2:
     * 小数部分 * 1000000
     *
     * 例如：
     *
     * 27.8125°C
     *
     * val1 = 27
     * val2 = 812500
     */


    int64_t temp_micro =
        ((int64_t)temp.val1 * 1000000LL)
        +
        temp.val2;


    /*
     * 转成 0.1°C
     *
     * 27.8°C
     *
     * ->
     *
     * 278
     */
    *temp_tenths =
        (int16_t)(
            temp_micro /
            100000LL
        );


    return 0;
}
