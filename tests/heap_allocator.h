#pragma once

/* A malloc-based allocator_t for tests.  The arenas the tests otherwise use
 * never free anything one by one, so they hide use-after-free and leaks;
 * with heap() both show up under ./test.sh asan (AddressSanitizer and
 * LeakSanitizer). */

#include <stdlib.h>

#include "arena/allocator.h"

static void *heap_alloc(void *ctx, size_t size, size_t align)
{
    (void)ctx;
    (void)align; /* malloc aligns for every type the path code allocates */
    return malloc(size);
}

static void *heap_realloc(
    void *ctx, void *ptr, size_t old_size, size_t new_size, size_t align)
{
    (void)ctx;
    (void)old_size;
    (void)align;
    return realloc(ptr, new_size);
}

static void heap_free(void *ctx, void *ptr, size_t size)
{
    (void)ctx;
    (void)size;
    free(ptr);
}

static const allocator_vtable_t heap_vtable = {
    heap_alloc, heap_realloc, heap_free};

static allocator_t heap(void)
{
    return (allocator_t){&heap_vtable, NULL};
}

/* heap() with a budget: *budget allocations (alloc or realloc) succeed,
 * then every one fails until the budget is raised again.  For walking
 * every out-of-memory point of a function. */
static void *budget_alloc(void *ctx, size_t size, size_t align)
{
    size_t *budget = ctx;
    if (*budget == 0)
    {
        return NULL;
    }
    --*budget;
    return heap_alloc(NULL, size, align);
}

static void *budget_realloc(
    void *ctx, void *ptr, size_t old_size, size_t new_size, size_t align)
{
    size_t *budget = ctx;
    if (*budget == 0)
    {
        return NULL;
    }
    --*budget;
    return heap_realloc(NULL, ptr, old_size, new_size, align);
}

static const allocator_vtable_t budget_vtable = {
    budget_alloc, budget_realloc, heap_free};

static inline allocator_t heap_with_budget(size_t *budget)
{
    return (allocator_t){&budget_vtable, budget};
}
