#ifndef FEATURE_CONFIG_H
#define FEATURE_CONFIG_H

/* Enable bare-metal polling mode (Bootloader must NOT use FreeRTOS). */
#define CONFIG_USE_BAREMETAL            1

/* Enable FreeRTOS scheduling mode (disabled in Bootloader). */
#define CONFIG_USE_FREERTOS             0

/* Enable the logging middleware. */
#define CONFIG_USE_LOG                  0

/* Enable parameter storage functionality (disabled in Bootloader). */
#define CONFIG_USE_PARAM_STORAGE        0

/* Enable OTA upgrade support. */
#define CONFIG_USE_OTA                  1

#endif /* FEATURE_CONFIG_H */
