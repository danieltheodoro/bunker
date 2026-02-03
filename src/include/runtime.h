#include <math.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
typedef FILE __BunkerFile;
typedef struct {
  long long value;
  char *errorMessage;
  bool isError;
} BunkerResult;
// Exception Handling
typedef struct {
  char *message;
} BunkerException;
static jmp_buf *current_env = NULL;
static BunkerException global_exception = {0};
// Function pointer type for lambdas
typedef void *(*BunkerFunction)(void *, ...);
// Closure wrapper: holds function + environment
typedef struct {
  BunkerFunction fn;
  void *env;
} BunkerClosure;
// Array struct
typedef struct {
  void **data;
  size_t length;
  size_t capacity;
} BunkerArray;
// GC Runtime
typedef struct GC_Header {
  struct GC_Header *next;
  size_t size;
  bool marked;
} GC_Header;

static GC_Header *gc_allocations = NULL;
static void *gc_min_ptr = (void *)-1;
static void *gc_max_ptr = NULL;
void *gc_stack_bottom = NULL;

void gc_mark(void *ptr);
void gc_scan_region(void *start, void *end);

void gc_free(void *ptr) {
  if (!ptr)
    return;
  GC_Header *h = (GC_Header *)ptr - 1;
  GC_Header **prev = &gc_allocations;
  GC_Header *curr = gc_allocations;
  while (curr) {
    if (curr == h) {
      *prev = curr->next;
      if (getenv("DEBUG_GC"))
        printf("DEBUG GC: Explicit Free %p\n", ptr);
      free(curr);
      return;
    }
    prev = &curr->next;
    curr = curr->next;
  }
}

void gc_scan_region(void *start, void *end) {
  if (start > end) {
    void *tmp = start;
    start = end;
    end = tmp;
  }
  char *p = (char *)((uintptr_t)start & ~(uintptr_t)(sizeof(void *) - 1));
  char *p_end = (char *)end;
  while (p + sizeof(void *) <= p_end) {
    gc_mark(*(void **)p);
    p += sizeof(void *);
  }
}

void gc_mark(void *ptr) {
  if (!ptr || ptr < gc_min_ptr || ptr > gc_max_ptr)
    return;
  // Check if ptr is within managed heap
  GC_Header *h = gc_allocations;
  while (h) {
    void *obj_ptr = (void *)(h + 1);
    if (ptr >= obj_ptr && ptr < (void *)((char *)obj_ptr + h->size)) {
      if (!h->marked) {
        h->marked = true;
        if (getenv("DEBUG_GC"))
          printf("DEBUG GC: Marking %p (size %zu)\n", obj_ptr, h->size);
        gc_scan_region(obj_ptr, (void *)((char *)obj_ptr + h->size));
      }
      return;
    }
    h = h->next;
  }
}

void gc_collect() {
  jmp_buf env;
  setjmp(env);
  __builtin_unwind_init();
  volatile void *stack_top = &stack_top;

  // Mark
  gc_scan_region((void *)stack_top, gc_stack_bottom);
  gc_scan_region((void *)env, (void *)((char *)env + sizeof(jmp_buf)));

  // Sweep
  int collected = 0;
  int kept = 0;
  void *new_min = (void *)-1;
  void *new_max = NULL;
  GC_Header **node = &gc_allocations;
  while (*node) {
    GC_Header *curr = *node;
    if (!curr->marked) {
      *node = curr->next;
      // if (getenv("DEBUG_GC")) printf("DEBUG GC: Freeing %p (size %zu)\n",
      // (void*)(curr+1), curr->size);
      free(curr);
      collected++;
    } else {
      curr->marked = false;
      void *obj_ptr = (void *)(curr + 1);
      if (obj_ptr < new_min)
        new_min = obj_ptr;
      if ((char *)obj_ptr + curr->size > (char *)new_max)
        new_max = (char *)obj_ptr + curr->size;
      node = &(*node)->next;
      kept++;
    }
  }
  gc_min_ptr = new_min;
  gc_max_ptr = new_max;
  if (getenv("DEBUG_GC"))
    printf("DEBUG GC: Collection done. Collected: %d, Kept: %d\n", collected,
           kept);
}

void *gc_alloc(size_t size) {
  static int counter = 0;
  if (++counter > 100000) {
    gc_collect();
    counter = 0;
  }
  // Round size up to void* boundary for safe scanning
  size = (size + sizeof(void *) - 1) & ~(sizeof(void *) - 1);
  // Allocate Header + Size
  size_t total_size = sizeof(GC_Header) + size;
  GC_Header *h = (GC_Header *)calloc(1, total_size);
  h->next = gc_allocations;
  h->size = size;
  h->marked = false;
  gc_allocations = h;
  void *obj_ptr = (void *)(h + 1);
  if (obj_ptr < gc_min_ptr)
    gc_min_ptr = obj_ptr;
  if ((char *)obj_ptr + size > (char *)gc_max_ptr)
    gc_max_ptr = (char *)obj_ptr + size;
  return obj_ptr;
}

void *gc_get_stack_base() { return gc_stack_bottom; }

#define GC_INIT()                                                              \
  {                                                                            \
    volatile void *dummy;                                                      \
    gc_stack_bottom = (void *)&dummy;                                          \
  }
