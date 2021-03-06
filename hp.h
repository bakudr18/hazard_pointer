/* shortcuts */
#define atomic_load(src, memorder) __atomic_load_n(src, memorder)
#define atomic_store(dst, val, memorder) __atomic_store(dst, val, memorder)
#define atomic_exchange(ptr, val, memorder) \
    __atomic_exchange_n(ptr, val, memorder)
#define atomic_cas(dst, expected, desired, success_order, fail_order)   \
    __atomic_compare_exchange(dst, expected, desired, 0, success_order, \
                              fail_order)
#define atomic_test_and_set(ptr, memorder) __atomic_test_and_set(ptr, memorder)
#define atomic_fetch_add(ptr, val, memorder) \
    __atomic_fetch_add(ptr, val, memorder)


#include <stdint.h>

#define LIST_ITER(head, node)                              \
    for (node = atomic_load(head, __ATOMIC_ACQUIRE); node; \
         node = atomic_load(&node->next, __ATOMIC_ACQUIRE))

// #define DO_ANALYSIS

#ifdef DO_ANALYSIS

#define TRACE(ops)                                          \
    do {                                                    \
        atomic_fetch_add(&stats[ops], 1, __ATOMIC_SEQ_CST); \
    } while (0)

#define TRACE_PRINTF(ops) printf("%s: %lu\n", #ops, stats[ops])

enum { TRACE_LOAD_SUCCESS = 0, TRACE_LOAD_FAIL, TRACE_SWAP, TRACE_NUMS };

static uint64_t stats[TRACE_NUMS] = {0};

void trace_print(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    TRACE_PRINTF(TRACE_LOAD_SUCCESS);
    TRACE_PRINTF(TRACE_LOAD_FAIL);
    TRACE_PRINTF(TRACE_SWAP);
}
#define TRACE_PRINT() trace_print()

#else /* DO_ANALYSIS */
#define TRACE(ops)
#define TRACE_PRINT()
#endif /* DO_ANALYSIS */

typedef struct __hp {
    uintptr_t ptr;
    struct __hp *next;
} hp_t;

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Allocate a new node with specified value and append to list */
static hp_t *list_append(hp_t **head, uintptr_t ptr)
{
    hp_t *new = calloc(1, sizeof(hp_t));
    if (!new)
        return NULL;

    new->ptr = ptr;
    hp_t *old = atomic_load(head, __ATOMIC_ACQUIRE);

    do {
        new->next = old;
    } while (!atomic_cas(head, &old, &new, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED));

    return new;
}

/* Attempt to find an empty node to store value, otherwise append a new node.
 * Returns the node containing the newly added value.
 */
hp_t *list_insert_or_append(hp_t **head, uintptr_t ptr)
{
    hp_t *node;
    bool need_alloc = true;

    LIST_ITER (head, node) {
        uintptr_t expected = atomic_load(&node->ptr, __ATOMIC_ACQUIRE);
        if (expected == 0 && atomic_cas(&node->ptr, &expected, &ptr,
                                        __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            need_alloc = false;
            break;
        }
    }

    if (need_alloc)
        node = list_append(head, ptr);

    return node;
}

/* Remove a node from the list with the specified value */
bool list_remove(hp_t **head, uintptr_t ptr)
{
    hp_t *node;
    const uintptr_t nullptr = 0;

    LIST_ITER (head, node) {
        uintptr_t expected = atomic_load(&node->ptr, __ATOMIC_ACQUIRE);
        if (expected == ptr && atomic_cas(&node->ptr, &expected, &nullptr,
                                          __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
            return true;
    }

    return false;
}

/* Returns 1 if the list currently contains an node with the specified value */
bool list_contains(hp_t **head, uintptr_t ptr)
{
    hp_t *node;

    LIST_ITER (head, node) {
        if (atomic_load(&node->ptr, __ATOMIC_ACQUIRE) == ptr)
            return true;
    }

    return false;
}

/* Frees all the nodes in a list - NOT THREAD SAFE */
void list_free(hp_t **head)
{
    hp_t *cur = *head;
    while (cur) {
        hp_t *old = cur;
        cur = cur->next;
        free(old);
    }
}

#define DEFER_DEALLOC 1

typedef struct {
    hp_t *pointers;
    hp_t *retired;
    void (*deallocator)(void *);
} domain_t;

/* Create a new domain on the heap */
domain_t *domain_new(void (*deallocator)(void *))
{
    domain_t *dom = calloc(1, sizeof(domain_t));
    if (!dom)
        return NULL;

    dom->deallocator = deallocator;
    return dom;
}

/* Free a previously allocated domain */
void domain_free(domain_t *dom)
{
    if (!dom)
        return;

    if (dom->pointers)
        list_free(&dom->pointers);

    if (dom->retired)
        list_free(&dom->retired);

    free(dom);
}

/*
 * Load a safe pointer to a shared object. This pointer must be passed to
 * `drop` once it is no longer needed. Returns 0 (NULL) on error.
 */
uintptr_t load(domain_t *dom, const uintptr_t *prot_ptr)
{
    const uintptr_t nullptr = 0;

    while (1) {
        uintptr_t val = atomic_load(prot_ptr, __ATOMIC_SEQ_CST);
        hp_t *node = list_insert_or_append(&dom->pointers, val);
        if (!node)
            return 0;

        /* Hazard pointer inserted successfully */
        if (atomic_load(prot_ptr, __ATOMIC_SEQ_CST) == val) {
            TRACE(TRACE_LOAD_SUCCESS);
            return val;
        }

        /*
         * This pointer is being retired by another thread - remove this hazard
         * pointer and try again. We first try to remove the hazard pointer we
         * just used. If someone else used it to drop the same pointer, we walk
         * the list.
         */
        TRACE(TRACE_LOAD_FAIL);
        uintptr_t tmp = val;
        if (!atomic_cas(&node->ptr, &tmp, &nullptr, __ATOMIC_ACQ_REL,
                        __ATOMIC_RELAXED))
            list_remove(&dom->pointers, val);
    }
}

/*
 * Drop a safe pointer to a shared object. This pointer (`safe_val`) must have
 * come from `load` or `swap`
 */
void drop(domain_t *dom, uintptr_t safe_val)
{
    if (!list_remove(&dom->pointers, safe_val))
        __builtin_unreachable();
}

void cleanup_ptr(domain_t *dom, uintptr_t ptr, int flags)
{
    if (!list_contains(&dom->pointers, ptr)) { /* deallocate straight away */
        dom->deallocator((void *) ptr);
    } else if (flags & DEFER_DEALLOC) { /* Defer deallocation for later */
        list_insert_or_append(&dom->retired, ptr);
    } else { /* Spin until all readers are done, then deallocate */
        while (list_contains(&dom->pointers, ptr))
            ;
        dom->deallocator((void *) ptr);
    }
}

/* Swaps the contents of a shared pointer with a new pointer. The old value will
 * be deallocated by calling the `deallocator` function for the domain, provided
 * when `domain_new` was called. If `flags` is 0, this function will wait
 * until no more references to the old object are held in order to deallocate
 * it. If flags is `DEFER_DEALLOC`, the old object will only be deallocated
 * if there are already no references to it; otherwise the cleanup will be done
 * the next time `cleanup` is called.
 */
uintptr_t swap(domain_t *dom, uintptr_t *prot_ptr, uintptr_t new_val)
{
    hp_t *node = list_insert_or_append(&dom->pointers, new_val);
    if (!node)
        return (uintptr_t) 0;
    uintptr_t old_obj = atomic_exchange(prot_ptr, new_val, __ATOMIC_SEQ_CST);
    TRACE(TRACE_SWAP);
    return old_obj;
}

/* Forces the cleanup of old objects that have not been deallocated yet. Just
 * like `swap`, if `flags` is 0, this function will wait until there are no
 * more references to each object. If `flags` is `DEFER_DEALLOC`, only
 * objects that already have no living references will be deallocated.
 */
void cleanup(domain_t *dom, int flags)
{
    hp_t *node;

    LIST_ITER (&dom->retired, node) {
        uintptr_t ptr = node->ptr;
        if (!ptr)
            continue;

        if (!list_contains(&dom->pointers, ptr)) {
            /* We can deallocate straight away */
            if (list_remove(&dom->pointers, ptr))
                dom->deallocator((void *) ptr);
        } else if (!(flags & DEFER_DEALLOC)) {
            /* Spin until all readers are done, then deallocate */
            while (list_contains(&dom->pointers, ptr))
                ;
            if (list_remove(&dom->pointers, ptr))
                dom->deallocator((void *) ptr);
        }
    }
}
