#include "device.h"

#include "return_code.h"

#include <string.h>

/* Maximum number of devices that can be registered. */
#define DEVICE_MAX_NUM 32

/* Global device registry table, holding device object pointers registered by each driver. */
static device_t *s_device_table[DEVICE_MAX_NUM];

/*****************************************************************************
@brief: Register a device into the global device table
@para:dev Pointer to the device object to register
@return: RET_OK indicates successful registration,
         RET_INVALID_PARAM indicates an invalid parameter,
         RET_NO_RESOURCE indicates the device table is full
*******************************************************************************/
int device_register(device_t *dev)
{
    if ((dev == 0) || (dev->name == 0) || (dev->name[0] == '\0') || (dev->ops == 0))
    {
        return RET_INVALID_PARAM;
    }

    //判断当前设备有没有被注册过
    for (int i = 0; i < DEVICE_MAX_NUM; i++)
    {
        if ((s_device_table[i] != 0) && (strcmp(s_device_table[i]->name, dev->name) == 0))
        {
            return RET_ALREADY_EXISTS;
        }
    }

    for (int i = 0; i < DEVICE_MAX_NUM; i++)
    {
        if (s_device_table[i] == 0)
        {
            s_device_table[i] = dev;
            return RET_OK;
        }
    }

    return RET_NO_RESOURCE;
}

/*****************************************************************************
@brief: Find a registered device by name
@para:name Device name string
@return: Pointer to the device object if found, null pointer if not found
         or if the parameter is invalid
*******************************************************************************/
device_t *device_find(const char *name)
{
    if ((name == 0) || (name[0] == '\0'))
    {
        return 0;
    }

    for (int i = 0; i < DEVICE_MAX_NUM; i++)
    {
        if ((s_device_table[i] != 0) && (strcmp(s_device_table[i]->name, name) == 0))
        {
            return s_device_table[i];
        }
    }

    return 0;
}

/*****************************************************************************
@brief: Call the device initialization interface
@para:dev Pointer to the device object
@return: RET_OK or a success value returned by the driver layer indicates success,
         RET_INVALID_PARAM indicates an invalid parameter or unimplemented interface
*******************************************************************************/
int device_init(device_t *dev)
{
    int ret;

    if ((dev == 0) || (dev->ops == 0))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->initialized != 0U)
    {
        return RET_OK;
    }

    if (dev->ops->init == 0)
    {
        return RET_NOT_SUPPORTED;
    }

    ret = dev->ops->init(dev);
    if (RET_IS_OK(ret))
    {
        dev->initialized = 1U;
    }

    return ret;
}

/*****************************************************************************
@brief: Call the device open interface
@para:dev Pointer to the device object
@return: RET_OK or a success value returned by the driver layer indicates success,
         RET_INVALID_PARAM indicates an invalid parameter or unimplemented interface
*******************************************************************************/
int device_open(device_t *dev)
{
    int ret = RET_OK;

    if ((dev == 0) || (dev->ops == 0))
    {
        return RET_INVALID_PARAM;
    }

    if ((dev->initialized == 0U) && (dev->ops->init != 0))
    {
        ret = device_init(dev);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }
    }

    if (dev->ops->open != 0)
    {
        ret = dev->ops->open(dev);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }
    }

    if (dev->ref_count < UINT8_MAX)
    {
        dev->ref_count++;
    }
    dev->opened = 1U;

    return ret;
}

/*****************************************************************************
@brief: Call the device close interface
@para:dev Pointer to the device object
@return: RET_OK or a success value returned by the driver layer indicates success,
         RET_INVALID_PARAM indicates an invalid parameter or unimplemented interface
*******************************************************************************/
int device_close(device_t *dev)
{
    int ret = RET_OK;

    if ((dev == 0) || (dev->ops == 0))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->opened == 0U)
    {
        return RET_OK;
    }

    if (dev->ref_count > 1U)
    {
        dev->ref_count--;
        return RET_OK;
    }

    if (dev->ops->close != 0)
    {
        ret = dev->ops->close(dev);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }
    }

    dev->opened = 0U;
    dev->ref_count = 0U;

    return ret;
}

/*****************************************************************************
@brief: Call the device read interface
@para:dev Pointer to the device object
@para:buf Data receive buffer
@para:len Expected read length
@return: A positive value indicates the actual number of bytes read,
         RET_INVALID_PARAM indicates an invalid parameter or unimplemented interface
*******************************************************************************/
int device_read(device_t *dev, uint8_t *buf, uint32_t len)
{
    if ((dev == 0) || (dev->ops == 0) || (buf == 0) || (len == 0U))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->ops->read == 0)
    {
        return RET_NOT_SUPPORTED;
    }

    return dev->ops->read(dev, buf, len);
}

/*****************************************************************************
@brief: Call the device write interface
@para:dev Pointer to the device object
@para:buf Data buffer to be written
@para:len Length of data to be written
@return: A positive value indicates the actual number of bytes written,
         RET_INVALID_PARAM indicates an invalid parameter or unimplemented interface
*******************************************************************************/
int device_write(device_t *dev, const uint8_t *buf, uint32_t len)
{
    if ((dev == 0) || (dev->ops == 0) || (buf == 0) || (len == 0U))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->ops->write == 0)
    {
        return RET_NOT_SUPPORTED;
    }

    return dev->ops->write(dev, buf, len);
}

/*****************************************************************************
@brief: Call the device control interface
@para:dev Pointer to the device object
@para:cmd Control command
@para:arg Pointer to control command argument
@return: RET_OK or a success value returned by the driver layer indicates success,
         RET_INVALID_PARAM indicates an invalid parameter or unimplemented interface
*******************************************************************************/
int device_control(device_t *dev, int cmd, void *arg)
{
    if ((dev == 0) || (dev->ops == 0))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->ops->control == 0)
    {
        return RET_NOT_SUPPORTED;
    }

    return dev->ops->control(dev, cmd, arg);
}

/*****************************************************************************
@brief: Dump the current registered device table through a caller supplied callback
@para:output Callback invoked once for each registered device
@return: Number of registered devices,
         RET_INVALID_PARAM indicates an invalid callback
*******************************************************************************/
int device_dump(device_dump_output_t output)
{
    int count = 0;

    if (output == 0)
    {
        return RET_INVALID_PARAM;
    }

    for (uint8_t i = 0U; i < DEVICE_MAX_NUM; i++)
    {
        if (s_device_table[i] != 0)
        {
            output(s_device_table[i], i);
            count++;
        }
    }

    return count;
}
