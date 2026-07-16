#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#define BOARD_SERVO_LED_STATUS_NAME         "servo_led_status"
#define BOARD_SERVO_PWM_NAME                "servo_pwm"
#define BOARD_SERVO_POWER_HOLD_NAME         "servo_power_hold"
#define BOARD_I2C0_NAME                     "i2c0"
#define BOARD_UART3_NAME                    "uart3"
#define BOARD_REMOTE_RF_GPIO_NAME           "remote_rf_data"
#define BOARD_REMOTE_SAMPLE_TIMER_NAME      "remote_sample_timer"

/* 板级 ADC 逻辑设备名。
   应用层和服务层只通过这些名字读取 raw 原始采样值，不直接依赖 ADC 通道号。 */
#define BOARD_ADC_POWER_RAW_NAME            "adc_power_raw"
#define BOARD_ADC_LOCKED_ROTOR_RAW_NAME     "adc_locked_rotor_raw"

/* 板级按键设备名称，BSP 注册 GPIO 时使用，应用层只通过这些名称绑定服务。 */
#define BOARD_KEY_BUTTON_NAME               "key_button"
#define BOARD_KEY_ENCODER_NAME              "key_encoder"
#define BOARD_KEY_ONBOARD_NAME              "key_onboard"

/* EC11 编码器 A/B 相 GPIO 逻辑设备名。
   BSP 在 board.c 中把这两个名字绑定到具体 PB8/PB9 引脚；
   service/application 只使用名字，不直接依赖 GD32 端口和引脚。 */
#define BOARD_ENCODER_PHASE_A_NAME          "encoder_phase_a"
#define BOARD_ENCODER_PHASE_B_NAME          "encoder_phase_b"

/* 数码管段选 GPIO 设备名。硬件连接沿用老工程：
   A/DP/C/D/G 在 GPIOA，B/E/F 在 GPIOC。 */
#define BOARD_DIGITAL_TUBE_SEG_A_NAME       "digital_seg_a"
#define BOARD_DIGITAL_TUBE_SEG_B_NAME       "digital_seg_b"
#define BOARD_DIGITAL_TUBE_SEG_C_NAME       "digital_seg_c"
#define BOARD_DIGITAL_TUBE_SEG_D_NAME       "digital_seg_d"
#define BOARD_DIGITAL_TUBE_SEG_E_NAME       "digital_seg_e"
#define BOARD_DIGITAL_TUBE_SEG_F_NAME       "digital_seg_f"
#define BOARD_DIGITAL_TUBE_SEG_G_NAME       "digital_seg_g"
#define BOARD_DIGITAL_TUBE_SEG_DP_NAME      "digital_seg_dp"

/* 数码管位选 GPIO 设备名。 */
#define BOARD_DIGITAL_TUBE_DIG1_NAME        "digital_digit_1"
#define BOARD_DIGITAL_TUBE_DIG2_NAME        "digital_digit_2"
#define BOARD_DIGITAL_TUBE_DIG3_NAME        "digital_digit_3"
#define BOARD_DIGITAL_TUBE_DIG4_NAME        "digital_digit_4"




#endif
