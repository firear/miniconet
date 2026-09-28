#ifndef __CHARARRAY_H__
#define __CHARARRAY_H__
#include "hexport.h"
#include <assert.h>
#include <stddef.h>

#define ARRAY_INIT_SIZE 16
#if _WIN32
#pragma warning(disable : 4201)
#if defined(_MSC_VER) && _MSC_VER > 1910
#pragma warning(disable : 5045)
#endif
#endif

// usage: init cleanup
// Automatically always add 0 at the end

typedef struct chars_s {
    size_t size;
    size_t maxsize;
    // private:
    union {
        char _d[ARRAY_INIT_SIZE];
        char* _ptr;
    };
} chars_t;

#if defined(__cplusplus)
extern "C" {
#endif

HV_INLINE char* chars_t_at(chars_t* p, int pos)
{
    if (pos < 0) {
        pos += (int)p->size;
    }
    assert(pos >= 0 && pos <= (int)p->size);
    return p->maxsize > sizeof(p->_d) ? p->_ptr + pos : p->_d + pos;
}

void chars_t_init(chars_t* p, size_t maxsize);
void chars_t_clear(chars_t* p);
void chars_t_cleanup(chars_t* p);
void chars_t_resize(chars_t* p, size_t maxsize);
void chars_t_push_back(chars_t* p, char* elem);
char chars_t_pop_back(chars_t* p);
void chars_t_insert(chars_t* p, int pos, const void* data, size_t datalen);
void chars_t_del(chars_t* p, int pos, int size);
void chars_t_append(chars_t* p, const void* data, size_t datalen);
void chars_t_lstrip(chars_t* p);
void chars_t_rstrip(chars_t* p);
void chars_t_strip(chars_t* p);
void chars_t_set(chars_t* p, const void* data, size_t datalen);
void chars_t_replace_at(chars_t* p, size_t pos, size_t sn, const void* r, size_t rn);
// search and replace s:sn in p by r:rn
void chars_t_replace(chars_t* p, const void* s, size_t sn, const void* r, size_t rn);
#define chars_t_appendcstr(p, c) chars_t_append(p, c, sizeof(c) - 1)
#define chars_t_cstr(p) chars_t_at((p), 0)
#define chars_t_set2(p, s) chars_t_set(p, chars_t_cstr(s), (s)->size)
#define chars_t_append2(p, s) chars_t_append(p, chars_t_cstr(s), (s)->size)
#define chars_t_empty {0,ARRAY_INIT_SIZE,{{0}}}

typedef struct {
    const char* begin;
    size_t len;
} strseg_t;
unsigned cstr_split(const char* strline, const char ctab, strseg_t** pValues);

void testchars_t(void);

#if defined(__cplusplus)
}
#endif

#endif
