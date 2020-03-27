
#include <sys/mman.h>
#include <pthread.h>
#include <string.h>

#include <stdio.h>

#include "xmalloc.h"

// Memory allocator by Kernighan and Ritchie,
// The C programming Language, 2nd ed.  Section 8.7.
//
// Then copied from xv6.
//
// Then modified to use mmap and add a mutex by Nat Tuck, becoming starter code
// for CS3650 Spring 2020.

typedef long Align;

union header {
  struct {
    union header *ptr;
    unsigned int size;
  } s;
  Align x;
};

typedef union header Header;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Header base;
static Header *freep;

static
void
xfree_helper(void *ap)
{
  Header *bp, *p;

  bp = (Header*)ap - 1;
  for(p = freep; !(bp > p && bp < p->s.ptr); p = p->s.ptr)
    if(p >= p->s.ptr && (bp > p || bp < p->s.ptr))
      break;
  if(bp + bp->s.size == p->s.ptr){
    bp->s.size += p->s.ptr->s.size;
    bp->s.ptr = p->s.ptr->s.ptr;
  } else
    bp->s.ptr = p->s.ptr;
  if(p + p->s.size == bp){
    p->s.size += bp->s.size;
    p->s.ptr = bp->s.ptr;
  } else
    p->s.ptr = bp;
  freep = p;
}

void
xfree(void* ap)
{
  pthread_mutex_lock(&lock);
  xfree_helper(ap);
  pthread_mutex_unlock(&lock);
}

static Header*
morecore(size_t nu)
{
  char *p;
  Header *hp;

  if(nu < 4096)
    nu = 4096;
  p = mmap(0, nu * sizeof(Header), PROT_READ|PROT_WRITE,
           MAP_ANONYMOUS|MAP_PRIVATE, -1, 0);
  if(p == (char*)-1)
    return 0;
  hp = (Header*)p;
  hp->s.size = nu;
  xfree_helper((void*)(hp + 1));
  return freep;
}

void*
xmalloc(size_t nbytes)
{
  Header *p, *prevp;
  unsigned int nunits;

  pthread_mutex_lock(&lock);
  nunits = (nbytes + sizeof(Header) - 1)/sizeof(Header) + 1;
  if((prevp = freep) == 0){
    base.s.ptr = freep = prevp = &base;
    base.s.size = 0;
  }
  for(p = prevp->s.ptr; ; prevp = p, p = p->s.ptr){
    if(p->s.size >= nunits){
      if(p->s.size == nunits)
        prevp->s.ptr = p->s.ptr;
      else {
        p->s.size -= nunits;
        p += p->s.size;
        p->s.size = nunits;
      }
      freep = prevp;
      pthread_mutex_unlock(&lock);
      return (void*)(p + 1);
    }
    if(p == freep) {
      if((p = morecore(nunits)) == 0) {
        pthread_mutex_unlock(&lock);
        return 0;
      }
    }
  }
}

void*
xrealloc(void* prev, size_t nn)
{
    if (prev == NULL) {
        // "If ptr is NULL, then the call is equivalent to malloc(size), for all values of size;"
        return xmalloc(nn);
    } else {
        // "if size is equal to zero, and ptr is not NULL, then the call is equivalent to free(ptr)"
        if (nn == 0) {
            xfree(prev);
            return NULL;
        }
        
        // The contents will be unchanged in the range from the start of the
        // region up to the minimum of the old and new sizes.
        Header* prev_head = ((Header*) prev) - 1;
        unsigned int block_size = prev_head->s.size * sizeof(Header);
        void* new_data = xmalloc(nn);
        if (block_size < nn) {
            memcpy(new_data, prev, block_size - sizeof(Header));
        } else {
            memcpy(new_data, prev, nn);
        }
        xfree(prev);
        return new_data;
    }
  
}
