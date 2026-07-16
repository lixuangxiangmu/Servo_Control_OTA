#ifndef RETURN_CODE_H
#define RETURN_CODE_H

/* Generic return value type. All modules should prefer using the definitions
   in this enumeration for int return values. */
typedef enum
{
    RET_OK                 = 0,     /* Operation succeeded. */
    RET_FAIL               = -1,    /* Generic failure, cannot be classified as a more specific error. */
    RET_INVALID_PARAM      = -2,    /* Invalid parameter or null pointer. */
    RET_NOT_FOUND          = -3,    /* Specified object, device, or resource not found. */
    RET_NO_RESOURCE        = -4,    /* Fixed resources such as entries, handles, or channels are exhausted. */
    RET_NO_MEMORY          = -5,    /* Insufficient dynamic memory. */
    RET_NOT_SUPPORTED      = -6,    /* The feature is not currently supported by this interface or platform. */
    RET_BUSY               = -7,    /* Resource is busy, cannot perform the operation at this time. */
    RET_TIMEOUT            = -8,    /* Wait timeout. */
    RET_INVALID_STATE      = -9,    /* The current state does not permit this operation. */
    RET_EXIST              = -10,   /* Object already exists, cannot create or register again. */
    RET_ALREADY_EXISTS     = -11,   /* Alias used by new registry code. */
    RET_NOT_INITED         = -12,   /* Object has not been initialized yet. */
    RET_IO_ERROR           = -13,   /* Low-level I/O error. */
    RET_BUFFER_TOO_SMALL   = -14,   /* Provided buffer cannot hold the requested data. */
} ret_code_t;

/* Convert a positive value (such as length or count) representing success
   into a unified int return value. */
#define RET_SUCCESS_VALUE(value)    ((int)(value))

/* Check whether the return value indicates success. */
#define RET_IS_OK(ret)              ((ret) == RET_OK)

/* Check whether the return value indicates an error. */
#define RET_IS_ERR(ret)             ((ret) < RET_OK)

#endif
