/*
 * Copyright (c) 2022 Andreas Rheinhardt <andreas.rheinhardt@outlook.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <limits.h>
#include <stdatomic.h>

#include "pthread_internal.h"
#include "threadprogress.h"
#include "libavutil/attributes.h"
#include "libavutil/thread.h"

DEFINE_OFFSET_ARRAY(ThreadProgress, thread_progress, init,
                    (offsetof(ThreadProgress, progress_mutex)),
                    (offsetof(ThreadProgress, progress_cond)));

av_cold int ff_thread_progress_init(ThreadProgress *pro, int init_mode)
{
    atomic_init(&pro->progress, init_mode ? -1 : INT_MAX);
    atomic_init(&pro->waiters, 0);
#if HAVE_THREADS
    if (init_mode)
        return ff_pthread_init(pro, thread_progress_offsets);
#endif
    pro->init = init_mode;
    return 0;
}

av_cold void ff_thread_progress_destroy(ThreadProgress *pro)
{
#if HAVE_THREADS
    ff_pthread_free(pro, thread_progress_offsets);
#else
    pro->init = 0;
#endif
}

void ff_thread_progress_report(ThreadProgress *pro, int n)
{
    if (atomic_load_explicit(&pro->progress, memory_order_relaxed) >= n)
        return;

    /* Custom (MV-only fork): publish, then check for sleepers. seq_cst on this
     * store and on the waiter-count load (and on the waiter's increment +
     * re-check below) means one side always observes the other: either we see
     * the waiter's increment and broadcast, or its re-check under the mutex
     * sees this store and it never sleeps. Same reasoning and the same
     * measured motivation as ff_thread_report_progress() in pthread_frame.c. */
    atomic_store_explicit(&pro->progress, n, memory_order_seq_cst);

    if (!atomic_load_explicit(&pro->waiters, memory_order_seq_cst))
        return;

    ff_mutex_lock(&pro->progress_mutex);
    ff_cond_broadcast(&pro->progress_cond);
    ff_mutex_unlock(&pro->progress_mutex);
}

void ff_thread_progress_await(const ThreadProgress *pro_c, int n)
{
    /* Casting const away here is safe, because we only read from progress
     * and will leave pro_c in the same state upon leaving the function
     * as it had at the beginning. */
    ThreadProgress *pro = (ThreadProgress*)pro_c;

    if (atomic_load_explicit(&pro->progress, memory_order_acquire) >= n)
        return;

    /* Custom (MV-only fork): register before re-checking progress; pairs with
     * the reporter's store-then-load above. */
    atomic_fetch_add_explicit(&pro->waiters, 1, memory_order_seq_cst);
    ff_mutex_lock(&pro->progress_mutex);
    while (atomic_load_explicit(&pro->progress, memory_order_relaxed) < n)
        ff_cond_wait(&pro->progress_cond, &pro->progress_mutex);
    ff_mutex_unlock(&pro->progress_mutex);
    atomic_fetch_sub_explicit(&pro->waiters, 1, memory_order_seq_cst);
}
