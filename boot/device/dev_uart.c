#include "dev_uart.h"

#include "device.h"
#include "gd32f10x.h"
#include "return_code.h"

#include <string.h>

#define DEV_UART_DEVICE_MAX              4U
#define DEV_UART_RX_FRAME_QUEUE_DEPTH    4U

typedef struct
{
    uint16_t len;
    uint8_t data[DEV_UART_FRAME_MAX_LEN];
} dev_uart_rx_frame_t;

typedef struct
{
    device_t *dev;
    uint8_t initialized;
    volatile uint8_t tx_done;
    uint32_t pending_tx_len;
    volatile uint8_t rx_head;
    volatile uint8_t rx_tail;
    volatile uint8_t rx_count;
    dev_uart_rx_frame_t rx_queue[DEV_UART_RX_FRAME_QUEUE_DEPTH];
    uart_stats_t stats;
} dev_uart_runtime_t;

static dev_uart_runtime_t s_uart_runtime[DEV_UART_DEVICE_MAX];

static uint32_t dev_uart_enter_critical(void)
{
    uint32_t primask = __get_PRIMASK();

    __disable_irq();

    return primask;
}

static void dev_uart_exit_critical(uint32_t primask)
{
    __set_PRIMASK(primask);
}

static device_t *dev_uart_find(const char *name)
{
    device_t *dev;

    if ((name == 0) || (name[0] == '\0'))
    {
        return 0;
    }

    dev = device_find(name);
    if ((dev == 0) || (dev->type != DEVICE_CLASS_UART))
    {
        return 0;
    }

    return dev;
}

static uint8_t dev_uart_config_is_valid(const uart_config_t *cfg)
{
    if ((cfg == 0) || (cfg->baudrate == 0U) ||
        (cfg->data_bits > UART_DATA_BITS_9) ||
        (cfg->stop_bits > UART_STOP_BITS_2) ||
        (cfg->parity > UART_PARITY_ODD))
    {
        return 0U;
    }

    return 1U;
}

static dev_uart_runtime_t *dev_uart_find_runtime(device_t *dev)
{
    uint8_t i;

    for (i = 0U; i < DEV_UART_DEVICE_MAX; i++)
    {
        if (s_uart_runtime[i].dev == dev)
        {
            return &s_uart_runtime[i];
        }
    }

    return 0;
}

static dev_uart_runtime_t *dev_uart_alloc_runtime(device_t *dev)
{
    dev_uart_runtime_t *runtime;
    uint8_t i;

    runtime = dev_uart_find_runtime(dev);
    if (runtime != 0)
    {
        return runtime;
    }

    for (i = 0U; i < DEV_UART_DEVICE_MAX; i++)
    {
        if (s_uart_runtime[i].dev == 0)
        {
            s_uart_runtime[i].dev = dev;
            s_uart_runtime[i].tx_done = 1U;
            return &s_uart_runtime[i];
        }
    }

    return 0;
}

static void dev_uart_reset_rx_queue(dev_uart_runtime_t *runtime)
{
    uint32_t primask;

    if (runtime == 0)
    {
        return;
    }

    primask = dev_uart_enter_critical();
    runtime->rx_head = 0U;
    runtime->rx_tail = 0U;
    runtime->rx_count = 0U;
    dev_uart_exit_critical(primask);
}

static void dev_uart_rx_isr_cb(const char *name, const uint8_t *data, uint32_t len, void *user_ctx)
{
    dev_uart_runtime_t *runtime = (dev_uart_runtime_t *)user_ctx;
    uint8_t tail;

    (void)name;

    if ((runtime == 0) || (data == 0) || (len == 0U))
    {
        return;
    }

    if (len > DEV_UART_FRAME_MAX_LEN)
    {
        runtime->stats.rx_overflow_frames++;
        return;
    }

    if (runtime->rx_count >= DEV_UART_RX_FRAME_QUEUE_DEPTH)
    {
        runtime->stats.rx_dropped_frames++;
        return;
    }

    tail = runtime->rx_tail;
    memcpy(runtime->rx_queue[tail].data, data, len);
    runtime->rx_queue[tail].len = (uint16_t)len;
    runtime->rx_tail = (uint8_t)((tail + 1U) % DEV_UART_RX_FRAME_QUEUE_DEPTH);
    runtime->rx_count++;
    runtime->stats.rx_frames++;
    runtime->stats.rx_bytes += len;
}

static void dev_uart_tx_done_isr_cb(const char *name, void *user_ctx)
{
    dev_uart_runtime_t *runtime = (dev_uart_runtime_t *)user_ctx;

    (void)name;

    if (runtime == 0)
    {
        return;
    }

    runtime->stats.tx_frames++;
    runtime->stats.tx_bytes += runtime->pending_tx_len;
    runtime->pending_tx_len = 0U;
    runtime->tx_done = 1U;
}

static int dev_uart_prepare(device_t *dev, dev_uart_runtime_t **runtime_out)
{
    dev_uart_runtime_t *runtime;
    uart_callback_t callbacks;
    int ret;

    if ((dev == 0) || (runtime_out == 0))
    {
        return RET_INVALID_PARAM;
    }

    if (dev->initialized == 0U)
    {
        ret = device_init(dev);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }
    }

    runtime = dev_uart_alloc_runtime(dev);
    if (runtime == 0)
    {
        return RET_NO_RESOURCE;
    }

    if (runtime->initialized == 0U)
    {
        runtime->pending_tx_len = 0U;
        runtime->tx_done = 1U;
        dev_uart_reset_rx_queue(runtime);

        callbacks.rx_cb = dev_uart_rx_isr_cb;
        callbacks.tx_done_cb = dev_uart_tx_done_isr_cb;
        callbacks.user_ctx = runtime;
        ret = device_control(dev, DEV_UART_CMD_ATTACH_CB, &callbacks);
        if (RET_IS_ERR(ret))
        {
            runtime->dev = 0;
            return ret;
        }

        runtime->initialized = 1U;
    }

    if (dev->opened == 0U)
    {
        ret = device_open(dev);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }
    }

    *runtime_out = runtime;

    return RET_OK;
}

static int dev_uart_pop_rx_frame(dev_uart_runtime_t *runtime, uint8_t *buf)
{
    uint32_t primask;
    uint8_t head;
    uint32_t len;

    if ((runtime == 0) || (buf == 0))
    {
        return RET_INVALID_PARAM;
    }

    primask = dev_uart_enter_critical();
    if (runtime->rx_count == 0U)
    {
        dev_uart_exit_critical(primask);
        return RET_TIMEOUT;
    }

    head = runtime->rx_head;
    len = runtime->rx_queue[head].len;
    memcpy(buf, runtime->rx_queue[head].data, len);
    runtime->rx_head = (uint8_t)((head + 1U) % DEV_UART_RX_FRAME_QUEUE_DEPTH);
    runtime->rx_count--;
    dev_uart_exit_critical(primask);

    return RET_SUCCESS_VALUE(len);
}

int dev_uart_config(const char *name, const uart_config_t *cfg)
{
    device_t *dev;
    dev_uart_runtime_t *runtime;
    int ret;

    if (dev_uart_config_is_valid(cfg) == 0U)
    {
        return RET_INVALID_PARAM;
    }

    dev = dev_uart_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    if (dev->initialized == 0U)
    {
        ret = device_init(dev);
        if (RET_IS_ERR(ret))
        {
            return ret;
        }
    }

    ret = device_control(dev, DEV_UART_CMD_CONFIG, (void *)cfg);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    runtime = dev_uart_find_runtime(dev);
    if (runtime != 0)
    {
        runtime->pending_tx_len = 0U;
        runtime->tx_done = 1U;
        dev_uart_reset_rx_queue(runtime);
    }

    return RET_OK;
}

int dev_uart_read(const char *name, uint8_t *buf)
{
    device_t *dev;
    dev_uart_runtime_t *runtime;
    int ret;

    if (buf == 0)
    {
        return RET_INVALID_PARAM;
    }

    dev = dev_uart_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = dev_uart_prepare(dev, &runtime);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    return dev_uart_pop_rx_frame(runtime, buf);
}

int dev_uart_write(const char *name, const uint8_t *buf, uint32_t len)
{
    device_t *dev;
    dev_uart_runtime_t *runtime;
    uint32_t primask;
    int ret;

    if ((buf == 0) || (len == 0U))
    {
        return RET_INVALID_PARAM;
    }

    if (len > DEV_UART_FRAME_MAX_LEN)
    {
        return RET_BUFFER_TOO_SMALL;
    }

    dev = dev_uart_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = dev_uart_prepare(dev, &runtime);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    primask = dev_uart_enter_critical();
    if (runtime->tx_done == 0U)
    {
        dev_uart_exit_critical(primask);
        return RET_BUSY;
    }
    runtime->tx_done = 0U;
    runtime->pending_tx_len = len;
    dev_uart_exit_critical(primask);

    ret = device_write(dev, buf, len);
    if (RET_IS_ERR(ret))
    {
        primask = dev_uart_enter_critical();
        runtime->pending_tx_len = 0U;
        runtime->tx_done = 1U;
        dev_uart_exit_critical(primask);
        return ret;
    }

    return ret;
}

int dev_uart_flush_rx(const char *name)
{
    device_t *dev;
    dev_uart_runtime_t *runtime;
    int ret;

    dev = dev_uart_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = dev_uart_prepare(dev, &runtime);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    dev_uart_reset_rx_queue(runtime);
    ret = device_control(dev, DEV_UART_CMD_RESTART_RX, 0);

    return RET_IS_ERR(ret) ? ret : RET_OK;
}

int dev_uart_get_stats(const char *name, uart_stats_t *stats)
{
    device_t *dev;
    dev_uart_runtime_t *runtime;
    uint32_t primask;
    int ret;

    if (stats == 0)
    {
        return RET_INVALID_PARAM;
    }

    dev = dev_uart_find(name);
    if (dev == 0)
    {
        return RET_NOT_FOUND;
    }

    ret = dev_uart_prepare(dev, &runtime);
    if (RET_IS_ERR(ret))
    {
        return ret;
    }

    primask = dev_uart_enter_critical();
    *stats = runtime->stats;
    dev_uart_exit_critical(primask);

    return RET_OK;
}
