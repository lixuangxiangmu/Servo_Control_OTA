#include "app_main.h"

#include "app_config.h"
#include "app_control.h"
#include "app_display.h"
#include "app_hmi.h"
#include "app_state.h"
#include "app_storage.h"

#include "return_code.h"

/*****************************************************************************
@brief: Initialize the product application layer
@para:
@return:
*******************************************************************************/
void app_main_init(void)
{
    app_servo_params_t params;
    app_storage_source_t storage_source;
    int ret;
    uint8_t state_ready = 0U;

    (void)app_components_init();

    ret = app_state_init();
    if (RET_IS_OK(ret))
    {
        ret = app_storage_load_params(&params, &storage_source);
        if (RET_IS_OK(ret))
        {
            (void)app_state_set_servo_params(&params);
            (void)app_state_set_storage_source(storage_source);
            //如果记忆功能开启，并且上次掉电前是折叠状态
            if ((params.memory_function_enable != 0U) && (params.power_off_fold_state == (uint8_t)APP_SERVO_POSE_FOLD))
            {
                (void)app_state_set_servo_pose(APP_SERVO_POSE_FOLD, params.angle_fold);
            }
            else
            {
                (void)app_state_set_servo_pose(APP_SERVO_POSE_INIT, params.angle_init);
            }
            state_ready = 1U;
        }
    }

    ret = app_services_init();
    if (RET_IS_OK(ret) && (state_ready != 0U))
    {
        (void)app_control_init();
        (void)app_display_init();
        (void)app_hmi_init();
    }
}
