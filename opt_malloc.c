#include "xmalloc.h"

#include <assert.h>

#include <pthread.h>
#include <string.h>
#include <sys/mman.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

#define ARENAS 4
#define BUCKETS 9

// A block of data is the piece of free-list data at the beginning of an allocation.
typedef struct block {
    int size; // The size of this allocation (block plus data afterwards).
    int arena_index; // The index of the arena in arenas.
    struct block* next; // We only traverse the free list as a singly linked list.
} block;

// Our data structure is defined as an array of free lists. (BUCKETS of them).
// The indices of these free lists match up with our settings arrays.
int page_sizes[BUCKETS] = {4096, 4096, 4096, 4096,
    4096, 4096, 4096, 4096, 8192};
int block_sizes[BUCKETS] = {40, 48, 80, 144, 272, 528, 1040, 2064, 4112};

typedef struct arena {
    // The indices of these free_lists match up with the indices of our global arrays.
    block* heads[BUCKETS];
    // Use the following lock when modifying the data structure.
    pthread_mutex_t lock;
} arena;

// Our data structure is a 

pthread_mutex_t initialize_lock = PTHREAD_MUTEX_INITIALIZER;

static arena arenas[ARENAS];
int initialized_arenas = 0;
__thread int threads_favorite_arena_index = 0;

/**
 * Initialize the arenas if necessary.
 */
void initialize_arenas() {
    if (!initialized_arenas) {
        pthread_mutex_lock(&initialize_lock);
        // We wake up. We might actually be initialized now.
        if (initialized_arenas) {
            pthread_mutex_unlock(&initialize_lock);
            return;
        }
        for (int i = 0; i < ARENAS; i++) {
            pthread_mutex_init(&(arenas[i].lock), NULL);
        }
        initialized_arenas = 1;
        pthread_mutex_unlock(&initialize_lock);
    }
}

const int PAGE_SIZE = 4096;

/**
 * Rounds the given number to be a multiple of page size.
 * @param bytes The size of the allocation.
 * @return      The number of bytes in multiples of page sizes to allocate it.
 */
int round_pages(int bytes) {
    int pages = bytes / PAGE_SIZE;
    if (pages * PAGE_SIZE < bytes) {
        return (pages + 1) * PAGE_SIZE;
    } else {
        return pages * PAGE_SIZE;
    }
    // This is fast? It was in Nat Tuck code.
}

/**
 * Searches the list of buckets for an adequate size, or returns -1.
 * @param bytes The size to allocate.
 * @returns     An index or -1.
 */
int bucket_lookup(size_t bytes) {
    int index = -1;
    for (int i = 0; i < BUCKETS; i++) {
        if (bytes <= block_sizes[i]) {
            return i;
        }
    }
    return index;
} 

block* NON_BUCKET_RESERVED = (block*) 1;

/**
 * Create a new allocation with bytes amount of bytes.
 * @param bytes     The number of bytes to allocate.
 * @return          A pointer to a new data block allocated.
 */
void* xmalloc(size_t bytes) {
    assert(bytes < INT_MAX); // TODO: Remove for optimization/
    
    initialize_arenas();
    // We change bytes to be representative of the block size we will need to store.
    bytes += sizeof(block);
    
    // Bucket lookup!
    int index = bucket_lookup(bytes);
    
    if (index != -1) {
        // This allocation will happen inside one of our free lists.
        
        // We first look for an appropriate arena.
        int arena_index;
        for (
            // We start looking at our favorite.
            arena_index = threads_favorite_arena_index;
            ; // No stop condition, we keep searching.
            arena_index = (arena_index + 1) % ARENAS) {
            if (pthread_mutex_trylock(&(arenas[arena_index].lock)) == 0) {
                break; // We obtained a lock.
            }
        }
        threads_favorite_arena_index = arena_index; // You're ma new favorite!
        
        block* first_block = arenas[arena_index].heads[index];
        if (first_block == 0) {
            // The list was actually empty, we need more free spaces.
            int page_size = page_sizes[index];
            void* ptr = mmap(0, round_pages(page_size),
                             PROT_READ | PROT_WRITE,
                             MAP_ANONYMOUS|MAP_PRIVATE,
                             -1, 0);
            assert(ptr != MAP_FAILED); // TODO: Optimize out.
            int block_size = block_sizes[index];
            int allocations = page_size / block_size; // Floored by int. division.
            for (int i = 0; i < allocations ; i++) {
                block* new_block = (block*) (ptr + i * block_size);
                new_block->size = block_size;
                new_block->next = (block*) (ptr + (i + 1) * block_size);
            } // This does blocks "out of order", but that doesn't matter. It's a stack!
            // Fix last one. It was pointing out of memory.
            ((block*) (ptr + (allocations - 1) * block_size))->next = 0;
            arenas[arena_index].heads[index] = ptr + block_size; // Second block is the head.
            first_block = ptr;  // First block is literally the first block.
        }
        
        // We now have a block to allocate to.
        // We pop the block off the stack.
        arenas[arena_index].heads[index] = first_block->next;
        void* ptr = first_block + 1; // We consciously use block pointer type here.
        pthread_mutex_unlock(&(arenas[arena_index].lock));
        
        if (first_block->size < bytes) {
            printf("%p \n", first_block);
            printf("%d \n", first_block->size);
            printf("%d \n", block_sizes[index]);
            printf("%ld \n", bytes);
            abort();
        }
        
        return ptr;
    } else {
        // This allocation will happen outside a free list.
        void* ptr = mmap(0, round_pages(bytes),
                         PROT_READ | PROT_WRITE,
                         MAP_ANONYMOUS|MAP_PRIVATE,
                         -1, 0);
        assert(ptr != MAP_FAILED); // TODO: Comment out when we are optimizing.
        block* my_block = (block*) ptr;
        my_block->size = round_pages(bytes);
        my_block->next = NON_BUCKET_RESERVED;
        // We use this to flag free that this is not in a free list.
        return my_block + 1; // We consciously use block pointer type.
    }
}

/**
 * Free the given item that was allocated by us.
 * @param ptr   A pointer to the item we allocated.
 */
void xfree(void* ptr) {
    block* my_block = ((block*) ptr) - 1; // We consciously use block pointer type here.
    if (my_block->next != NON_BUCKET_RESERVED) {
        int arena_index = my_block->arena_index;
        // We willingly wait for the lock for this item.
        pthread_mutex_lock(&arenas[arena_index].lock);
        
        int index = bucket_lookup(my_block->size);
        my_block->next = arenas[arena_index].heads[index]; // Set next to current head.
        arenas[arena_index].heads[index] = my_block; // Make my_block new head.
        
        pthread_mutex_unlock(&arenas[arena_index].lock);
    } else {
        // The block is NOT in a free list.
        munmap(my_block, my_block->size);
    }
}

void* xrealloc(void* prev, size_t bytes) {
    assert(bytes < INT_MAX); // TODO: Remove for optimization/
    
    // "If ptr is NULL, then the call is equivalent to malloc(size),
    // for all values of size;"
    if (prev == 0) {
        return xmalloc(bytes);
    } else {
        // "if size is equal to zero, and ptr is not NULL,
        // then the call is equivalent to free(ptr)"
        if (bytes == 0) {
            xfree(prev);
            return NULL;
        }
        
        // The contents will be unchanged in the range from the start of the
        // region up to the minimum of the old and new sizes.
        // Ideally we would use the same arena but we found this change unnecessary.
        
        block* my_block = ((block*) prev) - 1;
        // if (my_block->next != NON_BUCKET_RESERVED) {
            // TODO: In a bucket realloc.
            
            
        // } else {
            // Not in a free list.
            int to_copy;
            int allocated = my_block->size - sizeof(block);
            if (allocated > bytes) {
                to_copy = bytes;
            } else {
                to_copy = allocated;
            }
            void* new_ptr = xmalloc(bytes);
            memcpy(new_ptr, prev, to_copy);
            xfree(prev);
            return new_ptr;
        // }
    }

    return NULL;
}
