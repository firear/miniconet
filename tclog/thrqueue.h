/*
 * SSLsplit - transparent and scalable SSL/TLS interception
 * Copyright (c) 2009-2014, Daniel Roethlisberger <daniel@roe.ch>
 * All rights reserved.
 * http://www.roe.ch/SSLsplit
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef THRQUEUE_H
#define THRQUEUE_H
#include <stdlib.h>

typedef struct thrqueue thrqueue_t;

#ifdef __cplusplus
extern "C" {
#endif

thrqueue_t * thrqueue_new(size_t);
void thrqueue_free(thrqueue_t *);

void * thrqueue_enqueue(thrqueue_t *, void *);
void * thrqueue_enqueue_nb(thrqueue_t *, void *);
void * thrqueue_dequeue(thrqueue_t *);
void * thrqueue_dequeue_nb(thrqueue_t *);
void thrqueue_unblock_enqueue(thrqueue_t *);
void thrqueue_unblock_dequeue(thrqueue_t *);

// ATTENTION:for multi in one out
void ** thrqueue_enq_getstub(thrqueue_t *);
void thrqueue_enq_done(thrqueue_t *);
/* 批量出队 + 节点交换。buf[0..max-1] 进/出都是"空节点"：
 * 取走 k 个满节点放进 buf[0..k-1]，同时把等量的空节点留在腾出的槽位里。
 *
 * 调用方拿到满节点后在锁外处理，处理完它们自然又变回空节点，下一轮直接用。
 * 这样节点所有权始终单一（生产者 / 队列 / 消费者 三者之一），不需要空闲链表、
 * 也不需要每消息 malloc——而且消费者持有整批节点期间，生产者不可能再碰到
 * 它们（槽位里已被换成空节点），从而消除了"消费者还没写、槽位就被覆写"
 * 的竞争。 */
size_t thrqueue_dequeue_swap(thrqueue_t *, void **buf, size_t max);
//

void * thrqueue_dequeue_timedwait(thrqueue_t *, size_t millseconds);

#ifdef __cplusplus
}
#endif

#endif /* !THRQUEUE_H */

