#ifndef FEATURE_CONFIG_H
#define FEATURE_CONFIG_H

/* Enable bare-metal polling mode. */
#define CONFIG_USE_BAREMETAL        0

/* Enable FreeRTOS scheduling mode. */
#define CONFIG_USE_FREERTOS         1

/* Enable the logging middleware. */
#define CONFIG_USE_LOG              0

/* Enable parameter storage functionality. */
#define CONFIG_USE_PARAM_STORAGE    1

/* Enable APP-side OTA entry command and metadata service. */
#define CONFIG_USE_OTA              1

#endif
