/*
 * lwan - web server
 * Copyright (c) 2012 L. A. F. Pereira <l@tia.mat.br>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

#define _GNU_SOURCE
#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "lwan-private.h"

#include "lwan-arena.h"
#include "lwan-array.h"
#include "lwan-coro.h"

#if !defined(NDEBUG) && defined(LWAN_HAVE_VALGRIND)
#define INSTRUMENT_FOR_VALGRIND
#include <valgrind.h>
#include <memcheck.h>
#endif

#if !defined(SIGSTKSZ)
#define SIGSTKSZ 16384
#endif

#ifdef LWAN_HAVE_BROTLI
#define MIN_CORO_STACK_SIZE ((size_t)(8 * SIGSTKSZ))
#else
#define MIN_CORO_STACK_SIZE ((size_t)(4 * SIGSTKSZ))
#endif

#define CORO_STACK_SIZE ((MIN_CORO_STACK_SIZE + (size_t)PAGE_SIZE) & ~((size_t)PAGE_SIZE))

#if (!defined(NDEBUG) && defined(MAP_STACK)) || defined(__OpenBSD__)
/* As an exploit mitigation, OpenBSD requires any stacks to be allocated via
 * mmap(...  MAP_STACK ...).
 *
 * Also enable this on debug builds to catch stack overflows while testing
 * (MAP_STACK exists in Linux, but it's a no-op).  */
#define ALLOCATE_STACK_WITH_MMAP
#endif

LWAN_SELF_TEST(sizes_are_same)
{
    /* This is done in runtime rather than during compilation time because
     * in Glibc >= 2.34, SIGSTKSZ is defined as sysconf(_SC_MINSIGSTKSZ). */

    /* Request buffer fits inside coroutine stack */
    assert(DEFAULT_BUFFER_SIZE < CORO_STACK_SIZE);
}

typedef void (*defer1_func)(void *data);
typedef void (*defer2_func)(void *data1, void *data2);

struct coro_defer {
    union {
        struct {
            defer1_func func;
            void *data;
        } one;
        struct {
            defer2_func func;
            void *data1;
            void *data2;
        } two;
    };
    bool has_two_args;
};

DEFINE_ARRAY_TYPE_INLINEFIRST(coro_defer_array, struct coro_defer)

struct coro {
    coro_context *switcher;
    coro_context context;
    struct coro_defer_array defer;

    int64_t yield_value;

    struct arena arena;

#if defined(INSTRUMENT_FOR_VALGRIND)
    unsigned int vg_stack_id;
#endif

#if defined(ALLOCATE_STACK_WITH_MMAP)
    unsigned char *stack;
#else
    unsigned char stack[];
#endif
};

#if defined(__APPLE__)
#define ASM_SYMBOL(name_) "_" #name_
#else
#define ASM_SYMBOL(name_) #name_
#endif

#define ASM_ROUTINE(name_)                                                     \
    ".globl " ASM_SYMBOL(name_) "\n\t" ASM_SYMBOL(name_) ":\n\t"

/*
 * This swapcontext() implementation was obtained from glibc and modified
 * slightly to not save/restore the floating point registers, unneeded
 * registers, and signal mask.  It is Copyright (C) 2001, 2002, 2003 Free
 * Software Foundation, Inc and is distributed under GNU LGPL version 2.1
 * (or later).  I'm not sure if I can distribute them inside a GPL program;
 * they're straightforward so I'm assuming there won't be any problem; if
 * there is, I'll just roll my own.
 *     -- L.
 */
#if defined(__x86_64__)
void __attribute__((noinline, visibility("internal")))
coro_swapcontext(coro_context *current, coro_context *other);
asm(".text\n\t"
    ".p2align 5\n\t"
    ASM_ROUTINE(coro_swapcontext)
    "movq   %rbx,0(%rdi)\n\t"
    "movq   %rbp,8(%rdi)\n\t"
    "movq   %r12,16(%rdi)\n\t"
    "movq   %r13,24(%rdi)\n\t"
    "movq   %r14,32(%rdi)\n\t"
    "movq   %r15,40(%rdi)\n\t"
    "movq   %rdi,48(%rdi)\n\t"
    "movq   %rsi,56(%rdi)\n\t"
    "movq   (%rsp),%rcx\n\t"
    "movq   %rcx,64(%rdi)\n\t"
    "leaq   0x8(%rsp),%rcx\n\t"
    "movq   %rcx,72(%rdi)\n\t"
    "movq   72(%rsi),%rsp\n\t"
    "movq   0(%rsi),%rbx\n\t"
    "movq   8(%rsi),%rbp\n\t"
    "movq   16(%rsi),%r12\n\t"
    "movq   24(%rsi),%r13\n\t"
    "movq   32(%rsi),%r14\n\t"
    "movq   40(%rsi),%r15\n\t"
    "movq   48(%rsi),%rdi\n\t"
    "movq   64(%rsi),%rcx\n\t"
    "movq   56(%rsi),%rsi\n\t"
    "jmpq   *%rcx\n\t");
#elif defined(__aarch64__)
void __attribute__((noinline, visibility("internal")))
coro_swapcontext(coro_context *current, coro_context *other);
asm(".text\n\t"
    ".p2align 5\n\t"
    ASM_ROUTINE(coro_swapcontext)
    "mov x10, sp\n\t"
    "mov x11, x30\n\t"
    "stp x8, x9, [x0, #(1*16)]\n\t"
    "stp x10, x11, [x0, #(2*16)]\n\t"
    "stp x12, x13, [x0, #(3*16)]\n\t"
    "stp x14, x15, [x0, #(4*16)]\n\t"
    "stp x19, x20, [x0, #(5*16)]\n\t"
    "stp x21, x22, [x0, #(6*16)]\n\t"
    "stp x23, x24, [x0, #(7*16)]\n\t"
    "stp x25, x26, [x0, #(8*16)]\n\t"
    "stp x27, x28, [x0, #(9*16)]\n\t"
    "stp x29, x30, [x0, #(10*16)]\n\t"
    "stp x0, x1, [x0, #(0*16)]\n\t"
    "ldp x8, x9, [x1, #(1*16)]\n\t"
    "ldp x10, x11, [x1, #(2*16)]\n\t"
    "ldp x12, x13, [x1, #(3*16)]\n\t"
    "ldp x14, x15, [x1, #(4*16)]\n\t"
    "ldp x19, x20, [x1, #(5*16)]\n\t"
    "ldp x21, x22, [x1, #(6*16)]\n\t"
    "ldp x23, x24, [x1, #(7*16)]\n\t"
    "ldp x25, x26, [x1, #(8*16)]\n\t"
    "ldp x27, x28, [x1, #(9*16)]\n\t"
    "ldp x29, x30, [x1, #(10*16)]\n\t"
    "ldp x0, x1, [x1, #(0*16)]\n\t"
    "mov sp, x10\n\t"
    "br x11\n\t");
#elif defined(LWAN_HAVE_LIBUCONTEXT)
#define coro_swapcontext(cur, oth) libucontext_swapcontext(cur, oth)
#else
#error Unsupported platform.
#endif

__attribute__((used, visibility("internal")))
void
coro_entry_point(struct coro *coro, coro_function_t func, void *data)
{
    return (void)coro_yield(coro, func(coro, data));
}

#ifdef __x86_64__
/* See comment in coro_reset() for an explanation of why this routine is
 * necessary. */
void __attribute__((visibility("internal"))) coro_entry_point_x86_64();

asm(".text\n\t"
    ".p2align 5\n\t"
    ASM_ROUTINE(coro_entry_point_x86_64)
    "mov %r15, %rdx\n\t"
    "jmp " ASM_SYMBOL(coro_entry_point) "\n\t"
);
#elif defined(__aarch64__)
void __attribute__((visibility("internal"))) coro_entry_point_arm64();

asm(".text\n\t"
    ".p2align 5\n\t"
    ASM_ROUTINE(coro_entry_point_arm64)
    "mov x2, x28\n\t"
    "bl " ASM_SYMBOL(coro_entry_point) "\n\t"
);
#endif

void coro_deferred_run(struct coro *coro, size_t generation)
{
    struct lwan_array *array = (struct lwan_array *)&coro->defer;
    struct coro_defer *defers = array->base;

    for (size_t i = array->elements; i != generation; i--) {
        struct coro_defer *defer = &defers[i - 1];

        if (defer->has_two_args)
            defer->two.func(defer->two.data1, defer->two.data2);
        else
            defer->one.func(defer->one.data);
    }

    array->elements = generation;
}

ALWAYS_INLINE size_t coro_deferred_get_generation(const struct coro *coro)
{
    const struct lwan_array *array = (struct lwan_array *)&coro->defer;

    return array->elements;
}

void coro_reset(struct coro *coro, coro_function_t func, void *data)
{
    unsigned char *stack = coro->stack;

    coro_deferred_run(coro, 0);
    arena_reset(&coro->arena);
    coro_defer_array_reset(&coro->defer);

#if defined(__x86_64__)
    /* coro_entry_point() for x86-64 has 3 arguments, but RDX isn't
     * stored.  Use R15 instead, and implement the trampoline
     * function in assembly in order to use this register when
     * calling the user function. */
    coro->context[5 /* R15 */] = (uintptr_t)data;
    coro->context[6 /* RDI */] = (uintptr_t)coro;
    coro->context[7 /* RSI */] = (uintptr_t)func;
    coro->context[8 /* RIP */] = (uintptr_t)coro_entry_point_x86_64;

    /* Ensure stack is properly aligned: it should be aligned to a
     * 16-bytes boundary so SSE will work properly, but should be
     * aligned on an 8-byte boundary right after calling a function. */
    uintptr_t rsp = (uintptr_t)stack + CORO_STACK_SIZE;

#define STACK_PTR 9
    coro->context[STACK_PTR] = (rsp & ~0xful) - 0x8ul;
#elif defined(__aarch64__)
    coro->context[19/* x28 */] = (uintptr_t)data;
    coro->context[0 /* x0  */] = (uintptr_t)coro;
    coro->context[1 /* x1  */] = (uintptr_t)func;
    coro->context[5 /* lr  */] = (uintptr_t)coro_entry_point_arm64;

    uintptr_t rsp = (uintptr_t)stack + CORO_STACK_SIZE;
#define STACK_PTR 4
    coro->context[STACK_PTR] = rsp & ~0xful;
#elif defined(LWAN_HAVE_LIBUCONTEXT)
    libucontext_getcontext(&coro->context);

    coro->context.uc_stack.ss_sp = stack;
    coro->context.uc_stack.ss_size = CORO_STACK_SIZE;
    coro->context.uc_stack.ss_flags = 0;
    coro->context.uc_link = NULL;

    libucontext_makecontext(&coro->context, (void (*)())coro_entry_point, 3,
                            coro, func, data);

#endif
}

ALWAYS_INLINE struct coro *
coro_new(coro_context *switcher, coro_function_t function, void *data)
{
    struct coro *coro;

#if defined(ALLOCATE_STACK_WITH_MMAP)
    void *stack = mmap(NULL, CORO_STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_STACK | MAP_ANON | MAP_PRIVATE, -1, 0);
    if (UNLIKELY(stack == MAP_FAILED))
        return NULL;

    coro = lwan_aligned_alloc(sizeof(*coro), 64);
    if (UNLIKELY(!coro)) {
        munmap(stack, CORO_STACK_SIZE);
        return NULL;
    }

    coro->stack = stack;
#else
    coro = lwan_aligned_alloc(sizeof(struct coro) + CORO_STACK_SIZE, 64);

    if (UNLIKELY(!coro))
        return NULL;
#endif

    coro_defer_array_init(&coro->defer);
    arena_init(&coro->arena);

    coro->switcher = switcher;
    coro_reset(coro, function, data);

#if defined(INSTRUMENT_FOR_VALGRIND)
    coro->vg_stack_id = VALGRIND_STACK_REGISTER(
        coro->stack, (char *)coro->stack + CORO_STACK_SIZE);
#endif

    return coro;
}

ALWAYS_INLINE int64_t coro_resume(struct coro *coro)
{
    assert(coro);

#if defined(STACK_PTR)
    assert(coro->context[STACK_PTR] >= (uintptr_t)coro->stack &&
           coro->context[STACK_PTR] <=
               (uintptr_t)(coro->stack + CORO_STACK_SIZE));
#endif

    coro_swapcontext(coro->switcher, &coro->context);

    return coro->yield_value;
}

ALWAYS_INLINE int64_t coro_resume_value(struct coro *coro, int64_t value)
{
    assert(coro);

    coro->yield_value = value;
    return coro_resume(coro);
}

inline int64_t coro_yield(struct coro *coro, int64_t value)
{
    assert(coro);

    coro->yield_value = value;
    coro_swapcontext(&coro->context, coro->switcher);

    return coro->yield_value;
}

void coro_free(struct coro *coro)
{
    assert(coro);

    coro_deferred_run(coro, 0);
    arena_reset(&coro->arena);
    coro_defer_array_reset(&coro->defer);

#if defined(INSTRUMENT_FOR_VALGRIND)
    VALGRIND_STACK_DEREGISTER(coro->vg_stack_id);
#endif

#if defined(ALLOCATE_STACK_WITH_MMAP)
    int result = munmap(coro->stack, CORO_STACK_SIZE);
    assert(result == 0);  /* only fails if addr, len are invalid */
#endif

    free(coro);
}

static void disarmed_defer(void *data __attribute__((unused)))
{
}

static void coro_defer_disarm_internal(struct coro *coro,
                                       struct coro_defer *defer)
{
    const size_t num_defers = coro_defer_array_len(&coro->defer);

    assert(num_defers != 0 && defer != NULL);

    if (defer == coro_defer_array_get_elem(&coro->defer, num_defers - 1)) {
        /* If we're disarming the last defer we armed, there's no need to waste
         * space of a deferred callback to an empty function like
         * disarmed_defer(). */
        struct lwan_array *defer_base = (struct lwan_array *)&coro->defer;
        defer_base->elements--;
    } else {
        defer->one.func = disarmed_defer;
        defer->has_two_args = false;
    }
}

void coro_defer_disarm(struct coro *coro, coro_deferred d)
{
    assert(d >= 0);

    return coro_defer_disarm_internal(
        coro, coro_defer_array_get_elem(&coro->defer, (size_t)d));
}

void coro_defer_fire_and_disarm(struct coro *coro, coro_deferred d)
{
    assert(d >= 0);

    struct coro_defer *defer = coro_defer_array_get_elem(&coro->defer, (size_t)d);
    assert(coro);

    if (defer->has_two_args)
        defer->two.func(defer->two.data1, defer->two.data2);
    else
        defer->one.func(defer->one.data);

    return coro_defer_disarm_internal(coro, defer);
}

ALWAYS_INLINE coro_deferred
coro_defer(struct coro *coro, defer1_func func, void *data)
{
    struct coro_defer *defer = coro_defer_array_append(&coro->defer);

    if (UNLIKELY(!defer)) {
        lwan_status_error("Could not add new deferred function for coro %p",
                          coro);
        return -1;
    }

    defer->one.func = func;
    defer->one.data = data;
    defer->has_two_args = false;

    return (coro_deferred)coro_defer_array_get_elem_index(&coro->defer, defer);
}

ALWAYS_INLINE coro_deferred
coro_defer2(struct coro *coro, defer2_func func, void *data1, void *data2)
{
    struct coro_defer *defer = coro_defer_array_append(&coro->defer);

    if (UNLIKELY(!defer)) {
        lwan_status_error("Could not add new deferred function for coro %p",
                          coro);
        return -1;
    }

    defer->two.func = func;
    defer->two.data1 = data1;
    defer->two.data2 = data2;
    defer->has_two_args = true;

    return (coro_deferred)coro_defer_array_get_elem_index(&coro->defer, defer);
}

void *coro_malloc_full(struct coro *coro,
                       size_t size,
                       void (*destroy_func)(void *data))
{
    void *ptr = malloc(size);
    if (LIKELY(ptr))
        coro_defer(coro, destroy_func, ptr);

    return ptr;
}

void *coro_malloc(struct coro *coro, size_t size)
{
    return arena_alloc(&coro->arena, size);
}

char *coro_strndup(struct coro *coro, const char *str, size_t max_len)
{
    const size_t len = strnlen(str, max_len) + 1;
    char *dup = coro_memdup(coro, str, len);

    if (LIKELY(dup))
        dup[len - 1] = '\0';

    return dup;
}

char *coro_strdup(struct coro *coro, const char *str)
{
    return coro_memdup(coro, str, strlen(str) + 1);
}

char *coro_printf(struct coro *coro, const char *fmt, ...)
{
    va_list values;
    int len;
    char *tmp_str;

    va_start(values, fmt);
    len = vasprintf(&tmp_str, fmt, values);
    va_end(values);

    if (UNLIKELY(len < 0))
        return NULL;

    coro_defer(coro, free, tmp_str);
    return tmp_str;
}

void *coro_memdup(struct coro *coro, const void *src, size_t len)
{
    void *ptr = coro_malloc(coro, len);

    return LIKELY(ptr) ? memcpy(ptr, src, len) : NULL;
}
