#include "rbmap.h"
#include "hdef.h"
#include "hmutex.h"
#include <stdlib.h>

struct rbmap_s {
    struct rb_root rb_root;
    RBFREE_FUN freekey_fun;
    RBFREE_FUN freeval_fun;
    COMPAREFUN cmp_fun;
    hmutex_t mutex;
};

int cmp_i(const rbmap_key_u* a, const rbmap_key_u* b)
{
    if (a->ku64 > b->ku64) {
        return 1;
    } else if (a->ku64 == b->ku64) {
        return 0;
    } else {
        return -1;
    }
}

int cmp_m(const rbmap_key_u* a, const rbmap_key_u* b)
{
    uint8_t littlelen;
    int ret;
    if (a->_kplainlen && b->_kplainlen) {
        littlelen = a->_kplainlen <= b->_kplainlen ? a->_kplainlen : b->_kplainlen;
        ret = memcmp(a->_kplain, b->_kplain, littlelen);
        if (ret == 0 && a->_kplainlen != b->_kplainlen) {
            ret = a->_kplainlen > b->_kplainlen ? 1 : -1;
        }
    } else {
        if (a->kp == b->kp) {
            return 0;
        }
        littlelen = a->kplen <= b->kplen ? a->kplen : b->kplen;
        ret = memcmp(a->kp, b->kp, littlelen);
        if (ret == 0 && a->kplen != b->kplen) {
            ret = a->kplen > b->kplen ? 1 : -1;
        }
    }
    return ret;
}

rbmap_t* rbmap_init(RBFREE_FUN freekey, RBFREE_FUN freeval, COMPAREFUN cmpfun)
{
    rbmap_t* cm;
    SAFE_ALLOC(cm, sizeof(rbmap_t));
    cm->rb_root.rb_node = NULL;
    cm->freekey_fun = freekey;
    cm->freeval_fun = freeval;
    cm->cmp_fun = cmpfun;
    hmutex_init(&cm->mutex);
    return cm;
}

void rbmap_deinit(rbmap_t* cm)
{
    struct rb_node* n = NULL;
    rbmap_entry_t* e = NULL;
    while ((n = cm->rb_root.rb_node)) {
        e = rb_entry(n, rbmap_entry_t, _rb_node);
        rb_erase(n, &cm->rb_root);
        if (cm->freekey_fun) {
            cm->freekey_fun((void*)e->key.kp);
        }
        if (cm->freeval_fun) {
            cm->freeval_fun(e->val);
        }
        SAFE_FREE(e);
    }
    hmutex_destroy(&cm->mutex);
    SAFE_FREE(cm);
}

// compar return a-b //search malloc insert
rbmap_entry_t* rbmap_get(rbmap_t* root, const void* key, uint8_t keylen)
{
    struct rb_node** n = &root->rb_root.rb_node;
    struct rb_node* parent = NULL;
    rbmap_entry_t* e = NULL;
    rbmap_key_u searchkey = { 0 };
    if (root->cmp_fun == cmp_i) {
        searchkey.ku64 = (uint64_t)key;
    } else if (keylen <= __KEYSIZE) {
        searchkey._kplainlen = keylen;
        memcpy(searchkey._kplain, key, keylen);
    } else {
        searchkey.kp = key;
        searchkey.kplen = keylen;
    }
    hmutex_lock(&root->mutex);
    while (*n) {
        int cmp;
        parent = *n;
        e = rb_entry(*n, rbmap_entry_t, _rb_node);
        cmp = root->cmp_fun(&searchkey, &e->key);
        if (cmp < 0) {
            n = &(*n)->rb_left;
        } else if (cmp > 0) {
            n = &(*n)->rb_right;
        } else {
            goto END;
        }
    }

    SAFE_ALLOC(e, sizeof(rbmap_entry_t));
    e->key = searchkey;
    rb_link_node(&e->_rb_node, parent, n);
    rb_insert_color(&e->_rb_node, &root->rb_root);
END:
    hmutex_unlock(&root->mutex);
    return e;
}

int rbmap_remove(rbmap_t* root, rbmap_entry_t* e, uint8_t withlock)
{
    if (withlock) {
        hmutex_lock(&root->mutex);
    }
    rb_erase(&e->_rb_node, &root->rb_root);
    if (withlock) {
        hmutex_unlock(&root->mutex);
    }
    if (root->freekey_fun) {
        root->freekey_fun((void*)e->key.kp);
    }
    if (root->freeval_fun) {
        root->freeval_fun(e->val);
    }
    return 0;
}

rbmap_entry_t* rbmap_search(rbmap_t* root, const void* key, uint8_t keylen)
{
    struct rb_node* n = root->rb_root.rb_node;
    rbmap_entry_t* e = NULL;
    rbmap_key_u searchkey = { 0 };
    if (root->cmp_fun == cmp_i) {
        searchkey.ku64 = (uint64_t)key;
    } else if (keylen <= __KEYSIZE) {
        searchkey._kplainlen = keylen;
        memcpy(searchkey._kplain, key, keylen);
    } else {
        searchkey.kp = key;
        searchkey.kplen = keylen;
    }
    hmutex_lock(&root->mutex);
    while (n) {
        int cmpret;
        e = rb_entry(n, rbmap_entry_t, _rb_node);
        cmpret = root->cmp_fun(&searchkey, &e->key);
        if (cmpret < 0) {
            n = n->rb_left;
        } else if (cmpret > 0) {
            n = n->rb_right;
        } else {
            hmutex_unlock(&root->mutex);
            return e;
        }
    }
    hmutex_unlock(&root->mutex);
    return NULL;
}

void rbmap_for_each(rbmap_t* m, void (*cb)(rbmap_entry_t* e, void* user), void* user)
{
    struct rb_node* node;
    hmutex_lock(&m->mutex);
    for (node = rb_first(&m->rb_root); node; node = rb_next(node)) {
        rbmap_entry_t* e = rb_entry(node, rbmap_entry_t, _rb_node);
        cb(e, user);
    }
    hmutex_unlock(&m->mutex);
}

void rbmap_for_each_safe(rbmap_t* m, void (*cb)(rbmap_entry_t* e, void* user), void* user)
{
    struct rb_node *pos, *n;
    hmutex_lock(&m->mutex);
    for (pos = rb_first(&m->rb_root), n = rb_next(pos); pos; pos = n, n = rb_next(pos)) {
        rbmap_entry_t* e = rb_entry(pos, rbmap_entry_t, _rb_node);
        cb(e, user);
    }
    hmutex_unlock(&m->mutex);
}

///////////////////////////