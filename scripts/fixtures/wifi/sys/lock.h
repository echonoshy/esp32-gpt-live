#pragma once
#include <pthread.h>
#include <assert.h>
typedef int _lock_t;
static pthread_mutex_t test_lock = PTHREAD_MUTEX_INITIALIZER;
static inline void _lock_acquire(_lock_t *lock) { (void)lock; assert(!pthread_mutex_lock(&test_lock)); }
static inline void _lock_release(_lock_t *lock) { (void)lock; assert(!pthread_mutex_unlock(&test_lock)); }
