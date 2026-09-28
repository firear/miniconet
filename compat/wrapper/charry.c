#if defined(__GNUC__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#endif
#include "charry.h"
#include "hmemdup.h"
#include <assert.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HV_ALLOC(ptr, size)                \
    do {                                   \
        *(void**)&(ptr) = calloc(1, size); \
    } while (0)

#define HV_ALLOC_SIZEOF(ptr) HV_ALLOC(ptr, sizeof(*(ptr)))

#define HV_FREE(ptr)    \
    do {                \
        if (ptr) {      \
            free(ptr);  \
            ptr = NULL; \
        }               \
    } while (0)

static void* __realloc(void* oldptr, size_t newsize, size_t oldsize)
{

    void* ptr = realloc(oldptr, newsize);
    if (!ptr) {
        fprintf(stderr, "realloc failed!\n");
        exit(-1);
    }
    if (newsize > oldsize) {
        memset((char*)ptr + oldsize, 0, newsize - oldsize);
    }
    return ptr;
}

void chars_t_init(chars_t* p, size_t maxsize)
{
    memset(p, 0, sizeof(*p));
    if (maxsize > sizeof(p->_d)) {
        p->maxsize = maxsize;
        HV_ALLOC(p->_ptr, sizeof(char) * maxsize);
    } else {
        p->maxsize = sizeof(p->_d);
    }
}

void chars_t_clear(chars_t* p)
{
    p->size = 0;
    memset(p->maxsize > sizeof(p->_d) ? p->_ptr : p->_d, 0, p->maxsize);
}

void chars_t_cleanup(chars_t* p)
{
    if (p->maxsize > sizeof(p->_d)) {
        HV_FREE(p->_ptr);
        p->maxsize = sizeof(p->_d);
    }
    p->size = 0;
}

#define CHARRY_MAXSIZE 4 * 1024 * 1024 // 4M
void chars_t_resize(chars_t* p, size_t needsize)
{
    size_t maxsize = sizeof(p->_d);
    while (maxsize < needsize) {
        if (maxsize < CHARRY_MAXSIZE) {
            maxsize <<= 1;
        } else {
            maxsize = needsize + 1024;
        }
    }
    if (maxsize != p->maxsize) {
        if (p->maxsize > sizeof(p->_d)) { // data on heap
            p->_ptr = (char*)__realloc(p->_ptr, maxsize, p->maxsize);
        } else {
            char* temp = (char*)malloc(maxsize);
            memcpy(temp, p->_d, p->size);
            memset(temp + p->size, 0, maxsize - p->size);
            p->_ptr = temp;
        }
        p->maxsize = maxsize;
    }
}
#undef CHARRY_MAXSIZE

void chars_t_push_back(chars_t* p, char* elem)
{
    char* ptr;
    if (p->size + 1 >= p->maxsize) {
        chars_t_resize(p, p->size + 2);
    }
    ptr = p->maxsize > sizeof(p->_d) ? p->_ptr : p->_d;
    ptr[p->size++] = *elem;
    ptr[p->size] = '\0';
}

char chars_t_pop_back(chars_t* p)
{
    char *tail, ret;
    assert(p->size > 0);
    tail = chars_t_at(p, -1);
    ret = *tail;
    p->size--;
    *tail = '\0';
    return ret;
}

void chars_t_append(chars_t* p, const void* data, size_t datalen)
{
    size_t nxtsize = p->size + datalen;
    if (nxtsize >= p->maxsize) {
        chars_t_resize(p, nxtsize + 1);
    }
    memcpy(chars_t_at(p, (int)p->size), data, datalen);
    p->size = nxtsize;
    *chars_t_at(p, (int)p->size) = 0;
}

void chars_t_set(chars_t* p, const void* data, size_t datalen)
{
    p->size = 0;
    chars_t_append(p, data, datalen);
}

void chars_t_insert(chars_t* p, int pos, const void* data, size_t datalen)
{
    if (pos > (int)p->size) {
        assert(0);
    } else if (pos == (int)p->size) {
        chars_t_append(p, data, datalen);
    } else {
        size_t nxtsize = p->size + datalen;
        if (nxtsize >= p->maxsize) {
            chars_t_resize(p, nxtsize + 1);
        }

        memmove(chars_t_at(p, pos) + datalen, chars_t_at(p, pos), p->size - pos);
        memcpy(chars_t_at(p, pos), data, datalen);
        p->size = nxtsize;
        *chars_t_at(p, (int)p->size) = 0;
    }
}

void chars_t_del(chars_t* p, int pos, int size)
{
    char* begin = chars_t_at(p, pos);
    memmove(begin, begin + size, p->size - pos - size);
    p->size -= size;
    *chars_t_at(p, (int)p->size) = 0;
}

void chars_t_lstrip(chars_t* p)
{
    char *s, *begin;
    if (p->size == 0)
        return;
    begin = s = chars_t_at(p, 0);
    while (isspace((int)*s) && p->size > 0) {
        s++;
        p->size--;
    }
    if (s != begin) {
        if (p->size > 0) {
            memmove(begin, s, p->size);
        }
        begin[p->size] = '\0';
    }
}

void chars_t_rstrip(chars_t* p)
{
    char* last;
    if (p->size == 0)
        return;
    else {
        char* begin = chars_t_at(p, 0);
        last = chars_t_at(p, -1);
        while (last >= begin && isspace((int)*last)) {
            *last-- = (char)0;
            p->size--;
        }
    }
}

void chars_t_strip(chars_t* p)
{
    char *last, *s, *begin;
    if (p->size == 0)
        return;
    begin = s = chars_t_at(p, 0);
    last = chars_t_at(p, -1);

    while (isspace((int)*s) && p->size > 0) {
        s++;
        p->size--;
    }
    while (last >= s && isspace((int)*last)) {
        *last-- = (char)0;
        p->size--;
    }
    if (s != begin) {
        if (p->size > 0) {
            memmove(begin, s, p->size);
        }
        begin[p->size] = '\0';
    }
}

void chars_t_replace_at(chars_t* p, size_t pos, size_t sn, const void* r, size_t rn)
{
    char* begin;
    if (rn > sn) {
        p->size += (rn - sn);
        if (p->size >= p->maxsize) {
            chars_t_resize(p, p->size + 1);
        }
        begin = chars_t_at(p, 0);
        memmove(begin + pos + rn, begin + pos + sn, p->size - pos - rn);
    } else if (rn < sn) {
        begin = chars_t_at(p, 0);
        memmove(begin + pos + rn, begin + pos + sn, p->size - pos - sn);
        p->size -= (sn - rn);
        begin[p->size] = 0;
    } else {
        begin = chars_t_at(p, 0);
    }
    memcpy(begin + pos, r, rn);
    *chars_t_at(p, (int)p->size) = 0;
}

void chars_t_replace(chars_t* p, const void* s, size_t sn, const void* r, size_t rn)
{
    if (NULL == s || NULL == r) {
        return;
    }
    if (sn == rn && memcmp(s, r, sn) == 0) {
        return;
    } else {
        char* FindPos;
        char* sSrc = chars_t_at(p, 0);
        char* begin = sSrc;

        while ((FindPos = memmem(sSrc, p->size, s, sn)) != NULL) {
            chars_t_replace_at(p, FindPos - begin, sn, r, rn);
            sSrc = FindPos + rn;
        }
    }
}

char startsWith(const char* str, const char* start)
{
    if (str == NULL || start == NULL) {
        return 0;
    }
    if (strncmp(str, start, strlen(start)) == 0) {
        return 1;
    }
    return 0;
}

char startsWithn(const char* str, const char* start, size_t startlen)
{
    if (str == NULL || start == NULL) {
        return 0;
    }
    if (strncmp(str, start, startlen) == 0) {
        return 1;
    }
    return 0;
}

char startsWithnn(const char* str, size_t _strlen, const char* start, size_t startlen)
{
    if (str == NULL || start == NULL || _strlen < startlen) {
        return 0;
    }
    if (strncmp(str, start, startlen) == 0) {
        return 1;
    }
    return 0;
}

char endsWith(const char* str, const char* end)
{
    if (str == NULL || end == NULL) {
        return 0;
    } else {
        size_t strLen = strlen(str);
        size_t endLen = strlen(end);
        if (endLen > strLen) {
            return 0;
        }
        return strncmp(str + strLen - endLen, end, endLen) == 0;
    }
}

char endsWithnn(const char* str, size_t strLen, const char* end, size_t endLen)
{
    if (str == NULL || end == NULL) {
        return 0;
    }
    if (endLen > strLen) {
        return 0;
    }
    return strncmp(str + strLen - endLen, end, endLen) == 0;
}

unsigned cstr_split(const char* strline, const char ctab, strseg_t** pValues)
{
    unsigned num = 0, arrsz = 10;
    strseg_t* segs = malloc(sizeof(strseg_t) * arrsz);

    while (strline) {
        char* tp;
        if (num >= arrsz) {
            arrsz = arrsz > 40 ? arrsz + 10 : arrsz << 1;
            segs = realloc(segs, sizeof(strseg_t) * arrsz);
        }
        segs[num].begin = strline;
        tp = strchr(strline, ctab);
        if (tp) {
            segs[num++].len = tp++ - strline;
        } else {
            segs[num++].len = strlen(strline);
        }
        strline = tp;
    }
    *pValues = segs;
    return num;
}

void testchars_t(void)
{
    chars_t teststr;
    strseg_t* segs;
    size_t i;
    int num = cstr_split("11,22,33,44,55,66,,77,,88,99,11,22,33", ',', &segs);
    for (i = 0; i < num; i++) {
        printf("%zu %zu %.*s\n", i, segs[i].len, (int)segs[i].len, segs[i].begin);
    }
    chars_t_init(&teststr, 16);
    chars_t_append(&teststr, "   ", sizeof("   ") - 1);
    assert(teststr.size == sizeof("   ") - 1 && memcmp(chars_t_at(&teststr, 0), "   ", sizeof("   ")) == 0);

    chars_t_append(&teststr, "0123456789abcdef", sizeof("0123456789abcdef") - 1);
    assert(teststr.size == sizeof("   0123456789abcdef") - 1 && memcmp(chars_t_at(&teststr, 0), "   0123456789abcdef", sizeof("   0123456789abcdef")) == 0);

    chars_t_replace(&teststr, "  ", 2, "a", 1);
    assert(teststr.size == sizeof("a 0123456789abcdef") - 1 && memcmp(chars_t_at(&teststr, 0), "a 0123456789abcdef", sizeof("a 0123456789abcdef")) == 0);

    chars_t_replace(&teststr, " ", 1, "zyz", 3);
    assert(teststr.size == sizeof("azyz0123456789abcdef") - 1 && memcmp(chars_t_at(&teststr, 0), "azyz0123456789abcdef", sizeof("azyz0123456789abcdef")) == 0);

    // chars_t_lstrip(&teststr);
    chars_t_rstrip(&teststr);
    chars_t_strip(&teststr);

    chars_t_clear(&teststr);
    chars_t_append(&teststr, "aaa", sizeof("aaa") - 1);
    chars_t_lstrip(&teststr);
    chars_t_rstrip(&teststr);
    chars_t_strip(&teststr);

    chars_t_clear(&teststr);
    chars_t_append(&teststr, " \taaa \t", sizeof(" \taaa \t") - 1);
    chars_t_lstrip(&teststr);
    chars_t_rstrip(&teststr);

    chars_t_clear(&teststr);
    chars_t_append(&teststr, " \taaa \t", sizeof(" \taaa \t") - 1);
    chars_t_strip(&teststr);

    chars_t_push_back(&teststr, "b");
    chars_t_pop_back(&teststr);
    chars_t_insert(&teststr, 3, "xyzxyz", sizeof("xyzxyz") - 1);
    chars_t_del(&teststr, 3, 3);
    chars_t_insert(&teststr, 2, "xyzxyz", sizeof("xyzxyz") - 1);

    chars_t_cleanup(&teststr);
}
