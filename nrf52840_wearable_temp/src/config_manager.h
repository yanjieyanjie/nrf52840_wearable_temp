#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <stdint.h>


#define CONFIG_INTERVAL_DEFAULT_MS  2000U
#define CONFIG_INTERVAL_MIN_MS      1000U
#define CONFIG_INTERVAL_MAX_MS      10000U


/*
 * 初始化配置系统，并从 Flash 恢复配置。
 */
int config_manager_init(void);


/*
 * 获取当前采样周期。
 */
uint16_t config_manager_get_sampling_interval_ms(void);


/*
 * 修改采样周期。
 *
 * RAM 中立即生效，
 * Flash 在短暂延迟后保存。
 *
 * 返回：
 * 0       成功
 * -ERANGE 参数超出范围
 */
int config_manager_set_sampling_interval_ms(
    uint16_t interval_ms
);


#endif