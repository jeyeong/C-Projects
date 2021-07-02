/*** includes ***/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/*** defines, structs ***/

#define MIN_ALLOC_SIZE 4096  // page-sized chunk

typedef struct header {
    unsigned int size;
    struct header *next;
} header_t;

#define UNTAG(p) ((header_t *) ((long) p & 0xfffffffffffffffc))

// unsigned long stack_bottom;

/*** header operations ***/

static header_t base = {0, &base};  // zero sized block to start with
static header_t *freep = &base;  // pointer to the first free block of memory
static header_t *usedp = NULL;  // pointer to the first used block of memory

/*
 * Scans the free list and look for a place to put the block.
 */
static void add_to_free_list(header_t *bp)
{
    header_t *p;

    // Set p to the free block right before the new block
    for (p = freep; !(bp > p && bp < p->next); p = p->next)
        if (p >= p->next && (bp > p || bp < p->next))
            break;

    // Forward coalescence
    if (bp + bp->size == p->next) {
        bp->size += p->next->size;
        bp->next = p->next->next;
    } else
        bp->next = p->next;

    // Backward coalescence
    if (p + p->size == bp) {
        p->size += bp->size;
        p->next = bp->next;
    } else
        p->next = bp;

    // By setting freep to the free block right before the new block, we avoid
    // having to traverse the entire free list again.
    freep = p;
}

/*
 * Request more memory from the kernel.
 */
static header_t *more_core(size_t num_bytes)
{
    void *vp;
    header_t *up;

    // Determine size to allocate
    if (num_bytes < MIN_ALLOC_SIZE) num_bytes = MIN_ALLOC_SIZE;

    // Create space
    if ((vp = sbrk(num_bytes)) == (void *) -1)
        return NULL;

    // Create the header, add the new block to the free list
    up = (header_t *) vp;
    up->size = num_bytes / sizeof(header_t);
    add_to_free_list(up);
    return freep;
}

/*** malloc ***/

/*
 * Find a chunk from the free list and put it in the used list.
 */
void *gc_malloc(size_t alloc_size)
{
    size_t num_units;
    header_t *p, *prevp;

    // Get malloc size in terms of 16 byte-chunks
    // Note: (alloc_size + sizeof(header_t) - 1) ensures we obtain the right
    //       unit size
    num_units = (alloc_size + sizeof(header_t) - 1) / sizeof(header_t) + 1;
    prevp = freep;

    // Cycle through the list of free blocks, finding space to allocate
    for (p = prevp->next;; prevp = p, p = p->next) {
        if (p->size >= num_units) {  // big enough
            if (p->size == num_units)  // exact size
                prevp->next = p->next;
            else {
                p->size -= num_units;
                p += p->size;
                p->size = num_units;
            }

            freep = prevp;  // next fit strategy

            // Add p to used list
            if (usedp == NULL) usedp = p->next = p;
            else {
                p->next = usedp->next;
                usedp->next = p;
            }

            return (void *) (p + 1);
        }

        if (p == freep) {  // not enough memory
            p = more_core(num_units * sizeof(header_t));
            if (p == NULL)  // request for more memory failed
                return NULL;
        }
    }
}

/*** mark and sweep ***/

/*
 * Scan a region of memory and mark any items in the used list if there exists
 * a pointer in the region that points to the item.
 *
 * Note: Both arguments must be word-aligned.
 */
static void scan_region(long *sp, long *end)
{
    header_t *curr_used;

    // Scan through the region 8 bytes (size of a pointer) at a time
    for (; sp < end; sp++) {
        long ptr = *sp;
        curr_used = usedp;

        // Cycle through the used list. If the pointer (note: the value may not
        // be a pointer, but we check anyway) points to an address that is
        // within the used list, then the allocated space is still being used.
        // So we mark the header.
        do {
            if ((long) (curr_used + 1) <= ptr &&
                (long) (curr_used + curr_used->size) > ptr) {
                    // Mark header
                    curr_used->next = (header_t *) ((long) curr_used->next | 1);
                    break;
                }
        } while ((curr_used = UNTAG(curr_used->next)) != usedp);
    }
}

/*
 * Scan the marked blocks for references to other unmarked blocks.
 */
static void scan_heap(void)
{
    long *mem_block;
    header_t *curr_used, *up;

    // Cycle through the used list (i.e. the heap). For each member of the used
    // list, if its header is marked, then we scan every 8-byte block of memory
    // within the allocated space. If there is a pointer that points to some
    // other member of the used list, then that space is still being used. So
    // we mark the header.
    curr_used = usedp;
    do {
        // Unmarked
        if (!((long) curr_used->next & 1))
            continue;

        // Marked
        for (mem_block = (long *) (curr_used + 1);
             mem_block < (long *) (curr_used + curr_used->size);
             mem_block++) {
            long ptr = *mem_block;
            up = UNTAG(curr_used->next);
            do {
                if (up != curr_used &&
                    (long) (up + 1) <= ptr &&
                    (long) (up + up->size) > ptr) {
                    up->next = (header_t *) ((long) up->next | 1);
                    break;
                }
            } while ((up = UNTAG(up->next)) != curr_used);
        }
    } while ((curr_used = UNTAG(curr_used->next)) != usedp);
}

/*
 * Marks blocks of memory in use and frees the ones not in use.
 */
void gc_collect(void)
{
    header_t *p, *prevp, *tp;
    // unsigned long stack_top;
    extern char end, etext;  // provided by the linker

    if (usedp == NULL) return;

    // Scan the BSS and initialized data segments
    scan_region((long *) &etext, (long *) &end);

    // Scan the stack

    // Scan the heap
    scan_heap();

    // Collection
    for (prevp = usedp, p = UNTAG(usedp->next);; prevp = p, p = UNTAG(p->next)) {
    next_chunk:
        if (!((long) p->next & 1)) {
            // The chunk hasn't been marked. Thus, it must be set free. 
            tp = p;
            p = UNTAG(p->next);
            add_to_free_list(tp);

            if (usedp == tp) { 
                usedp = NULL;
                break;
            }

            prevp->next = (header_t *) ((long) p | ((long) prevp->next & 1));
            goto next_chunk;
        }
        p->next = (header_t *) ((long) p->next & ~1);
        if (p == usedp)
            break;
    }
}

/*** main ***/

/*
 * Initialization.
 */
static void initialize()
{
    more_core(MIN_ALLOC_SIZE);
}

int main()
{
    initialize();

    printf("freep: %p\n", freep);
    printf("freep->next: %p\n", freep->next);
    printf("freep->next->next: %p\n", freep->next->next);

    int *p1, *p2, *p3;

    printf("Malloc 1: %p\n", p1 = gc_malloc(16));
    printf("Malloc 2: %p\n", p2 = gc_malloc(4080));
    printf("Malloc 3: %p\n", p3 = gc_malloc(32));

    gc_collect();

    printf("freep: %p\n", freep);
    printf("freep->next: %p\n", freep->next);
    printf("freep->next->next: %p\n", freep->next->next);
    printf("freep->next->next->next: %p\n", freep->next->next->next);

    return 0;
}
