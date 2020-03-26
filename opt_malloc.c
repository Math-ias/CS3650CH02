#include "xmalloc.h"

#include <stddef.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/mman.h>
#include <assert.h>

/******************************
 * Program Design Description *
 * ****************************/

// Observations about the programs we need to optimize.
// we need to coalesce well in order to survive the frag test. (xv6 couldn't do it.)
// collatz_list uses one huge block, but mostly blocks exactly the size of 16 bytes.
// collatz_ivec also uses a huge block,
// but most of the blocks are off versions of our well known versions of powers of 2.
// Like 64, 24, 32. Stuff like that.
// The full list of used sizes from the collatz programs are.
// 16, 24, 32, 64, 512, 1024, 2048, 4096
// We can initially tailor ourselves to these sizes and just use a page for everything else.
// But this will break down for the frag test.
// This informed the first choice of an allocator, buckets!

// We maintain a list of supported chunk sizes.
// Powers of 2, and 3/4 of those sizes.

/*********************
 * All About Bitmaps *
 * *******************/

// A collection of code relating to bitmaps that are 256 bits (32 bytes).
// We represent these as an array of 4 unsigned long long's.

typedef struct bitstring { unsigned long long s[4]; } bitstring;

bitstring FILLED_STRING = { {0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFFFFFFFFFF} };
bitstring EMPTY_STRING = { {0, 0, 0, 0} };

void AND(bitstring* a, bitstring* b) {
    a->s[0] &= b->s[0];
    a->s[1] &= b->s[1];
    a->s[2] &= b->s[2];
    a->s[3] &= b->s[3];
}

void OR(bitstring* a, bitstring* b) {
    a->s[0] |= b->s[0];
    a->s[1] |= b->s[1];
    a->s[2] |= b->s[2];
    a->s[3] |= b->s[3];
}

int EQUAL(bitstring* a, bitstring* b) {
    return a->s[0] == b->s[0] &&
        a->s[1] == b->s[1] &&
        a->s[2] == b->s[2] &&
        a->s[3] == b->s[3];
}

void NOT(bitstring* a) {
    a->s[0] = ~a->s[0];
    a->s[1] = ~a->s[1];
    a->s[2] = ~a->s[2];
    a->s[3] = ~a->s[3];
}

// Uses the ffs builtin to return one plus the index of the least significant 0 bit of x.
// Normally, ffs means to return the first 1, but we are using 1 to signify "occupiedness".
// If x is full return zero.
// I believe the reason its 1 plus is to differentiate between 0 being at 0, and 1 being at 0.
int NFFS(bitstring* a) {
    int pos0 = __builtin_ffsll(~(a->s[0]));
    int pos1 = __builtin_ffsll(~(a->s[1]));
    int pos2 = __builtin_ffsll(~(a->s[2]));
    int pos3 = __builtin_ffsll(~(a->s[3]));
    if (pos3 != 0) {
        return pos3;
    } else if (pos2 != 0) {
        return pos2 + 64;
    } else if (pos1 != 0) {
        return pos1 + 128;
    } else if (pos0 != 0) {
        return pos0 + 192;
    } else {
        return 0;
    }
}

// Flip a bit located at the given index.
void FLIP(bitstring* a, int index) {
    assert(index >= 0 && index < 256);
    if (index <= 63) {
        a->s[3] ^= 1ULL << index;
    } else if (index <= 127) {
        a->s[2] ^= 1ULL << (index - 64);
    } else if (index <= 191) {
        a->s[1] ^= 1ULL << (index - 128);
    } else {
        a->s[0] ^= 1ULL << (index - 192);
    }
}

/********************************
 * All About Chunks and Buckets *
 * *****************************/

// We will put the chunk_head at the start of every piece of memory we allocate.
typedef struct chunk_head {
    size_t chunk_size;          // The size of this chunk (need to munmap easily).
    long bucket_index;          // The bucket "id" of the blocks we allocate in this chunk.
    // We need this extra metadate to figure out if we are bucketed or not.
    // Set to -1 if you are not using it.
} chunk_head;

// A bucket head is like a chunk head but has extra metadata at the end specific to bucketed items.
typedef struct bucket_head {
    chunk_head chunk;
    struct bucket_head* next;        // The next chunk in our list of chunks.
    struct bucket_head* prev;
    bitstring allocations;   // The "used" bitstring for this bucket. 1 means used.
} bucket_head;

const unsigned long long FULL_ULL = 0xFFFFFFFFFFFFFFFF;

// The buckets we will use are hardcoded.
// I chose pages manually, and calculated the allocated bitstring in excel.
// Note that each block we allocate has a pointer to the start of the chunk preceding it.
// That's 8 extra bytes for each allocation. That's why these bucket sizes seem 8 off.

const bitstring BUCKET24 = { { FULL_ULL, 0xFFFFFF0000000000, 0ULL, 0ULL} }; // We want to allow 168 allocs.
const bitstring BUCKET32 = { { FULL_ULL, FULL_ULL, 0xC000000000000000, 0ULL } }; // 126
const bitstring BUCKET40 = { { FULL_ULL, FULL_ULL, 0xFFFFFFF000000000, 0ULL } }; // 100
const bitstring BUCKET72 = { { FULL_ULL, FULL_ULL, FULL_ULL, 0xFF00000000000000 } }; // 56
const bitstring BUCKET520 = { { FULL_ULL, FULL_ULL, FULL_ULL, 0xC000000000000000} }; // 62
const bitstring BUCKET1032 = { { FULL_ULL, FULL_ULL, FULL_ULL, 0x8000000000000000 } }; // 63
const bitstring BUCKET2056 = { { FULL_ULL, FULL_ULL, FULL_ULL, 0xFFFFFFFF80000000 } }; // 31
const bitstring BUCKET4104 = { { FULL_ULL, FULL_ULL, FULL_ULL, 0xFFFFFFFFFFFF8000 } }; // 15

bitstring SUPPORTED_BUCKETS_BITSTRINGS[] = {
    BUCKET24, BUCKET32, BUCKET40, BUCKET72,
    BUCKET520,
    BUCKET1032, BUCKET2056, BUCKET4104
};

long SUPPORTED_BUCKETS_SIZES[] = {
    24, 32, 40, 72,
    520,
    1032, 2056, 4104
};

long SUPPORTED_BUCKETS_PAGES[] = {
    1, 1, 1, 1,
    8,
    16, 16, 16
};

// We will put this block at the head of every allocation to lead us back to the chunk.
// Which also may be a bucket ...
typedef struct block_head {
    struct chunk_head* parent_chunk;
} block_head;

/****************
 * RUNTIME CODE *
 ***************/

const size_t PAGE_SIZE = 4096;

// The indices for the starts of these lists match up with the indices in SUPPORTED_BUCKETS.
// This will probably become thread local or arena-specific when we become thread safe.
bucket_head head_bucket_list[8];
// These are uninitialized, we will need make the head nodes when we initialize.

// Global initialized flag.
int initialized = 0;

/**
 * void initialize()
 * Initializes the heads of these doubly linked bucket lists if needed.
 */
void initialize() {
    if (!initialized) {
        for (long i = 0; i < 8; i++) {
            head_bucket_list[i].chunk.chunk_size = 0;
            head_bucket_list[i].chunk.bucket_index = 1;
            head_bucket_list[i].next = &(head_bucket_list[i]);
            head_bucket_list[i].prev = &(head_bucket_list[i]);
            head_bucket_list[i].allocations = FILLED_STRING; 
        }
        initialized = 1;
    }
}

/**
 * long get_bucket(size_t bytes)
 * Gives the chosen bucket size for this type of allocation.
 * @param bytes The size of the allocation to make.
 * @return      The bucket index in SUPPORTED_BUCKETS
 *              or -1 if the allocation doesn't match.
 */
long get_bucket(size_t bytes) {
    for (int i = 0; i < 8; i++) {
        size_t bucket_size = SUPPORTED_BUCKETS_SIZES[i];
        if (bytes < bucket_size) {
            return i;
        }
    }
    return -1;
}

/**
 * long pages_needed(size_t bytes)
 * Returns the number of pages needed to store an item of this given size.
 * @param bytes The number of bytes needed to allocate (including block size).
 * @returns     The number of pages needed (basically a ciel).
 */
long pages_needed(size_t bytes) {
    long pages = bytes / PAGE_SIZE;     // Hardcoded page size.
    if (pages * PAGE_SIZE < bytes) {
        return pages + 1;
    } else {
        return pages;
    }
}

/**
 * void check_map(void* map)
 * A call to check the return of mmap to know if there are any errors allocating.
 * @param map   The null pointer returned by mmap.
 */
void check_map(void* map) {
    if (map == MAP_FAILED) {
        perror("There were issues when attempting to map in new memory.");
        printf("We encountered an issue whilst allocating and mmaping, this was the errno %d.",     
            errno);
        fflush(stdout);
        fflush(stderr);
        abort();
    }
}

/**
 * void xbig_malloc(size_t size)
 * A helper function for when our allocation doesn't fit in a bucket.
 * @param bytes The size of the allocation needed (block_head size included).
 * @returns     The pointer to the new memory allocated.
 */
void* xbig_malloc(size_t size) {
    size_t big_alloc_size = size + sizeof(chunk_head);
    long pages = pages_needed(big_alloc_size);
    void* map = mmap(0, pages * PAGE_SIZE,
                        PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
    check_map(map);
    chunk_head* page_head = (chunk_head*) map;
    page_head->chunk_size = big_alloc_size; // We say how big this chunk is, needed to unmap.
    page_head->bucket_index = -1; // We specify we aren't a bucket.
    void* location = (void*) (page_head + 1);
    block_head* item_head = (block_head*) location;
    item_head->parent_chunk = page_head;
    return ((void*) (item_head + 1));
}

/**
 * bucket_head* allocate_bucket(long b_index)
 * Allocate a bucket into our bucket list given the predefined bucket index.
 * @param b_index   The bucket index we will use to allocate.
 * @returns         A pointer to the bucket_head of the newly allocated bucket.
 */
bucket_head* allocate_bucket(long b_index) {
    size_t chunk_size = SUPPORTED_BUCKETS_PAGES[b_index] * PAGE_SIZE;
    void* map = mmap(0, chunk_size,
                        PROT_READ | PROT_WRITE,
                        MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
    check_map(map);
    chunk_head* new_chunk_head = (chunk_head*) map;
    new_chunk_head->chunk_size = chunk_size;
    new_chunk_head->bucket_index = b_index;
    bucket_head* new_bucket_head = (bucket_head*) map;
    new_bucket_head->next = head_bucket_list[b_index].next;
    new_bucket_head->next->prev = new_bucket_head; // Fix the nodes in front.
    new_bucket_head->prev = &(head_bucket_list[b_index]);
    head_bucket_list[b_index].next = new_bucket_head; // Fix the nodes behind.
    new_bucket_head->allocations = SUPPORTED_BUCKETS_BITSTRINGS[b_index];

    return new_bucket_head;
}

/**
 * bucket_head* find_free_bucket_space(long b_index)
 * Find a bucket with space with the given bucket id.
 * @param b_index   The index of the type of bucket we are looking for in SUPPORTED_BUCKETS.
 * @returns         A pointer to either an existing bucket or a newly allocated bucket.
 */
bucket_head* find_free_bucket_space(long b_index) {
    bucket_head* current_bucket = head_bucket_list[b_index].next;
    while (current_bucket != &(head_bucket_list[b_index])) {
        // We seek through all the buckets looking for free space.
        if (EQUAL(&(current_bucket->allocations), &FILLED_STRING)) {
            current_bucket = current_bucket->next; // This bucket is filled, look in the next one.
        } else {
            break; // There was a zero in the string signifying open space!
        }
    }
    
    if (current_bucket == &(head_bucket_list[b_index])) {
        // If we didn't actually find a free bucket and the loop ended before we found one.
        current_bucket = allocate_bucket(b_index);
        // Then we allocate a new one.
    }
    return current_bucket;
}

/**
 * void* xmalloc(size_t bytes)
 * Create a new allocation with bytes amount of bytes.
 * @param bytes     The number of bytes to allocate.
 * @return          A pointer to a new data block allocated.
 */
void* xmalloc(size_t bytes) {
    initialize();
    
    // We always allocate with the block_head. Sorry bub.
    size_t size = bytes + sizeof(block_head);
    long b_index = get_bucket(size);
    
    if (b_index == -1) {
        // There is no defined bucket for this size.
        return xbig_malloc(size);
    } else {
        // There is a defined bucket for this size.
        // If there is no chunk ready then we allocate it.
        bucket_head* free_bucket = find_free_bucket_space(b_index);
        int free_index = NFFS(&(free_bucket->allocations)) - 1; // FFS returns plus one, we sub that.
        FLIP(&(free_bucket->allocations), free_index); // We flip that one to a zero.
        void* location = ((void*) (free_bucket + 1)) + free_index * SUPPORTED_BUCKETS_SIZES[b_index];
        block_head* item_head = (block_head*) location;
        item_head->parent_chunk = (chunk_head*) free_bucket;
        return (void*) (item_head + 1);
    }
}

/**
 * void xbig_free(block_head* item_head)
 * Free the given item from it's item head that we've determined to not be in a bucket.
 * @param item_head The head of the item's block including the pointer we will follow to the chunk.
 */
void xbig_free(block_head* item_head) {
    chunk_head* item_chunk_head = item_head->parent_chunk;
    munmap(item_chunk_head, item_chunk_head->chunk_size);
}

/**
 * void xfree(void* ptr)
 * Free the given item that was allocated by us.
 * @param ptr   A pointer to the item we allocated.
 */
void xfree(void* ptr) {
    // We seek to the item head to follow it back to the chunk start.
    block_head* item_head = ((block_head*) ptr) - 1;
    chunk_head* item_chunk_head = item_head->parent_chunk;
    if (item_chunk_head->bucket_index == -1) {
        xbig_free(item_head);
    } else {
        // This item is part of a bucket.
        // Let's mark it as free.
        bucket_head* item_bucket_head = (bucket_head*) item_chunk_head;
        size_t b_index = item_chunk_head->bucket_index;
        int item_index = (((void*) item_head) - ((void*) (item_bucket_head + 1))) / 
            SUPPORTED_BUCKETS_SIZES[b_index];
        FLIP(&(item_bucket_head->allocations), item_index);
        
        // Then we check if the bucket is empty.
        if (EQUAL(&(item_bucket_head->allocations),
            &(SUPPORTED_BUCKETS_BITSTRINGS[b_index]))) {
            // It is empty, we remove it from the list.
            item_bucket_head->prev->next = item_bucket_head->next;
            item_bucket_head->next->prev = item_bucket_head->prev;
            // Zoop!
        }
    }
}

void* xrealloc(void* prev, size_t bytes) {
    return 0;
}
