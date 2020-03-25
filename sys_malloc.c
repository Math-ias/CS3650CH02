#include <stdlib.h>
#include <stdio.h>

#include "xmalloc.h"

void*
xmalloc(size_t bytes)
{
    printf("allocated %ld\n", bytes);
    return malloc(bytes);
}

void
xfree(void* ptr)
{
    free(ptr);
}

void*
xrealloc(void* prev, size_t bytes)
{
    return realloc(prev, bytes);
}