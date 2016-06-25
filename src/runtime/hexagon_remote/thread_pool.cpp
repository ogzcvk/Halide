#include <HalideRuntime.h>

extern "C" {
#include <qurt.h>
#include <stdlib.h>
#include <memory.h>
}

#include "log.h"


struct halide_thread {
    qurt_thread_t val;
};

int halide_host_cpu_count() {
    // Assume a Snapdragon 820
    return 4;
}

struct spawned_thread {
    void (*f)(void *);
    void *closure;
    void *stack;
    halide_thread handle;
};
void spawn_thread_helper(void *arg) {
    spawned_thread *t = (spawned_thread *)arg;
    t->f(t->closure);
}

#define STACK_SIZE 256*1024

struct halide_thread *halide_spawn_thread(void (*f)(void *), void *closure) {
    spawned_thread *t = (spawned_thread *)malloc(sizeof(spawned_thread));
    t->f = f;
    t->closure = closure;
    t->stack = memalign(128, STACK_SIZE);
    memset(&t->handle, 0, sizeof(t->handle));
    qurt_thread_attr_t thread_attr;
    qurt_thread_attr_init(&thread_attr);
    qurt_thread_attr_set_stack_addr(&thread_attr, t->stack);
    qurt_thread_attr_set_stack_size(&thread_attr, STACK_SIZE);
    qurt_thread_attr_set_priority(&thread_attr, 100);
    qurt_thread_create(&t->handle.val, &thread_attr, &spawn_thread_helper, t);
    return (halide_thread *)t;
}

void halide_join_thread(struct halide_thread *thread_arg) {
    spawned_thread *t = (spawned_thread *)thread_arg;
    int ret = 0;
    qurt_thread_join(t->handle.val, &ret);
    free(t->stack);
    free(t);
}

void halide_mutex_lock(halide_mutex *mutex) {
    qurt_mutex_lock((qurt_mutex_t *)mutex);
}

void halide_mutex_unlock(halide_mutex *mutex) {
    qurt_mutex_unlock((qurt_mutex_t *)mutex);
}

void halide_mutex_destroy(halide_mutex *mutex) {
    qurt_mutex_destroy((qurt_mutex_t *)mutex);
    memset(mutex, 0, sizeof(halide_mutex));
}

void halide_cond_init(struct halide_cond *cond) {
    qurt_cond_init((qurt_cond_t *)cond);
}

void halide_cond_destroy(struct halide_cond *cond) {
    qurt_cond_destroy((qurt_cond_t *)cond);
}

void halide_cond_broadcast(struct halide_cond *cond) {
    qurt_cond_broadcast((qurt_cond_t *)cond);
}

void halide_cond_wait(struct halide_cond *cond, struct halide_mutex *mutex) {
    qurt_cond_wait((qurt_cond_t *)cond, (qurt_mutex_t *)mutex);
}


#define WEAK
#include "../thread_pool_common.h"

// We wrap the closure passed to jobs with extra info we
// need. Currently just the hvx mode to use.
struct wrapped_closure {
    uint8_t *closure;
    int hvx_mode;
};

extern "C" {
int halide_do_par_for(void *user_context,
                      halide_task_t task,
                      int min, int size, uint8_t *closure) {
    // Get the work queue mutex. We need to do a handful of hexagon-specific things.
    qurt_mutex_t *mutex = (qurt_mutex_t *)(&work_queue.mutex);

    if (!work_queue.initialized) {
        // The thread pool asssumes that a zero-initialized mutex can
        // be locked. Not true on hexagon, and there doesn't seem to
        // be an init_once mechanism either. In this shim binary, it's
        // safe to assume that the first call to halide_do_par_for is
        // done by the main thread, so there's no race condition on
        // initializing this mutex.
        qurt_mutex_init(mutex);
    }
    wrapped_closure c = {closure, qurt_hvx_get_mode()};

    // Set the desired number of threads based on the current HVX
    // mode.
    qurt_mutex_lock(mutex);
    int old_desired_num_threads = work_queue.desired_num_threads;
    work_queue.desired_num_threads = (c.hvx_mode == QURT_HVX_MODE_128B) ? 2 : 4;
    qurt_mutex_unlock(mutex);

    qurt_hvx_unlock();
    int ret = Halide::Runtime::Internal::default_do_par_for(user_context, task, min, size, (uint8_t *)&c);
    qurt_hvx_lock((qurt_hvx_mode_t)c.hvx_mode);

    // Set the desired number of threads back to what it was, in case
    // we're a 128 job and we were sharing the machine with a 64 job.
    qurt_mutex_lock(mutex);
    work_queue.desired_num_threads = old_desired_num_threads;
    qurt_mutex_unlock(mutex);
    return ret;
}

int halide_do_task(void *user_context, halide_task_t f,
                   int idx, uint8_t *closure) {
    // Dig the appropriate hvx mode out of the wrapped closure and lock it.
    wrapped_closure *c = (wrapped_closure *)closure;
    qurt_hvx_lock((qurt_hvx_mode_t)c->hvx_mode);
    int ret = f(user_context, idx, c->closure);
    qurt_hvx_unlock();
    return ret;
}
}
