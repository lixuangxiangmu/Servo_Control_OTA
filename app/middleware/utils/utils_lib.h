#ifndef __UTILS_LIB_H__
#define __UTILS_LIB_H__

#include <stdint.h>


uint16_t utils_calculate_crc16( const uint8_t* temp_buff, uint16_t uilen);
uint32_t utils_calculate_crc32(const uint8_t *buf, uint32_t len);
uint16_t utils_get_u16_le(const uint8_t *buf);
uint32_t utils_get_u32_le(const uint8_t *buf);
void utils_put_u16_le(uint8_t *buf, uint16_t value);
void utils_put_u32_le(uint8_t *buf, uint32_t value);


#endif