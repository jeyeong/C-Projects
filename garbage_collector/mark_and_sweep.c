/*** includes ***/

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/*** structs ***/

typedef struct header {
    unsigned int size;
    struct header *next;
} header_t;

/*** header operations ***/

static header_t base = {0, &base};  // Zero sized block to start with
static header_t *freep = &base;  // Pointer to the first free block of memory
static header_t *usedp;  // Pointer to the first used block of memory

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

/* page sized chunks */
#define MIN_ALLOC_SIZE 4096

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

/*** main - test ***/

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

    printf("Malloc 1: %p\n", gc_malloc(16));
    printf("Malloc 2: %p\n", gc_malloc(4080));
    printf("Malloc 3: %p\n", gc_malloc(32));

    return 0;
}
