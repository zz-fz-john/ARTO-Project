#ifndef BCM_GPIO_H
#define BCM_GPIO_H

#define PIN_IN_DIR  0
#define PIN_OUT_DIR 1
 
#define PIN_LOW_VAL  0
#define PIN_HIGH_VAL 1

// Defines according to periferal connections
// 树莓派 BCM GPIO 引脚编号（不是物理引脚号）
#define PIR_GPIO 17      // BCM GPIO 17 (物理引脚 11)
#define RELAY1_GPIO 8    // BCM GPIO 8  (物理引脚 24)
#define RELAY2_GPIO 9    // BCM GPIO 9  (物理引脚 21)
#define RELAY3_GPIO 10   // BCM GPIO 10 (物理引脚 19)
#define RELAY4_GPIO 11   // BCM GPIO 11 (物理引脚 23)

// Internal defines 
#define PIN_NAME_MAX_BUFF_LEN 4
#define PIN_DIRECTION_PATH_MAX_LEN 34
#define GPIO_EXPORT_MAX_WAIT_CYCLES 20
#define PIN_VALUE_PATH_MAX_LEN 30
#define PIN_VALUE_STR_LEN 4

// General fn for BCM
int GPIO_export(int pin);
int GPIO_unexport(int pin);
int GPIO_direction(int pin, int dir);
int GPIO_read(int pin, int *value);
int GPIO_write(int pin, int value);

// Fn specific for alarm sys connections
int export_gpios(void);
int configure_gpios(void);
int unexport_gpios(void);

#endif
