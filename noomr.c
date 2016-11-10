#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <assert.h>
#include <error.h>
#include "noomr.h"
#include "memmap.h"
#include "stack.h"
#include "noomr_utils.h"


#define max(a, b) ((a) > (b) ? (a) : (b))

shared_data_t * shared;
size_t my_growth;

static stack_t delayed_frees[NUM_CLASSES];

bool out_of_range(void * payload) {
  return payload < (void*) shared->base || payload >= sbrk(0);
}

static header_t * lookup_header(void * user_payload) {
  return out_of_range(user_payload) ? NULL : getblock(user_payload)->header;
}

static inline stack_t * get_stack_index(size_t index) {
  assert(index >= 0 && index < NUM_CLASSES);
  return speculating() ? &shared->spec_free[index] : &shared->seq_free[index];
}

void synch_lists() {
  size_t i;
  volatile header_page_t * page;
  // Set the heads of the stacks to the corresponding elements
  for (i = 0; i < NUM_CLASSES; i++) {
    if(shared->seq_free[i].head != NULL) {
      shared->spec_free[i].head = (node_t *) &seq_node_to_header(shared->seq_free[i].head)->spec_next;
    } else {
      shared->spec_free[i].head = NULL;
    }
  }

  // Set the stack elements
  for (page = shared->header_pg; page != NULL; page = (header_page_t *) page->next) {
    for (i = 0; i < MIN(HEADERS_PER_PAGE, page->next_free); i++) {
      if (!seq_alloced(&page->headers[i]) && page->headers[i].seq_next.next != NULL) {
        header_t * next_header = seq_node_to_header((node_t*) page->headers[i].seq_next.next);
        page->headers[i].spec_next.next = (struct node_t *) &next_header->spec_next;
      } else {
        page->headers[i].spec_next.next = NULL;
      }
    }
  }
}

// This step can (may?) be eliminated by sticking the two items in an array and swaping the index
// mapping to spec / seq free lists
// pre-spec is still needed OR 3x wide CAS operations -- 2 for the pointers
// (which need to be adjacent to each other) and another for the counter
void promote_list() {
  size_t i;
  volatile header_page_t * page;
  // Set the heads of the stacks to the corresponding elements
  for (i = 0; i < NUM_CLASSES; i++) {
    if(shared->spec_free[i].head != NULL) {
      shared->seq_free[i].head = (node_t *) &spec_node_to_header(shared->spec_free[i].head)->seq_next;
    } else {
      shared->seq_free[i].head = NULL;
    }
  }

  // Set the stack elements
  for (page = shared->header_pg; page != NULL; page = (header_page_t *) page->next) {
    for (i = 0; i < MIN(HEADERS_PER_PAGE, page->next_free); i++) {
      assert(payload(&page->headers[i]) != NULL);
      if (!spec_alloced(&page->headers[i]) && page->headers[i].seq_next.next != NULL) {
        header_t * next_header = spec_node_to_header((node_t *) page->headers[i].spec_next.next);
        page->headers[i].seq_next.next = (struct node_t *) &next_header->seq_next;
      } else {
        page->headers[i].seq_next.next = NULL;
      }
    }
  }
}

static header_page_t * payload_to_header_page(void * payload) {
  if (out_of_range(payload)) {
    return NULL;
  } else {
    block_t * block = getblock(payload);
    return  (header_page_t *) (((intptr_t) block) & ~(PAGE_SIZE - 1));
  }
}

size_t noomr_usable_space(void * payload) {
  if (out_of_range(payload)) {
    return getblock(payload)->huge_block_sz - sizeof(block_t);
  } else {
    return getblock(payload)->header->size - sizeof(block_t);
  }
}

/**
 NOTE: when in spec the headers pointers will not be
 into all process's private address space
 To solve this, promise all spec-growth regions
*/
static void map_headers(char * begin, size_t index, size_t num_blocks) {
  size_t i, header_index = -1;
  volatile header_page_t * page;
  size_t block_size = CLASS_TO_SIZE(index);
  stack_t * spec_stack = &shared->spec_free[index];
  stack_t * seq_stack = &shared->seq_free[index];
  block_t * block;
  assert(block_size == ALIGN(block_size));

  for (i = 0; i < num_blocks; i++) {
    while(header_index >= (HEADERS_PER_PAGE - 1) || header_index == -1) {
      for (page = shared->header_pg; page != NULL; page = (volatile header_page_t *) page->next) {
        if (page->next_free < HEADERS_PER_PAGE - 1) {
          header_index = __sync_fetch_and_add(&page->next_free, 1);
          if (header_index < HEADERS_PER_PAGE) {
            goto found;
          }
        }
      }
      if (page == NULL) {
        allocate_header_page();
      }
    }
    found: block = (block_t *) (&begin[i * block_size]);

    page->headers[header_index].size = block_size;
    // mmaped pages are padded with zeros, set NULL anyways
    page->headers[header_index].spec_next.next = NULL;
    page->headers[header_index].seq_next.next = NULL;
    page->headers[header_index].payload = getpayload(block);

    block->header = (header_t * ) &page->headers[header_index];
    // (in)sanity checks
    assert(getblock(getpayload(block)) == block);
    assert(getblock(getpayload(block))->header == &page->headers[header_index]);
    assert(payload(&page->headers[header_index]) == getpayload(block));

    __sync_synchronize(); // mem fence
    push(seq_stack, (node_t * ) &page->headers[header_index].seq_next);
    push(spec_stack, (node_t * ) &page->headers[header_index].spec_next);
    // get the i-block
    header_index = -1;
  }
}

static inline void set_large_perm(int flags) {
  block_t * block;
  for (block = (block_t *) shared->large_block; block != NULL; block = block->next_block) {
    int fd = create_large_pg(block->file_name);
    fsync(fd);
    if (!mmap(block, block->huge_block_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0)) {
      noomr_perror("Unable to reconfigure permissions.");
    }
  }
}

void beginspec() {
  noomr_init();
  my_growth = 0;
  synch_lists();
  set_large_perm(MAP_PRIVATE);
}

void free_delayed(void);

void endspec() {
  promote_list();
  set_large_perm(MAP_SHARED);
  free_delayed();
}

void noomr_init() {
  if (shared == NULL) {
    shared = mmap(NULL, PAGE_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    bzero(shared, PAGE_SIZE);
    shared->number_mmap = 1;
    shared->base = (size_t) sbrk(0); // get the starting address
  }
}


static inline header_t * convert_head_mode_aware(node_t * node) {
  if (speculating()) {
    return spec_node_to_header(node);
  } else {
    return seq_node_to_header(node);
  }
}

void * inc_heap(size_t s) {
#ifdef COLLECT_STATS
  __sync_add_and_fetch(&shared->sbrks, 1);
#endif
  return sbrk(s);
}

size_t stack_for_size(size_t min_size) {
  size_t klass;
  for (klass = 0; CLASS_TO_SIZE(klass) < min_size && klass < NUM_CLASSES; klass++) {
    ;
  }
  if (klass >= NUM_CLASSES) {
    return -1;
  }
  return CLASS_TO_SIZE(klass);
}

size_t class_for_size(size_t size) {
  size_t index;
  for (index = 0; CLASS_TO_SIZE(index) < size; index++) {
    ;
  }
  return index;
}

void * noomr_malloc(size_t size) {
  header_t * header;
  stack_t * stack;
  node_t * stack_node;
  size_t aligned = ALIGN(size + sizeof(block_t));
  if (shared == NULL) {
    noomr_init();
  }
#ifdef COLLECT_STATS
  __sync_add_and_fetch(&shared->allocations, 1);
#endif
  if (size > MAX_SIZE) {
    void * block = allocate_large(aligned);
    record_allocation(block, noomr_usable_space(block));
    return block;
  } else {
    alloc: stack = get_stack_index(SIZE_TO_CLASS(aligned));
    stack_node = pop(stack);
    if (! stack_node) {
      // Need to grow the space
      size_t index = class_for_size(aligned);
      size_t size = CLASS_TO_SIZE(index);
      assert(size > 0);
      size_t blocks = MIN(HEADERS_PER_PAGE, max(1024 / size, 15));

      size_t my_region_size = size * blocks;
      if (speculating()) {
        // first allocate the extra space that's needed. Don't record the to allocation
        inc_heap(__sync_add_and_fetch(&shared->spec_growth, my_region_size) - my_growth);
        __sync_add_and_fetch(&my_growth, size * blocks);
      }
      // Grow the heap by the space needed for my allocations
      char * base = inc_heap(my_region_size);
      if (speculating()) {
      record_allocation(base, my_region_size);
      }
      // now map headers for my new (private) address region
      map_headers(base, index, blocks);
      goto alloc;
    }
    header = convert_head_mode_aware(stack_node);
    assert(payload(header) != NULL);
    assert(getblock(payload(header))->header == header);
#ifdef COLLECT_STATS
      __sync_add_and_fetch(&shared->allocs_per_class[class_for_size(aligned)], 1);
#endif
    record_mode_alloc(header);
    if (speculating()) {
      assert(spec_alloced(header));
    } else {
      assert(seq_alloced(header));
    }
    record_allocation(payload(header), header->size);
    return payload(header);
  }
}

void noomr_free(void * payload) {
#ifdef COLLECT_STATS
  __sync_add_and_fetch(&shared->frees, 1);
#endif
  if (out_of_range(payload)) {
    // A huge block is unmapped directly to kernel
    // This can be done immediately -- there is no re-allocation conflicts
    block_t * block = getblock(payload);
    if (munmap(block, block->huge_block_sz) == -1) {
      noomr_perror("Unable to unmap block");
    }
  } else if (!speculating()) {
    // Not speculating -- free now
    header_t * header = getblock(payload)->header;
    // look up the index & push onto the stack
    stack_t * stack = get_stack_index(SIZE_TO_CLASS(header->size));
    record_mode_free(header);
    push(stack, speculating() ? &header->spec_next : &header->seq_next);
  } else {
    /**
     * We can use the speculative next. If the payload was allocated speculatively,
     * 	then the spec next is not needed
     * If it was originally allocated sequentially (eg. before starting spec)
     * 	then spec_next was unused -- no need to keep around
     */
    unsigned index = SIZE_TO_CLASS(noomr_usable_space(payload));
    push_ageless(&delayed_frees[index], &getblock(payload)->header->spec_next);
  }
}

void free_delayed() {
  size_t index;
  for (index = 0; index < NUM_CLASSES; index++) {
    while (!empty(&delayed_frees[index])) {
      node_t * node = pop_ageless(&delayed_frees[index]);
      header_t * head = container_of(node, header_t, spec_next);
      push(&shared->spec_free[index], &head->spec_next);
    }
  }
}

void * noomr_calloc(size_t nmemb, size_t size) {
  void * payload = noomr_malloc(nmemb * size);
  memset(payload, 0, noomr_usable_space(payload));
  return payload;
}

void print_noomr_stats() {
#ifdef COLLECT_STATS
  int index;
  printf("NOOMR stats\n");
  printf("allocations: %u\n", shared->allocations);
  printf("frees: %u\n", shared->frees);
  printf("sbrks: %u\n", shared->sbrks);
  printf("huge allocations: %u\n", shared->huge_allocations);
  printf("header pages: %u\n", shared->header_pages);
  for (index = 0; index < NUM_CLASSES; index++) {
    printf("class %d allocations: %u\n", index, shared->allocs_per_class[index]);
  }
#else
  printf("NOOMR not configured to collect statistics\n");
#endif
}

void * noomr_realloc(void * p, size_t size) {
  size_t original_size = noomr_usable_space(p);
  if (original_size >= size) {
    return p;
  } else {
    void * new_payload = noomr_malloc(size);
    memcpy(new_payload, p, original_size);
    noomr_free(p);
    return new_payload;
  }
}

#ifdef NOOMR_SYSTEM_ALLOC
#ifdef __GNUC__
#include <malloc.h>

static void * noomr_malloc_hook (size_t size, const void * c){
  return noomr_malloc(size);
}

static void noomr_free_hook (void* payload, const void * c){
  noomr_free(payload);
}

static void * noomr_realloc_hook(void * payload, size_t size, const void * c) {
  return noomr_realloc(payload, size);
}

static void * noomr_calloc_hook(size_t a, size_t b, const void * c) {
  return noomr_calloc(a, b);
}

static void noomr_dehook() {
  __malloc_hook = NULL;
  __free_hook = NULL;
  __realloc_hook = NULL;
#ifdef __calloc_hook
  __calloc_hook = NULL;
#endif
}

void __attribute__((constructor)) noomr_hook() {
  // atexit(noomr_dehook);
  __malloc_hook = noomr_malloc_hook;
  __free_hook = noomr_free_hook;
  __realloc_hook = noomr_realloc_hook;
#ifdef __calloc_hook
  __calloc_hook = noomr_calloc_hook;
#endif
}

#else

void * malloc(size_t t) {
  return noomr_malloc(t);
}
void free(void * p) {
  noomr_free(p);
}
size_t malloc_usable_size(void * p) {
  return noomr_usable_space(p);
}
void * realloc(void * p, size_t size) {
  return noomr_realloc(p, size);
}
void * calloc(size_t a, size_t b) {
  return noomr_calloc(a, b);
}
#endif
#endif
