
#include "malloc_2d.h"

static malloc_2d_t _malloc_2d;
static malloc_2d_t *malloc_2d = NULL;

//
//* malloc_2d_debug
//

static int malloc_2d_debug_alloc_offset = 0;
void *malloc_2d_debug_arena = NULL;
// Debug allocation using bump pointer; Never free
void *malloc_2d_debug_alloc(size_t sz) {
  if(sz == 0UL) {
    sz = 1UL;
  }
  sz = (sz + 7UL) & ~0x7UL;
  if(sz > 64 * MALLOC_2D_PAGE_SIZE) {
    error_exit("Allocate too large (sz %lu)\n", sz);
  }
  if((malloc_2d_debug_alloc_offset + sz) >= (64 * MALLOC_2D_PAGE_SIZE) || malloc_2d_debug_arena == NULL) {
    malloc_2d_debug_arena = malloc_2d_alloc_os_page_unaligned(64);
    malloc_2d_debug_alloc_offset = 0;
  }
  void *ret = MALLOC_2D_PTR_ADD(malloc_2d_debug_arena, malloc_2d_debug_alloc_offset);
  malloc_2d_debug_alloc_offset += sz;
  return ret;
}

//
//* malloc_2d_arena_t
//

// If obj_size is -1, then we initialize a varlen arena. Otherwise, we initialize object arena.
malloc_2d_arena_t *malloc_2d_arena_obj_init(int obj_size) {
  assert(obj_size <= MALLOC_2D_OBJ_MAX_SIZE);
  void *actual_base;
  malloc_2d_arena_t *arena = (malloc_2d_arena_t *)malloc_2d_alloc_os_page(MALLOC_2D_ARENA_SIZE, &actual_base);
  arena->sc = NULL;
  arena->next = arena->prev = NULL;
  arena->base = actual_base;
  arena->free_count = arena->max_count = \
    (((MALLOC_2D_PAGE_SIZE * MALLOC_2D_ARENA_SIZE) - sizeof(malloc_2d_arena_t)) / obj_size);
  arena->free_list = (uint8_t *)arena + sizeof(malloc_2d_arena_t);
  uint8_t *p = (uint8_t *)arena->free_list;
  for(int i = 0;i < arena->max_count;i++) {
    uint8_t *q = p + obj_size;
    *((void **)p) = (void *)q;
    p = q;
  }
  // Last object points to nothing
  *((void **)(p - obj_size)) = NULL;
  // SEE THIS:
  // Notify the OS of the step size (for Multi-Block Compression)
  //   1. If the object is <= 64 bytes, then the step size is zero
  //   2. If the object is > 64 bytes, then the step size is the number of full cache lines it occupies.
  //      This mechanism works for non-full-cache-block objects.
  // Commented out for submitting to HPCA committee (it only works with the simulator)
  //zsim_magic_ops_notify_step_size((obj_size + 63) / 64);
  malloc_2d->stat->arena_init_count++;
  malloc_2d_arena_set_obj(arena);
  return arena;
}

// Allocate a varlen arena
malloc_2d_arena_t *malloc_2d_arena_varlen_init() {
  void *actual_base;
  malloc_2d_arena_t *arena = (malloc_2d_arena_t *)malloc_2d_alloc_os_page(MALLOC_2D_ARENA_SIZE, &actual_base);
  arena->sc = NULL;
  arena->next = arena->prev = NULL;
  arena->base = actual_base;
  arena->free_size = arena->max_size = MALLOC_2D_VARLEN_MAX_SIZE;
  arena->free_list = MALLOC_2D_PTR_ADD(arena, sizeof(malloc_2d_arena_t));
  malloc_2d_arena_varlen_header_t *header = (malloc_2d_arena_varlen_header_t *)arena->free_list;
  header->prev_size = 0;
  header->size = arena->free_size;
  header->next_free = header->prev_free = NULL;
  malloc_2d->stat->arena_init_count++;
  malloc_2d_arena_set_varlen(arena);
  return arena;
}

// Obj and varlen arena share the same free routine
void malloc_2d_arena_free(malloc_2d_arena_t *arena) {
  //if(arena->free_count != arena->max_count) {
  //  printf("[malloc_2d] Freeing a non-empty arena (max %d free %d addr 0x%lX base 0x%lX)\n",
  //    arena->max_count, arena->free_count, (uint64_t)arena, (uint64_t)arena->base);
  //}
  int type = malloc_2d_arena_get_type(arena);
  switch(type) {
    case MALLOC_2D_ARENA_FLAGS_OBJ:
    case MALLOC_2D_ARENA_FLAGS_VARLEN: {
      malloc_2d_free_os_page(arena->base, MALLOC_2D_ARENA_SIZE);
    } break;
    case MALLOC_2D_ARENA_FLAGS_HUGE: {
      malloc_2d_free_os_page(arena->base, arena->alloc_page_count);
    } break;
    default: {
      error_exit("Unknown arena type: %d\n", type);
    }
  }
  malloc_2d->stat->arena_free_count++;
  return;
}

void malloc_2d_arena_varlen_free_list_remove(
  malloc_2d_arena_t *arena, malloc_2d_arena_varlen_header_t *header) {
  if(header->next_free != NULL) {
    header->next_free->prev_free = header->prev_free;
  }
  if(header->prev_free != NULL) {
    header->prev_free->next_free = header->next_free;
  } else {
    arena->free_list = header->next_free;
  }
  return;
}

void malloc_2d_arena_varlen_free_list_insert_head(
  malloc_2d_arena_t *arena, malloc_2d_arena_varlen_header_t *header) {
  header->prev_free = NULL;
  header->next_free = (malloc_2d_arena_varlen_header_t *)arena->free_list;
  if(arena->free_list != NULL) {
    ((malloc_2d_arena_varlen_header_t *)arena->free_list)->prev_free = header;
  }
  arena->free_list = header;
  return;
}

// Remove the arena from the free list of its associated SC object
void malloc_2d_arena_sc_free_list_remove(malloc_2d_arena_t *arena) {
  if(arena->next != NULL) {
    arena->next->prev = arena->prev;
  }
  if(arena->prev != NULL) {
    arena->prev->next = arena->next;
  } else {
    arena->sc->free_list = arena->next;
  }
  return;
}

void malloc_2d_arena_sc_free_list_insert_head(malloc_2d_arena_t *arena) {
  malloc_2d_sc_t *sc = arena->sc;
  arena->next = sc->free_list;
  arena->prev = NULL;
  if(sc->free_list != NULL) {
    sc->free_list->prev = arena;
  }
  sc->free_list = arena;
  return;
}

// This function returns NULL if the arena is already full
void *malloc_2d_arena_obj_alloc(malloc_2d_arena_t *arena) {
  if(malloc_2d_arena_is_full(arena) == 1) {
    return NULL;
  }
  void *ret = arena->free_list;
  assert(ret != NULL);
  arena->free_list = *(void **)ret;
  assert(arena->free_count > 0);
  arena->free_count--;
  assert(arena->free_list != NULL || arena->free_count == 0);
  return ret;
}

// Return NULL if allocation cannot be made from this arena
void *malloc_2d_arena_varlen_alloc(malloc_2d_arena_t *arena, int size) {
  // This is the actual size to be allocated
  int actual_size = ((size + (MALLOC_2D_VARLEN_ALIGNMENT - 1)) & ~(MALLOC_2D_VARLEN_ALIGNMENT - 1)) + \
          sizeof(malloc_2d_arena_varlen_header_t);
  // Fast path
  if(actual_size > arena->free_size) {
    return NULL;
  }
  // Traverse free list and find the first that match
  void *ret = arena->free_list;
  while(ret != NULL) {
    malloc_2d_arena_varlen_header_t *header = (malloc_2d_arena_varlen_header_t *)ret;
    if(actual_size <= header->size) {
      if((header->size - actual_size) > (int)(MALLOC_2D_OBJ_MAX_SIZE + sizeof(malloc_2d_arena_varlen_header_t))) {
        // Create a new block after the current one
        malloc_2d_arena_varlen_header_t *new_header = \
          (malloc_2d_arena_varlen_header_t *)MALLOC_2D_PTR_ADD(header, actual_size);
        //new_header->next_free = header->next_free;
        //new_header->prev_free = header->prev_free;
        new_header->size = header->size - actual_size;
        new_header->prev_size = actual_size;
        // Update the status of the next block after the new block
        malloc_2d_arena_varlen_header_t *next_header = \
          (malloc_2d_arena_varlen_header_t *)MALLOC_2D_PTR_ADD(ret, header->size);
        void *arena_end = MALLOC_2D_PTR_ADD(arena, MALLOC_2D_PAGE_SIZE * MALLOC_2D_ARENA_SIZE);
        if(MALLOC_2D_PTR_IS_GEQ(next_header, arena_end) == 0) {
          next_header->prev_size = new_header->size;
        }
        // Update the size field of the current header
        header->size = actual_size;
        // Remove header from free list, insert new header into the list
        malloc_2d_arena_varlen_free_list_remove(arena, header);
        malloc_2d_arena_varlen_free_list_insert_head(arena, new_header);
      } else {
        malloc_2d_arena_varlen_free_list_remove(arena, header);
      }
      // Set the current header's status as used
      malloc_2d_arena_varlen_header_set_used(header);
      // Use header's actual allocated size
      arena->free_size -= header->size;
      // Data region
      ret = MALLOC_2D_PTR_ADD(ret, sizeof(malloc_2d_arena_varlen_header_t));
      break;
    }
    ret = header->next_free;
  }
  // ret can be NULL or not NULL at this point
  return ret;
}

void malloc_2d_arena_obj_dealloc(malloc_2d_arena_t *arena, void *ptr) {
  *(void **)ptr = arena->free_list;
  arena->free_list = ptr;
  arena->free_count++;
  assert(arena->free_count <= arena->max_count);
  // If this is the first free object in the arena, then insert into the free list of the sc
  // If arena->sc == NULL then it is debug mode and we simply ignore it
  if(arena->sc != NULL) {
    assert(arena->sc->count > 0);
    arena->sc->count--;
    if(arena->free_count == 1 && arena != arena->sc->curr_arena) {
      malloc_2d_arena_sc_free_list_insert_head(arena);
      malloc_2d->stat->arena_full_to_free_count++;
    } else if(arena->free_count == arena->max_count && arena != arena->sc->curr_arena) {
      // Only free the arena if it is in the free list
      malloc_2d_arena_sc_free_list_remove(arena);
      // Only return it to the OS when sc is not NULL
      malloc_2d_arena_free(arena);
    }
  }
  return;
}

void malloc_2d_arena_varlen_dealloc(malloc_2d_arena_t *arena, void *ptr) {
  void *arena_end = MALLOC_2D_PTR_ADD(arena, MALLOC_2D_PAGE_SIZE * MALLOC_2D_ARENA_SIZE);
  // Data region begin
  void *arena_begin = MALLOC_2D_PTR_ADD(arena, sizeof(malloc_2d_arena_t));
  assert(MALLOC_2D_PTR_IS_GEQ(ptr, arena_begin) == 1 && MALLOC_2D_PTR_IS_GEQ(ptr, arena_end) == 0);
  (void)arena_begin;
  malloc_2d_arena_varlen_header_t *header = \
    (malloc_2d_arena_varlen_header_t *)MALLOC_2D_PTR_SUB(ptr, sizeof(malloc_2d_arena_varlen_header_t));
  if(malloc_2d_arena_varlen_header_is_used(header) == 0) {
    //malloc_2d_arena_varlen_print(arena);
    error_exit("Deallocating a free or invalid object from varlen arena 0x%lX (ptr 0x%lX)\n", 
      (uint64_t)arena, (uint64_t)ptr);
  }
  //fprintf(stderr, "end %p begin %p\n", arena_end, arena_begin);
  //malloc_2d_arena_varlen_print(arena);
  // Update arena free size here, because header will potentially change later
  arena->free_size += header->size;
  malloc_2d_arena_varlen_header_t *next_header = \
    (malloc_2d_arena_varlen_header_t *)MALLOC_2D_PTR_ADD(header, header->size);
  malloc_2d_arena_varlen_header_t *prev_header = \
    (malloc_2d_arena_varlen_header_t *)MALLOC_2D_PTR_SUB(header, header->prev_size);
  int next_is_free = (MALLOC_2D_PTR_IS_GEQ(next_header, arena_end) == 0) && \
                     (malloc_2d_arena_varlen_header_is_used(next_header) == 0);
  int prev_is_free = (header->prev_size != 0) && \
                     (malloc_2d_arena_varlen_header_is_used(prev_header) == 0);
  if(next_is_free == 1 && prev_is_free == 1) {
    malloc_2d_arena_varlen_free_list_remove(arena, next_header);
    malloc_2d_arena_varlen_free_list_remove(arena, prev_header);
    prev_header->size += (header->size + next_header->size);
    next_header = (malloc_2d_arena_varlen_header_t *)MALLOC_2D_PTR_ADD(next_header, next_header->size);
    header = prev_header;
  } else if(next_is_free == 1) {
    malloc_2d_arena_varlen_free_list_remove(arena, next_header);
    header->size += next_header->size;
    next_header = (malloc_2d_arena_varlen_header_t *)MALLOC_2D_PTR_ADD(next_header, next_header->size);
  } else if(prev_is_free == 1) {
    malloc_2d_arena_varlen_free_list_remove(arena, prev_header);
    prev_header->size += header->size;
    header = prev_header;
  }
  //
  // Invariant: header points to the block just inserted into the free list
  // next_header points to the next block of the merged block
  //
  // Insert the header into the head of the linked list
  malloc_2d_arena_varlen_free_list_insert_head(arena, header);
  if(MALLOC_2D_PTR_IS_GEQ(next_header, arena_end) == 0) {
    next_header->prev_size = header->size;
  }
  // Update sc
  if(arena->sc != NULL) {
    assert(arena->sc->count > 0);
    arena->sc->count--;
    // If the arena is completely free, then deallocate it
    if(arena->free_size == arena->max_size && arena != arena->sc->curr_arena) {
      malloc_2d_arena_sc_free_list_remove(arena);
      malloc_2d_arena_free(arena);
    }
  }
  return;
}

void malloc_2d_arena_dealloc(void *ptr) {
  // Round down to nearest arena boundary, low bits are zero
  uint64_t round_mask = ~(MALLOC_2D_PAGE_SIZE * MALLOC_2D_ARENA_SIZE - 1);
  malloc_2d_arena_t *arena = (malloc_2d_arena_t *)((uint64_t)ptr & round_mask);
  int type = malloc_2d_arena_get_type(arena);
  switch(type) {
    case MALLOC_2D_ARENA_FLAGS_OBJ: {
      malloc_2d_arena_obj_dealloc(arena, ptr);
    } break;
    case MALLOC_2D_ARENA_FLAGS_VARLEN: {
      malloc_2d_arena_varlen_dealloc(arena, ptr);
    } break;
    case MALLOC_2D_ARENA_FLAGS_HUGE: {
      malloc_2d_arena_huge_dealloc(arena);
    } break;
    default: {
      error_exit("Unknown type: %d (0x%X) on arena deallocation\n", type, type);
    } break;
  }
  return;
}

// Initializes an arena for huge memory region
// For huge memory region, one arena holds an entire object. The arena header should still be 
// aligned on arena boundaries. The data body starts immediately after the arena header
malloc_2d_arena_t *malloc_2d_arena_huge_init(size_t sz) {
  assert(sz > MALLOC_2D_VARLEN_MAX_SIZE);
  int page_count = (int)((sz + sizeof(malloc_2d_arena_t) + MALLOC_2D_PAGE_SIZE - 1) / MALLOC_2D_PAGE_SIZE);
  int alloc_page_count = page_count + MALLOC_2D_ARENA_SIZE;
  void *ret = malloc_2d_alloc_os_page_unaligned(alloc_page_count);
  void *old_ret = ret;
  uint64_t mask = ~(MALLOC_2D_ARENA_SIZE * MALLOC_2D_PAGE_SIZE - 1);
  if(((uint64_t)ret & mask) != (uint64_t)ret) {
    // Round up to the nearest boundary
    ret = (void *)(((uint64_t)ret + (MALLOC_2D_PAGE_SIZE * MALLOC_2D_ARENA_SIZE - 1)) & mask);
  } 
  malloc_2d_arena_t *arena = (malloc_2d_arena_t *)ret;
  arena->base = old_ret;
  arena->sc = NULL;
  arena->alloc_page_count = alloc_page_count;
  arena->page_count = page_count;
  arena->free_list = arena->prev = arena->next = NULL;
  malloc_2d_arena_set_huge(arena);
  return arena;
}

// Huge arena is freed directly
void malloc_2d_arena_huge_dealloc(malloc_2d_arena_t *arena) {
  if(arena->sc != NULL) {
    assert(arena->sc->count > 0);
    arena->sc->count--;
  }
  // Remove the arena from sc's free list
  malloc_2d_arena_sc_free_list_remove(arena);
  malloc_2d_arena_free(arena);
  return;
}

void malloc_2d_arena_huge_print(malloc_2d_arena_t *arena) {
  printf("Arena (huge) 0x%lX base 0x%lX pages %d alloc pages %d\n", 
    (uint64_t)arena, (uint64_t)arena->base, arena->page_count, arena->alloc_page_count);
  return;
}

uint64_t malloc_2d_arena_get_size(void *ptr) {
  uint64_t round_mask = ~(MALLOC_2D_PAGE_SIZE * MALLOC_2D_ARENA_SIZE - 1);
  malloc_2d_arena_t *arena = (malloc_2d_arena_t *)((uint64_t)ptr & round_mask);
  int type = malloc_2d_arena_get_type(arena);
  switch(type) {
    case MALLOC_2D_ARENA_FLAGS_OBJ: {
      return (arena->sc->sc_index + 1) * 8UL;
    } break;
    case MALLOC_2D_ARENA_FLAGS_VARLEN: {
      malloc_2d_arena_varlen_header_t *header = \
        (malloc_2d_arena_varlen_header_t *)(MALLOC_2D_PTR_SUB(ptr, sizeof(malloc_2d_arena_varlen_header_t)));
      return (uint64_t)header->size - sizeof(malloc_2d_arena_varlen_header_t);
    } break;
    case MALLOC_2D_ARENA_FLAGS_HUGE: {
      return (uint64_t)arena->page_count * MALLOC_2D_PAGE_SIZE - sizeof(malloc_2d_arena_t);
    } break;
    default: { 
      error_exit("Unknown type: %d (0x%X) on arena deallocation\n", type, type);
    } break;
  }
  return 0UL;
}

// Check whether a given pointer within the arena is free (i.e., in the free list)
// Works for both versions of arena
static int malloc_2d_arena_check_ptr_free(malloc_2d_arena_t *arena, void *ptr) {
  if((uint64_t)ptr < (uint64_t)arena || 
     (uint64_t)ptr >= (uint64_t)((uint8_t *)arena + MALLOC_2D_PAGE_SIZE * MALLOC_2D_ARENA_SIZE)) {
    error_exit("The pointer is not within the given arena\n");
  }
  void *free_list = arena->free_list;
  while(free_list != NULL) {
    if(free_list == ptr) {
      return 1;
    }
    int type = malloc_2d_arena_get_type(arena);
    if(type == MALLOC_2D_ARENA_FLAGS_OBJ) {
      free_list = *(void **)free_list;
    } else if(type == MALLOC_2D_ARENA_FLAGS_VARLEN) {
      free_list = ((malloc_2d_arena_varlen_header_t *)free_list)->next_free;
    }
  }
  return 0;
}

static int malloc_2d_arena_get_free_list_count(malloc_2d_arena_t *arena) {
  void *free_list = arena->free_list;
  int count = 0;
  while(free_list != NULL) {
    int type = malloc_2d_arena_get_type(arena);
    if(type == MALLOC_2D_ARENA_FLAGS_OBJ) {
      free_list = *(void **)free_list;
    } else if(type == MALLOC_2D_ARENA_FLAGS_VARLEN) {
      malloc_2d_arena_varlen_header_t *next_free = ((malloc_2d_arena_varlen_header_t *)free_list)->next_free;
      malloc_2d_arena_varlen_header_t *prev_free = ((malloc_2d_arena_varlen_header_t *)free_list)->prev_free;
      if(next_free != NULL && next_free->prev_free != free_list) {
        printf("WARNING: Inconsistent free list on 0x%lX (curr->next->prev != curr)\n", (uint64_t)free_list);
      }
      if(prev_free != NULL && prev_free->next_free != free_list) {
        printf("WARNING: Inconsistent free list on 0x%lX (curr->prev->next != curr)\n", (uint64_t)free_list);
      }
      if(prev_free == NULL && arena->free_list != free_list) {
        printf("WARNING: Inconsistent free list on 0x%lX (arena->free_list != curr)\n", (uint64_t)free_list);
      }
      free_list = next_free;
    }
    count++;
  }
  return count;
}

// Printing an arena's layout given the obj size
void malloc_2d_arena_obj_print(malloc_2d_arena_t *arena, int obj_size) {
  uint8_t *p = (uint8_t *)arena + sizeof(malloc_2d_arena_t);
  int free_flag = malloc_2d_arena_check_ptr_free(arena, p);
  int span_index = 0;
  printf("Arena (obj) 0x%lX base 0x%lX free %d max %d free list count %d\n", 
    (uint64_t)arena, (uint64_t)arena->base, arena->free_count, arena->max_count, 
    malloc_2d_arena_get_free_list_count(arena));
  for(int i = 0;i < arena->max_count;i++) {
    int curr_free = malloc_2d_arena_check_ptr_free(arena, p);
    if(curr_free != free_flag) {
      // We have seen the end of a free span
      if(free_flag == 0) {
        printf("%d--%d used count %d\n", span_index, i - 1, i - span_index);
      } else {
        printf("%d--%d free count %d\n", span_index, i - 1, i - span_index);
      }
      free_flag = curr_free;
      span_index = i;
    }
    p += obj_size;
  }
  if(free_flag == 0) {
    printf("%d--%d used count %d\n", span_index, arena->max_count - 1, arena->max_count - span_index);
  } else {
    printf("%d--%d free count %d\n", span_index, arena->max_count - 1, arena->max_count - span_index);
  }
  return;
}

// This function also checks the integrity of the arena
void malloc_2d_arena_varlen_print(malloc_2d_arena_t *arena) {
  int free_count = malloc_2d_arena_get_free_list_count(arena);
  printf("Arena (varlen) 0x%lX base 0x%lX free list 0x%lX free %d max %d free list count %d\n", 
    (uint64_t)arena, (uint64_t)arena->base, (uint64_t)arena->free_list,
    arena->free_size, arena->max_size, free_count);
  malloc_2d_arena_varlen_header_t *header = \
    (malloc_2d_arena_varlen_header_t *)MALLOC_2D_PTR_ADD(arena, sizeof(malloc_2d_arena_t));
  malloc_2d_arena_varlen_header_t *prev = NULL;
  void *arena_end = MALLOC_2D_PTR_ADD(arena, MALLOC_2D_PAGE_SIZE * MALLOC_2D_ARENA_SIZE);
  int actual_free_count = 0;
  while(MALLOC_2D_PTR_IS_GEQ(header, arena_end) == 0) {
    printf("  Block 0x%lX data 0x%lX size %d prev size %d next free 0x%lX prev free 0x%lX is free %d\n",
      (uint64_t)header, (uint64_t)MALLOC_2D_PTR_ADD(header, sizeof(malloc_2d_arena_varlen_header_t)),
      header->size, header->prev_size, (uint64_t)header->next_free, (uint64_t)header->prev_free,
      malloc_2d_arena_check_ptr_free(arena, header));
    actual_free_count += !malloc_2d_arena_varlen_header_is_used(header);
    // Check whether prev size matches the actual previous block
    if(prev != NULL && MALLOC_2D_PTR_SUB(header, header->prev_size) != prev) {
      printf("    WARNING: Inconsistent prev_size for middle block\n");
    }
    if(prev == NULL && header->prev_size != 0) {
      printf("    WARNING: Inconsistent prev_size for first block\n");
    }
    prev = header;
    if(header->size == 0) {
      printf("    WARNING: Header size is zero. Terminate print\n");
      break;
    }
    header = (malloc_2d_arena_varlen_header_t *)MALLOC_2D_PTR_ADD(header, header->size);
  }
  if(free_count != actual_free_count) {
    printf("  WARNING: Inconsistent free_count (%d) and actual_free_count (%d)\n",
      free_count, actual_free_count);
  }
  return;
}

void malloc_2d_arena_print(malloc_2d_arena_t *arena, int obj_size) {
  int type = malloc_2d_arena_get_type(arena);
  if(type == MALLOC_2D_ARENA_FLAGS_OBJ) {
    malloc_2d_arena_obj_print(arena, obj_size);
  } else if(type == MALLOC_2D_ARENA_FLAGS_VARLEN) {
    malloc_2d_arena_varlen_print(arena); 
  } else if(type == MALLOC_2D_ARENA_FLAGS_HUGE) {
    malloc_2d_arena_huge_print(arena);
  }
  return;
}

//
//* malloc_2d_sc_t
//

// If sc_index == -1, then we initialize a varlen size class object. Otherwise we initialize object size class
void malloc_2d_sc_init_in_place(malloc_2d_sc_t *sc, uint64_t type_id, int sc_index) {
  assert(sc_index == -1 || sc_index == -2 || (sc_index >= 0 && sc_index < MALLOC_2D_SC_COUNT));
  memset(sc, 0x00, sizeof(malloc_2d_sc_t));
  sc->type_id = type_id;
  sc->sc_index = sc_index;
  switch(sc_index) {
    case MALLOC_2D_SC_INDEX_VARLEN: {
      sc->curr_arena = malloc_2d_arena_varlen_init();
      sc->curr_arena->sc = sc;
    } break;
    case MALLOC_2D_SC_INDEX_HUGE: {
      sc->curr_arena = NULL;
    } break;
    default: {
      sc->curr_arena = malloc_2d_arena_obj_init((sc_index + 1) * MALLOC_2D_SC_INCREMENT);
      sc->curr_arena->sc = sc;
    }
  }
  malloc_2d->stat->sc_init_count++;
  return;
}

malloc_2d_sc_t *malloc_2d_sc_init(uint64_t type_id, int sc_index) {
  malloc_2d_sc_t *sc = (malloc_2d_sc_t *)malloc_2d_sc_obj_alloc(&malloc_2d->meta_sc);
  SYSEXPECT(sc != NULL);
  malloc_2d_sc_init_in_place(sc, type_id, sc_index);
  return sc;
}

void malloc_2d_sc_free_in_place(malloc_2d_sc_t *sc) {
  malloc_2d_arena_t *arena = sc->free_list;
  while(arena != NULL) {
    malloc_2d_arena_t *next = arena->next;
    malloc_2d_arena_free(arena);
    arena = next;
  }
  if(sc->curr_arena != NULL) {
    malloc_2d_arena_free(sc->curr_arena);
  }
  malloc_2d->stat->sc_free_count++;
  return;
}

void malloc_2d_sc_free(malloc_2d_sc_t *sc) {
  malloc_2d_sc_free_in_place(sc);
  malloc_2d_sc_dealloc(sc);
  return;
}

// Allocate an object from the size class; Allocate new arena if the current one is full
void *malloc_2d_sc_obj_alloc(malloc_2d_sc_t *sc) {
  if(malloc_2d_arena_is_full(sc->curr_arena) == 1) {
    if(sc->free_list == NULL) {
      sc->curr_arena = malloc_2d_arena_obj_init((sc->sc_index + 1) * MALLOC_2D_SC_INCREMENT);
      sc->curr_arena->sc = sc;
    } else {
      sc->curr_arena = sc->free_list;
      sc->free_list = sc->curr_arena->next;
      if(sc->curr_arena->next != NULL) {
        sc->curr_arena->next->prev = NULL;
      }
      sc->curr_arena->next = sc->curr_arena->prev = NULL;
      malloc_2d->stat->arena_free_to_curr_count++;
    }
    malloc_2d->stat->arena_curr_to_full_count++;
  }
  sc->count++;
  void *ret = malloc_2d_arena_obj_alloc(sc->curr_arena);
  assert(ret != NULL);
  return ret;
}

// Allocate a varlen block from an sc object
// The process is as follows:
//   1. Try curr_arena;
//   2. If curr_arena does not fulfill the request, then search free_list. We search all free arenas.
//   3. If the previous search still does not find an allocation, then allocate a new arena, and make it the 
//      current arena. The existing current arena will be moved to the free list
// This process is not constant time. In fact, it is linear to the number of varlen blocks.
void *malloc_2d_sc_varlen_alloc(malloc_2d_sc_t *sc, size_t sz) {
  malloc_2d_arena_t *arena = sc->curr_arena;
  void *ptr;
  ptr = malloc_2d_arena_varlen_alloc(arena, (int)sz);
  if(ptr == NULL) {
    arena = sc->free_list;
    while(arena != NULL) {
      ptr = malloc_2d_arena_varlen_alloc(arena, (int)sz);
      if(ptr != NULL) {
        break;
      }
      arena = arena->next;
    }
    if(arena == NULL) {
      assert(ptr == NULL);
      sc->curr_arena->sc = sc;
      malloc_2d_arena_sc_free_list_insert_head(sc->curr_arena);
      sc->curr_arena = malloc_2d_arena_varlen_init();
      sc->curr_arena->sc = sc;
      ptr = malloc_2d_arena_varlen_alloc(sc->curr_arena, (int)sz);
      assert(ptr != NULL);
    }
  }
  sc->count++;
  return ptr;
}

void *malloc_2d_sc_huge_alloc(malloc_2d_sc_t *sc, size_t sz) {
  malloc_2d_arena_t *arena = malloc_2d_arena_huge_init(sz);
  arena->sc = sc;
  // Add the arena into the head of the sc
  malloc_2d_arena_sc_free_list_insert_head(arena);
  sc->count++;
  return MALLOC_2D_PTR_ADD(arena, sizeof(malloc_2d_arena_t));
}

// Print size class information and free list
void malloc_2d_sc_obj_print(malloc_2d_sc_t *sc) {
  printf("Size class (obj) type ID %lu sc index %d free list 0x%lX curr arena 0x%lX\n", 
    sc->type_id, sc->sc_index, (uint64_t)sc->free_list, (uint64_t)sc->curr_arena);
  printf("  Curr arena 0x%lX free %d (used %d)\n", 
    (uint64_t)sc->curr_arena, sc->curr_arena->free_count, sc->curr_arena->max_count - sc->curr_arena->free_count);
  malloc_2d_arena_t *arena = sc->free_list;
  while(arena != NULL) {
    printf("  Free arena 0x%lX free %d (used %d)\n", 
      (uint64_t)arena, arena->free_count, arena->max_count - arena->free_count);
    assert(arena->next == NULL || arena->next->prev == arena);
    assert(arena->prev == NULL || arena->prev->next == arena);
    arena = arena->next;
  }
  return;
}

void malloc_2d_sc_varlen_print(malloc_2d_sc_t *sc) {
  printf("Size class (varlen) free list 0x%lX curr arena 0x%lX\n", 
    (uint64_t)sc->free_list, (uint64_t)sc->curr_arena);
  printf("  Curr arena 0x%lX free %d (used %d)\n", 
    (uint64_t)sc->curr_arena, sc->curr_arena->free_size, sc->curr_arena->max_size - sc->curr_arena->free_size);
  malloc_2d_arena_t *arena = sc->free_list;
  while(arena != NULL) {
    printf("  Free arena 0x%lX free %d (used %d)\n", 
      (uint64_t)arena, arena->free_size, arena->max_size - arena->free_size);
    assert(arena->next == NULL || arena->next->prev == arena);
    assert(arena->prev == NULL || arena->prev->next == arena);
    arena = arena->next;
  }
  return;
}

void malloc_2d_sc_huge_print(malloc_2d_sc_t *sc) {
  printf("Size class (huge) count %d \n",
    sc->count);
  malloc_2d_arena_t *arena = sc->free_list;
  while(arena != NULL) {
    printf("  Huge arena 0x%lX base 0x%lX pages %d alloc pages %d\n", 
      (uint64_t)arena, (uint64_t)arena->base, arena->page_count, arena->alloc_page_count);
    assert(arena->next == NULL || arena->next->prev == arena);
    assert(arena->prev == NULL || arena->prev->next == arena);
    arena = arena->next;
  }
  return;
}

void malloc_2d_sc_print(malloc_2d_sc_t *sc) {
  if(sc->sc_index == MALLOC_2D_SC_INDEX_VARLEN) {
    malloc_2d_sc_varlen_print(sc);
  } else if(sc->sc_index == MALLOC_2D_SC_INDEX_HUGE) {
    malloc_2d_sc_huge_print(sc);
  } else {
    malloc_2d_sc_obj_print(sc);
  }
  return;
}

//
//* malloc_2d_t
//

// Initialize the static object
void malloc_2d_init_static() {
  //fprintf(stderr, "init static\n");
  malloc_2d = &_malloc_2d;
  malloc_2d->stat = &malloc_2d->_stat;
  memset(malloc_2d->stat, 0x00, sizeof(malloc_2d_stat_t));
  // Initialize type-less size classes
  for(int i = 0;i < MALLOC_2D_SC_COUNT;i++) {
    malloc_2d_sc_init_in_place(&malloc_2d->sc_no_type[i], 0UL, i);
  }
  // Initialize the meta sc for allocating other scs
  malloc_2d_sc_init_in_place(&malloc_2d->meta_sc, 0UL, (sizeof(malloc_2d_sc_t) - 1) / 8);
  // Initialize sc for varlen object
  malloc_2d_sc_init_in_place(&malloc_2d->varlen_sc, 0UL, MALLOC_2D_SC_INDEX_VARLEN);
  // Initialize sc for huge object
  malloc_2d_sc_init_in_place(&malloc_2d->huge_sc, 0UL, MALLOC_2D_SC_INDEX_HUGE);
  // Initialize typed size class hash table
  malloc_2d->sc_ht_page_count = \
    ((sizeof(malloc_2d_sc_t *) * MALLOC_2D_SC_HT_INIT_SIZE) + MALLOC_2D_PAGE_SIZE - 1) / MALLOC_2D_PAGE_SIZE;
  malloc_2d->sc_ht = (malloc_2d_sc_t **)malloc_2d_alloc_os_page_unaligned(malloc_2d->sc_ht_page_count);
  SYSEXPECT(malloc_2d->sc_ht != NULL);
  memset(malloc_2d->sc_ht, 0x00, sizeof(malloc_2d_sc_t *) * MALLOC_2D_SC_HT_INIT_SIZE);
  malloc_2d->hash_mask = (uint64_t)MALLOC_2D_SC_HT_INIT_SIZE - 1UL;
  malloc_2d->sc_ht_count = 0;
  malloc_2d->sc_ht_bucket_count = MALLOC_2D_SC_HT_INIT_SIZE;
  return;
}

void malloc_2d_free_static() {
  // Free all sc in the static region first
  for(int i = 0;i < MALLOC_2D_SC_COUNT;i++) {
    malloc_2d_sc_free_in_place(&malloc_2d->sc_no_type[i]);
  }
  // Free all sc in the hash table
  for(int i = 0;i < malloc_2d->sc_ht_bucket_count;i++) {
    malloc_2d_sc_t *sc = malloc_2d->sc_ht[i];
    while(sc != NULL) {
      malloc_2d_sc_t *next = sc->next;
      malloc_2d_sc_free(sc);
      sc = next;
    }
  }
  // Free huge sc
  malloc_2d_sc_free_in_place(&malloc_2d->huge_sc);
  // Free varlen sc
  malloc_2d_sc_free_in_place(&malloc_2d->varlen_sc);
  // Free meta sc -- this function must be called after we freed hash table entries
  malloc_2d_sc_free_in_place(&malloc_2d->meta_sc);
  malloc_2d_free_os_page(malloc_2d->sc_ht, malloc_2d->sc_ht_page_count);
  malloc_2d = NULL;
  return;
}

// This function wraps mmap() for allocating readable, writable, private and anonymous memory
void *malloc_2d_alloc_os_page_unaligned(int count) {
  void *ret = mmap(NULL, MALLOC_2D_PAGE_SIZE * count, PROT_READ | PROT_WRITE, 
    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0UL);
  if(ret == (void *)-1) {
    error_exit("[malloc_2d] mmap() failed with return code %lu (0x%lX)\n", (uint64_t)ret, (uint64_t)ret);
  }
  // Update stats
  malloc_2d->stat->mmap_count++;
  malloc_2d->stat->mmap_page_count += (uint64_t)count;
  return ret;
}

// Allocate virtual addresses used as heap memory
// The function aligns the returned page to "count" page boundaries
// "count" must be a power of 2
// Value "actual base" is the address that should be saved and used on munmap()
void *malloc_2d_alloc_os_page(int count, void **actual_base) {
  // This mask has high bits being one and low bits being zero
  uint64_t mask = ~((MALLOC_2D_PAGE_SIZE * count) - 1);
  // Request 2x larger from the OS
  void *ret = malloc_2d_alloc_os_page_unaligned(count * 2);
  *actual_base = ret;
  if(((uint64_t)ret & mask) != (uint64_t)ret) {
    // Round up to the nearest boundary
    ret = (void *)(((uint64_t)ret + (MALLOC_2D_PAGE_SIZE * count)) & mask);
  } 
  assert(((uint64_t)ret & ~mask) == 0UL);
  return ret;
}

// Free virtual addresses back to the OS
void malloc_2d_free_os_page(void *ptr, int count) {
  int ret = munmap(ptr, MALLOC_2D_PAGE_SIZE * count);
  SYSEXPECT(ret == 0);
  malloc_2d->stat->munmap_count++;
  malloc_2d->stat->munmap_page_count += (uint64_t)count;
  return;
}

inline static uint64_t malloc_2d_get_hash(uint64_t type_id, int sc_index) {
  uint64_t h = (type_id << 9) | sc_index;
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdL;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53L;
  h ^= h >> 33;
  return h & malloc_2d->hash_mask;
}

// This function allocates a new size class and inserts into the hash table of the given type ID and sz
// Returns the newly allocated size class object
malloc_2d_sc_t *malloc_2d_add_new_sc(int ht_index, uint64_t type_id, int sc_index) {
  malloc_2d_sc_t *sc = malloc_2d_sc_init(type_id, sc_index);
  sc->next = malloc_2d->sc_ht[ht_index];
  malloc_2d->sc_ht[ht_index] = sc;
  // TODO: Resizing logic here
  malloc_2d->sc_ht_count++;
  return sc;
}

// Searches the linked list on the hash table, and return if found.
// Returns NULL if not found
malloc_2d_sc_t *malloc_2d_find_sc(int ht_index, uint64_t type_id, int sc_index) {
  assert(ht_index >= 0 && ht_index < malloc_2d->sc_ht_bucket_count);
  malloc_2d_sc_t *sc = malloc_2d->sc_ht[ht_index];
  malloc_2d_sc_t *prev = NULL;
  while(sc != NULL) {
    if(sc->type_id == type_id && sc->sc_index == sc_index) {
      break;
    }
    // Removing the sc from the hash table if it is free
    if(sc->count == 0) {
      malloc_2d_sc_t *gc = NULL;
      if(prev != NULL) {
        prev->next = sc->next;
        gc = sc;
        sc = prev->next;
      } else {
        malloc_2d->sc_ht[ht_index] = sc->next;
        gc = sc;
        sc = malloc_2d->sc_ht[ht_index];
      }
      malloc_2d_sc_free(gc);
      malloc_2d->sc_ht_count--;
    } else {
      prev = sc;
      sc = sc->next;
    }
  }
  return sc;
}

void *malloc_2d_alloc(uint64_t sz) {
  void *ret;
  if(sz > MALLOC_2D_OBJ_MAX_SIZE) {
    if(sz <= MALLOC_2D_VARLEN_MAX_SIZE) {
      //ret = malloc_2d_debug_alloc(sz); 
      ret = malloc_2d_sc_varlen_alloc(&malloc_2d->varlen_sc, sz);
    } else {
      //ret = malloc_2d_debug_alloc(sz);
      ret = malloc_2d_sc_huge_alloc(&malloc_2d->huge_sc, sz);
    }
  } else {
    if(sz == 0UL) {
      sz = 1UL;
    }
    int sc_index = (int)(sz - 1) / MALLOC_2D_SC_INCREMENT;
    ret = malloc_2d_sc_obj_alloc(&malloc_2d->sc_no_type[sc_index]);
  }
  //fprintf(stderr, "malloc_2d_alloc %lu ptr %p\n", sz, ret);
  return ret;
}

// Uses the implicit return address as the type ID
// This piece of code must remain an actual function and must not be inlined. Otherwise the 
// compiler would give the wrong return address
void *malloc_2d_typed_alloc_implicit(uint64_t sz) {
  // This built-in function with argument 0 returns the current return address
  // i.e., the implicit type ID
  return malloc_2d_typed_alloc((uint64_t)__builtin_return_address(0), sz);
}

void *malloc_2d_typed_alloc(uint64_t type_id, uint64_t sz) {
  if(sz > MALLOC_2D_OBJ_MAX_SIZE) {
    if(sz <= MALLOC_2D_VARLEN_MAX_SIZE) {
      return malloc_2d_sc_varlen_alloc(&malloc_2d->varlen_sc, sz);
    } else {
      return malloc_2d_sc_huge_alloc(&malloc_2d->huge_sc, sz);
    }
  } else if(sz == 0UL) {
    sz = 1UL;
  }
  int sc_index = (int)(sz - 1) / MALLOC_2D_SC_INCREMENT;
  uint64_t h = malloc_2d_get_hash(type_id, sc_index);
  assert(h < (uint64_t)malloc_2d->sc_ht_bucket_count);
  malloc_2d_sc_t *sc = malloc_2d_find_sc(h, type_id, sc_index);
  if(sc == NULL) {
    // If no entry found, then allocate a new one
    sc = malloc_2d_add_new_sc(h, type_id, sc_index);
  }
  assert(sc != NULL);
  void *ret = malloc_2d_sc_obj_alloc(sc);
  return ret;
}

uint64_t malloc_2d_get_net_mmap_count() {
  return malloc_2d->stat->mmap_count - malloc_2d->stat->munmap_count;
}

int malloc_2d_get_sc_ht_bucket_count() {
  return malloc_2d->sc_ht_bucket_count;
}

int malloc_2d_get_sc_ht_count() {
  return malloc_2d->sc_ht_count;
}

// Print hash buckets related information
void malloc_2d_print() {
  for(int i = 0;i < MALLOC_2D_SC_COUNT;i++) {
    malloc_2d_sc_t *sc = &malloc_2d->sc_no_type[i];
    if(sc->count != 0) {
      printf("Static sc index %d (%d--%d) count %d curr arena free %d max %d\n",
        sc->sc_index, sc->sc_index * 8 + 1, sc->sc_index * 8 + 8,
        sc->count, sc->curr_arena->free_count, sc->curr_arena->max_count);
    }
  }
  printf("Average chain len %.4lf\n", 
    (double)malloc_2d->sc_ht_count / (double)malloc_2d->sc_ht_bucket_count);
  for(int i = 0;i < malloc_2d->sc_ht_bucket_count;i++) {
    malloc_2d_sc_t *sc = malloc_2d->sc_ht[i];
    while(sc != NULL) {
      printf("Bucket %d type ID %lu (0x%lX) sc index %d (%d--%d) count %d curr arena free %d max %d\n",
        i, sc->type_id, sc->type_id, sc->sc_index, sc->sc_index * 8 + 1, sc->sc_index * 8 + 8,
        sc->count, sc->curr_arena->free_count, sc->curr_arena->max_count);
      sc = sc->next;
    }
  }
  return;
}

malloc_2d_t *malloc_2d_get() {
  return malloc_2d;
}

void malloc_2d_conf_print() {
  printf("---------- malloc_2d conf ----------\n");
  printf("Page size %lu arena size (# of pages) %lu\n", 
    MALLOC_2D_PAGE_SIZE, (uint64_t)MALLOC_2D_ARENA_SIZE);
  printf("Arena (obj) size max %d inc %d size class count %d\n",
    MALLOC_2D_OBJ_MAX_SIZE, MALLOC_2D_SC_INCREMENT, MALLOC_2D_SC_COUNT);
  printf("Arena (varlen) max size %d min size %d alignment %d\n",
    (int)MALLOC_2D_VARLEN_MAX_SIZE, (int)MALLOC_2D_VARLEN_MIN_SIZE, (int)MALLOC_2D_VARLEN_ALIGNMENT);
  printf("HT init buckets %d pages %d\n",
    MALLOC_2D_SC_HT_INIT_SIZE, 
    (int)(((sizeof(malloc_2d_sc_t *) * MALLOC_2D_SC_HT_INIT_SIZE) + MALLOC_2D_PAGE_SIZE - 1) / MALLOC_2D_PAGE_SIZE));
  printf("SC size %lu sc index %d\n",
    sizeof(malloc_2d_sc_t), (int)(sizeof(malloc_2d_sc_t) - 1) / 8);
  return;
}

// Printing stats
void malloc_2d_stat_print() {
  printf("---------- malloc_2d stat ----------\n");
  malloc_2d_stat_t *stat = malloc_2d->stat;
  printf("Malloc %lu free %lu\n", stat->malloc_count, stat->free_count);
  printf("Mmap %lu pages %lu\n", stat->mmap_count, stat->mmap_page_count);
  printf("Munmap %lu pages %lu\n", stat->munmap_count, stat->munmap_page_count);
  printf("SC init %lu free %lu\n", stat->sc_init_count, stat->sc_free_count);
  printf("   meta count %d\n", malloc_2d->meta_sc.count);
  printf("Arena init %lu free %lu curr_to_full %lu full_to_free %lu free_to_curr %lu\n",
    stat->arena_init_count, stat->arena_free_count, stat->arena_curr_to_full_count,
    stat->arena_full_to_free_count, stat->arena_free_to_curr_count);
  printf("HT curr buckets %d count %d mask 0x%lX (%d buckets) pages %d\n",
    malloc_2d->sc_ht_bucket_count, malloc_2d->sc_ht_count, malloc_2d->hash_mask, 
    (int)(malloc_2d->hash_mask + 1), malloc_2d->sc_ht_page_count);
  return;
}

#ifdef MALLOC_2D_LIB

extern "C" {

void __attribute__((constructor)) init() {
  if(malloc_2d == NULL) {
    malloc_2d_init_static();
  }
  return;
}

void __attribute__((destructor)) fini() {
  //if(malloc_2d != NULL) {
  //  malloc_2d_free_static();
  //}
  return;
}

void *malloc(size_t sz) {
  if(malloc_2d == NULL) {
    malloc_2d_init_static();
  }
  void *ptr = malloc_2d_alloc(sz);
  //fprintf(stderr, "malloc sz %lu\n", sz);
  return ptr;
}

void *calloc(size_t sz, size_t count) {
  //fprintf(stderr, "calloc sz %lu count %lu\n", sz, count);
  void *ptr = malloc_2d_alloc(sz * count);
  memset(ptr, 0x00, sz * count);
  return ptr;
}

void *realloc(void *old, size_t sz) {
  if(old == NULL) {
    return malloc_2d_alloc(sz);
  } else if(sz == 0UL) {
    malloc_2d_dealloc(old);
    return NULL;
  }
  uint64_t old_sz = malloc_2d_arena_get_size(old);
  //fprintf(stderr, "realloc old %p sz %lu old sz %lu\n", old, sz, old_sz);
  if(sz == old_sz) {
    return old;
  } 
  void *ptr = malloc_2d_alloc(sz);
  memcpy(ptr, old, (sz > old_sz) ? old_sz : sz);
  malloc_2d_dealloc(old);
  return ptr;
}

void free(void *ptr) {
  //fprintf(stderr, "free ptr %p\n", ptr);
  malloc_2d_dealloc(ptr);
  return;
}

void *aligned_alloc(size_t alignment, size_t size) {
  error_exit("Not implemented\n"); (void)alignment; (void)size;
}

void *memalign(size_t alignment, size_t size) {
  error_exit("Not implemented\n"); (void)alignment; (void)size;
}

int posix_memalign(void **memptr, size_t alignment, size_t size) {
  error_exit("Not implemented\n"); (void)memptr; (void)alignment; (void)size;
}

void *valloc(size_t size) {
  error_exit("Not implemented\n"); (void)size;
}

void *pvalloc(size_t size) {
  error_exit("Not implemented\n"); (void)size;
}

size_t malloc_usable_size(void *ptr) {
  error_exit("Not implemented\n"); (void)ptr;
}

}

#endif
