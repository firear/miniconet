#ifndef __HMEMDUP_H__
#define __HMEMDUP_H__
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* hmemdup(const void* mem, const size_t len);
#ifdef _WIN32
void* memmem(const void* l, size_t l_len, const void* s, size_t s_len);
#endif

#ifdef __cplusplus
}
#endif
#endif //__HMEMDUP_H__