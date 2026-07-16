#ifndef DEVICE_H
#define DEVICE_H

#include <stdint.h>

/* Forward declaration of device_t, allowing operation function pointers
   to reference the device object. */
typedef struct device device_t;

/* Device operation function table, implemented by the specific driver
   adaptation layer. */
typedef struct
{
    int (*init)(device_t *dev);                                   /* Initialize the device. */
    int (*open)(device_t *dev);                                   /* Open the device. */
    int (*close)(device_t *dev);                                  /* Close the device. */
    int (*read)(device_t *dev, uint8_t *buf, uint32_t len);        /* Read data from the device. */
    int (*write)(device_t *dev, const uint8_t *buf, uint32_t len); /* Write data to the device. */
    int (*control)(device_t *dev, int cmd, void *arg);             /* Device control extension interface. */
} device_ops_t;

/* Device class definitions, used for registry inspection and dependency checks. */
typedef enum
{
    DEVICE_CLASS_NONE = 0,
    DEVICE_CLASS_GPIO,
    DEVICE_CLASS_UART,
    DEVICE_CLASS_I2C,
    DEVICE_CLASS_SPI,
    DEVICE_CLASS_PWM,
    DEVICE_CLASS_ADC,
    DEVICE_CLASS_FLASH,
    DEVICE_CLASS_TIMER,
    DEVICE_CLASS_RTC,
    DEVICE_CLASS_WDG,
} device_class_t;

/* Generic device object. All peripherals are registered and managed
   uniformly through the device layer. */
struct device
{
    const char *name;               /* Device name, for lookup by upper layers. */
    device_class_t type;            /* Device class for diagnostics and dispatch checks. */
    const device_ops_t *ops;        /* Device operation function table. */
    void *user_data;                /* 指向这个外设真正的硬件上下文 */
    uint16_t flags;                 /* Reserved flags for driver and device-layer extension. */
    uint8_t initialized;            /* Initialization status flag. */
    uint8_t opened;                 /* Open status flag. */
    uint8_t ref_count;              /* Open reference count. */
};

typedef void (*device_dump_output_t)(const device_t *dev, uint8_t index);

/*****************************************************************************
@brief: Register a device into the global device table
@para:dev Pointer to the device object to register
@return: RET_OK indicates successful registration,
         other return values indicate registration failure
*******************************************************************************/
int device_register(device_t *dev);

/*****************************************************************************
@brief: Find a registered device by name
@para:name Device name string
@return: Pointer to the device object if found, null pointer if not found
         or if the parameter is invalid
*******************************************************************************/
device_t *device_find(const char *name);

/*****************************************************************************
@brief: Call the device initialization interface
@para:dev Pointer to the device object
@return: RET_OK or a success value returned by the driver layer indicates success,
         other return values indicate failure
*******************************************************************************/
int device_init(device_t *dev);

/*****************************************************************************
@brief: Call the device open interface
@para:dev Pointer to the device object
@return: RET_OK or a success value returned by the driver layer indicates success,
         other return values indicate failure
*******************************************************************************/
int device_open(device_t *dev);

/*****************************************************************************
@brief: Call the device close interface
@para:dev Pointer to the device object
@return: RET_OK or a success value returned by the driver layer indicates success,
         other return values indicate failure
*******************************************************************************/
int device_close(device_t *dev);

/*****************************************************************************
@brief: Call the device read interface
@para:dev Pointer to the device object
@para:buf Data receive buffer
@para:len Expected read length
@return: A positive value indicates the actual number of bytes read,
         other return values indicate failure
*******************************************************************************/
int device_read(device_t *dev, uint8_t *buf, uint32_t len);

/*****************************************************************************
@brief: Call the device write interface
@para:dev Pointer to the device object
@para:buf Data buffer to be written
@para:len Length of data to be written
@return: A positive value indicates the actual number of bytes written,
         other return values indicate failure
*******************************************************************************/
int device_write(device_t *dev, const uint8_t *buf, uint32_t len);

/*****************************************************************************
@brief: Call the device control interface
@para:dev Pointer to the device object
@para:cmd Control command
@para:arg Pointer to control command argument
@return: RET_OK or a success value returned by the driver layer indicates success,
         other return values indicate failure
*******************************************************************************/
int device_control(device_t *dev, int cmd, void *arg);

/*****************************************************************************
@brief: Dump the current registered device table through a caller supplied callback
@para:output Callback invoked once for each registered device
@return: Number of registered devices,
         RET_INVALID_PARAM indicates an invalid callback
*******************************************************************************/
int device_dump(device_dump_output_t output);

#endif
