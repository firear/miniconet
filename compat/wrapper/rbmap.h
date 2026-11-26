#ifndef __RBMAP_H__
#define __RBMAP_H__
#include "hexport.h"
#include "rbtree.h"
#include "vc6_stdint.h"

typedef struct rbmap_s rbmap_t;
#define __KEYSIZE 31
#define RBMAP_IKEY    void*)(long
typedef union {
    struct {
        const void* kp;
        uint8_t kplen;
    };
    uint64_t ku64;
    struct {
        uint8_t _kplain[__KEYSIZE];
        uint8_t _kplainlen;
    };
} rbmap_key_u;

typedef struct {
    struct rb_node _rb_node;
    rbmap_key_u key;
    void* val;
} rbmap_entry_t;

typedef int (*COMPAREFUN)(const rbmap_key_u*, const rbmap_key_u*);
typedef void (*RBFREE_FUN)(void*);

#ifdef __cplusplus
extern "C" {
#endif

// uint64_t
HV_EXPORT int cmp_i(const rbmap_key_u* a, const rbmap_key_u* b);
// memcmp
HV_EXPORT int cmp_m(const rbmap_key_u* a, const rbmap_key_u* b);

// rbmap_init(NULL, NULL, cmp_i or cmp_m or cmp_userdefine);
HV_EXPORT rbmap_t* rbmap_init(RBFREE_FUN freekey, RBFREE_FUN freeval, COMPAREFUN cmpfun);
HV_EXPORT void rbmap_deinit(rbmap_t* cm);
// compar return a-b //search malloc insert;
HV_EXPORT rbmap_entry_t* rbmap_get(rbmap_t* root, const void* key, uint8_t keylen);
// val free outside
HV_EXPORT int rbmap_remove(rbmap_t* root, rbmap_entry_t* entry, uint8_t withlock);

HV_EXPORT rbmap_entry_t* rbmap_search(rbmap_t* root, const void* key, uint8_t kenley);

/**
 * @brief do not call remove during traversal; set val to NULL to mark deletion.
 *
 * @param m
 * @param cb
 * @param user
 */
HV_EXPORT void rbmap_for_each(rbmap_t* m, void (*cb)(rbmap_entry_t* e, void* user), void* user);

/**
 * @brief nodes may be removed during traversal.
 *
 * @param m
 * @param cb
 * @param user
 */
HV_EXPORT void rbmap_for_each_safe(rbmap_t* m, void (*cb)(rbmap_entry_t* e, void* user), void* user);
#ifdef __cplusplus
}
#endif

#endif //__RBMAP_H__
