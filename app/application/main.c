#include "app_main.h"
#include "board.h"
#include "services.h"

#include "FreeRTOS.h"
#include "task.h"

/*****************************************************************************
@brief: Program entry point
@para:
@return: Does not return under normal conditions
*******************************************************************************/
int main(void)
{
    board_init();
    services_init();
    app_main_init();
    
    /* 启动调度，开始执行任务 */
    vTaskStartScheduler();

    /* 正常情况下不会运行到这里；如果调度器启动失败，停在这里便于调试。 */
    while (1)
    {
    }
}
