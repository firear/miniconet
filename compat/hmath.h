#ifndef HV_MATH_H_
#define HV_MATH_H_

#include "portable/vc6_stdint.h"
#include <math.h>

static inline unsigned long floor2e(unsigned long num) {
    unsigned long n = num;
    int e = 0;
    unsigned long ret = 1;
    while (n>>=1) ++e;
    while (e--) ret<<=1;
    return ret;
}

static inline unsigned long ceil2e(unsigned long num) {
    // 2**0 = 1
    if (num == 0 || num == 1)   return 1;
    else{
        unsigned long n = num - 1;
        int e = 1;
        unsigned long ret = 1;
        while (n>>=1) ++e;
        while (e--) ret<<=1;
        return ret;
    }
}

// varint little-endian
// MSB
static inline int varint_encode(int64_t value, unsigned char* buf) {
    unsigned char ch;
    unsigned char *p = buf;
    int bytes = 0;
    do {
        ch = value & 0x7F;
        value >>= 7;
        *p++ = value == 0 ? ch : (ch | 0x80);
        ++bytes;
    } while (value);
    return bytes;
}

// @param[IN|OUT] len: in=>buflen, out=>varint bytesize
static inline int64_t varint_decode(const unsigned char* buf, int* len) {
    int64_t ret = 0;
    int bytes = 0, bits = 0;
    const unsigned char *p = buf;
    do {
        if (len && *len && bytes == *len) {
            // Not enough length
            *len = 0;
            return 0;
        }
        ret |= ((int64_t)(*p & 0x7F)) << bits;
        ++bytes;
        if ((*p & 0x80) == 0) {
            // Found end
            if (len) *len = bytes;
            return ret;
        }
        ++p;
        bits += 7;
    } while(bytes < 10);

    // Not found end
    if (len) *len = -1;
    return ret;
}


// return cost length of buffer
HV_INLINE int decode_varint(const unsigned char* buffer, size_t size, uint64_t* varint)
{
    size_t i = 0;
    uint8_t shift = 0;
    *varint = 0;
    while (i < size && shift < 64) {
        *varint |= (uint64_t)(buffer[i] & 0x7F) << shift;
        if ((buffer[i] & 0x80) == 0) {
            return i + 1; // Return the number of bytes read
        }
        shift += 7;
        i++;
    }
    return -1; // Invalid encoding
}

// Return the number of bytes written
HV_INLINE int encode_varint(uint64_t length, unsigned char* buffer)
{
    int i = 0;
    while ((length & ~0x7F) != 0) {
        buffer[i++] = (unsigned char)((length & 0x7F) | 0x80);
        length >>= 7;
    }
    buffer[i++] = (unsigned char)(length & 0x7F);
    return i; // Return the number of bytes written
}

#endif // HV_MATH_H_
