#include "motor_bsp.h"
#include "user_config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "motor_bsp";
static motor_t motors[2];

TaskHandle_t motor_limit_task_handle;
int8_t limit_direction = 0; // 当前被子的方向，1表示在床头，-1表示在床尾
void motor_limit_task(void *arg)
{
    static int counter = 0;
    while (1)
    {
        // 读取限位霍尔状态，床头位置，床是开着的
        if (gpio_get_level(MOTOR_LIMIT_HEAD_PIN) == 0 && limit_direction == 1)
        {
            motor_set_mode_speed(0, MOTOR_MODE_STOP);
            motor_set_mode_speed(1, MOTOR_MODE_STOP);
            counter = 0; // 重置计数器
            // 挂起限位任务
            vTaskSuspend(NULL);
        }
        // 读取限位霍尔状态，床尾位置，床是关闭状态
        if (gpio_get_level(MOTOR_LIMIT_BOTTOM_PIN) == 0 && limit_direction == -1)
        {
            motor_set_mode_speed(0, MOTOR_MODE_STOP);
            motor_set_mode_speed(1, MOTOR_MODE_STOP);
            counter = 0; // 重置计数器
            // 挂起限位任务
            vTaskSuspend(NULL);
        }
        // 计时5秒，如果5秒内没有触发限位，说明可能是限位失效了，强制停止电机并挂起任务
        counter++;
        if (counter >= 100) // 5秒钟（50ms * 100）
        {
            motor_set_mode_speed(0, MOTOR_MODE_STOP);
            motor_set_mode_speed(1, MOTOR_MODE_STOP);
            ESP_LOGW(TAG, "Motor limit timeout, stopping motor and suspending task");
            counter = 0; // 重置计数器
            vTaskSuspend(NULL);
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// 被子移动的前后限位霍尔引脚初始化
void motor_limit_init()
{
    gpio_reset_pin(MOTOR_LIMIT_HEAD_PIN);
    gpio_reset_pin(MOTOR_LIMIT_BOTTOM_PIN);
    gpio_config_t gpio_conf = {};
    gpio_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_conf.mode = GPIO_MODE_INPUT;
    gpio_conf.pin_bit_mask = (1ULL << MOTOR_LIMIT_HEAD_PIN) | (1ULL << MOTOR_LIMIT_BOTTOM_PIN);
    gpio_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&gpio_conf));
}

// 电机的4路pwm初始化
void motor_bsp_init()
{
    const gpio_num_t pins[4] = {MOTOR_PWM_PIN1, MOTOR_PWM_PIN2, MOTOR_PWM_PIN3, MOTOR_PWM_PIN4};

    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,    // 低速模式
        .duty_resolution = LEDC_TIMER_10_BIT, // 10位分辨率
        .timer_num = LEDC_TIMER_2,            // 使用定时器2
        .freq_hz = 1 * 1000,                  // 1kHz PWM频率（电机控制的合适频率）
        .clk_cfg = LEDC_AUTO_CLK,             // 自动选择时钟源
    };
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_timer_config(&timer_conf));
    // 配置电机PWM通道
    for (int i = 0; i < 4; i++)
    {
        ledc_channel_config_t ledc_conf = {
            .gpio_num = pins[i],               // 设置GPIO引脚
            .speed_mode = LEDC_LOW_SPEED_MODE, // 低速模式
            .channel = (ledc_channel_t)i,      // 使用通道0-3
            .intr_type = LEDC_INTR_DISABLE,    // 禁用中断
            .timer_sel = LEDC_TIMER_2,         // 选择定时器2
            .duty = 0,                         // 初始占空比为0
            .hpoint = 0,                       // 初始hpoint为0
        };
        ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_channel_config(&ledc_conf));
    }
    // 初始化电机结构体
    motors[0].pwm_channel1 = LEDC_CHANNEL_0;
    motors[0].pwm_channel2 = LEDC_CHANNEL_1;
    motors[0].speed = MOTOR_SPEED_STOP;
    motors[1].pwm_channel1 = LEDC_CHANNEL_2;
    motors[1].pwm_channel2 = LEDC_CHANNEL_3;
    motors[1].speed = MOTOR_SPEED_STOP;

    motor_limit_init();
    xTaskCreatePinnedToCore(motor_limit_task, "motor_limit_task", 4096, NULL, 10, &motor_limit_task_handle, 1);
    vTaskSuspend(motor_limit_task_handle); // 默认挂起，使用时再恢复

    if (gpio_get_level(MOTOR_LIMIT_HEAD_PIN) == 0)
    {
        limit_direction = 1; // 初始状态在床头
    }
    else
    {
        limit_direction = 0;
        quilt_open();
    }
}

void pwm_set_duty(ledc_channel_t motor_channel, uint16_t duty)
{
    if (motor_channel >= 4 || motor_channel < 0)
    {
        ESP_LOGE("motor_bsp", "Invalid motor channel: %d", motor_channel);
        return;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)motor_channel, duty));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)motor_channel));
}

void motor_set_mode_speed(uint8_t motor_index, motor_mode_t mode, motor_speed_t speed)
{
    if (motor_index >= 2)
    {
        ESP_LOGE(TAG, "Invalid motor index: %d", motor_index);
        return;
    }

    motor_t *motor = &motors[motor_index];
    motor->speed = speed;

    switch (mode)
    {
    case MOTOR_MODE_STOP:
        pwm_set_duty(motor->pwm_channel1, speed);
        pwm_set_duty(motor->pwm_channel2, speed);
        break;
    case MOTOR_MODE_FORWARD:
        pwm_set_duty(motor->pwm_channel1, speed);
        pwm_set_duty(motor->pwm_channel2, 0);
        break;
    case MOTOR_MODE_REVERSE:
        pwm_set_duty(motor->pwm_channel1, 0);
        pwm_set_duty(motor->pwm_channel2, speed);
        break;
    case MOTOR_MODE_BRAKE:
        pwm_set_duty(motor->pwm_channel1, MOTOR_SPEED_FULL);
        pwm_set_duty(motor->pwm_channel2, MOTOR_SPEED_FULL);
        break;
    default:
        ESP_LOGE(TAG, "Invalid motor mode: %d", mode);
        break;
    }
}

void quilt_open()
{
    if (limit_direction == 1)
    {
        return;
    }
    motor_set_mode_speed(0, MOTOR_MODE_REVERSE, MOTOR_SPEED_80);
    motor_set_mode_speed(1, MOTOR_MODE_FORWARD, MOTOR_SPEED_80);
    limit_direction = 1;
    vTaskResume(motor_limit_task_handle);
}

void quilt_close()
{
    if (limit_direction == -1)
    {
        return;
    }
    motor_set_mode_speed(0, MOTOR_MODE_FORWARD, MOTOR_SPEED_80);
    motor_set_mode_speed(1, MOTOR_MODE_REVERSE, MOTOR_SPEED_80);
    limit_direction = -1;
    vTaskResume(motor_limit_task_handle);
}

void quilt_limit_reset()
{
    limit_direction = 0;
}