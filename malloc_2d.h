
#ifndef _MALLOC_2D
#define _MALLOC_2D

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>
#include <assert.h>
#include <unistd.h>
#include <sys/mman.h>

// Error reporting and system call assertion
#define SYSEXPECT(expr) do { if(!(expr)) { perror(__func__); assert(0); exit(1); } } while(0)
#define SYSEXPECT_FILE(expr, path) do { if(!(expr)) { printf("File operation failed with path: \"%s\"\n", path); perror(__func__); assert(0); exit(1); } } while(0)
#define error_exit(fmt, ...) do { fprintf(stderr, "%s error: " fmt, __func__, ##__VA_ARGS__); assert(0); exit(1); } while(0);
#ifndef NDEBUG
#define dbg_printf(fmt, ...) do { fprintf(stderr, fmt, ##__VA_ARGS__); } while(0);
#else
#define dbg_printf(fmt, ...) do {} while(0);
#endif

// Number of bytes in a page
#define MALLOC_2D_PAGE_SIZE 4096UL
// Number of pages in an arena
#define MALLOC_2D_ARENA_SIZE 16
// Maximum size of objects
#define MALLOC_2D_OBJ_MAX_SIZE    512
// Alignment of varlen block
#define MALLOC_2D_VARLEN_ALIGNMENT  8
// Maximum size of single varlen object (including varlen header)
#define MALLOC_2D_VARLEN_MAX_SIZE   ((MALLOC_2D_PAGE_SIZE * MALLOC_2D_ARENA_SIZE) - sizeof(malloc_2d_arena_t))
// Minimum size of single varlen object (including varlen header)
#define MALLOC_2D_VARLEN_MIN_SIZE   (MALLOC_2D_OBJ_MAX_SIZE + MALLOC_2D_VARLEN_ALIGNMENT + sizeof(malloc_2d_arena_varlen_header_t))
// Size class increment
#define MALLOC_2D_SC_INCREMENT 8
// Size class count
#define MALLOC_2D_SC_COUNT     (MALLOC_2D_OBJ_MAX_SIZE / MALLOC_2D_SC_INCREMENT)
// Size class hash table init size
#define MALLOC_2D_SC_HT_INIT_SIZE    4096

inline static void *MALLOC_2D_PTR_ADD(void *ptr, int size) {
  return (void *)((uint8_t *)ptr + size);
}
inline static void *MALLOC_2D_PTR_SUB(void *ptr, int size) {
  return (void *)((uint8_t *)ptr - size);
}
inline static int MALLOC_2D_PTR_IS_GEQ(void *ptr1, void *ptr2) {
  return (((uint64_t)ptr1) >= ((uint64_t)ptr2));
}

// Allocate virtual addresses used as heap memory
void *malloc_2d_alloc_os_page_unaligned(int count);
void *malloc_2d_alloc_os_page(int count, void **actual_base);

// Free virtual addresses back to the OS
void malloc_2d_free_os_page(void *ptr, int count);

typedef struct {
  uint64_t malloc_count;
  uint64_t free_count;
  uint64_t mmap_count;
  uint64_t munmap_count;
  uint64_t mmap_page_count;
  uint64_t munmap_page_count;
  // This counts both type-less sc and typed sc
  uint64_t sc_init_count;
  uint64_t sc_free_count;
  // Arena stats
  uint64_t arena_init_count;
  uint64_t arena_free_count;
  uint64_t arena_curr_to_full_count;
  uint64_t arena_full_to_free_count;
  uint64_t arena_free_to_curr_count;
} malloc_2d_stat_t;

struct malloc_2d_sc_struct_t;

// Whether the arena is varlen arena
#define MALLOC_2D_ARENA_FLAGS_OBJ          0x00000000
#define MALLOC_2D_ARENA_FLAGS_VARLEN       0x00000001
#define MALLOC_2D_ARENA_FLAGS_HUGE         0x00000002
#define MALLOC_2D_ARENA_FLAGS_TYPE_MASK    0x00000003

// Object header for varlen blocks
typedef struct malloc_2d_arena_varlen_header_struct_t {
  // Size of the previous block, pointing to the header; 0 if this is the first
  int prev_size;
  // Size of the current block, including this header
  int size;
  // Point to the next free block, if it is a free block; NULL means it is the last free block
  struct malloc_2d_arena_varlen_header_struct_t *next_free;
  // Point to the prev free block. NULL means it is the first free block
  struct malloc_2d_arena_varlen_header_struct_t *prev_free;
} malloc_2d_arena_varlen_header_t;

inline static void malloc_2d_arena_varlen_header_set_used(malloc_2d_arena_varlen_header_t *header) {
  header->next_free = (malloc_2d_arena_varlen_header_t *)1;
}
inline static int malloc_2d_arena_varlen_header_is_used(malloc_2d_arena_varlen_header_t *header) {
  return header->next_free == (malloc_2d_arena_varlen_header_t *)1;
}

typedef struct malloc_2d_arena_struct_t {
  // This stores the actual base that should be munmap'ed
  void *base; 
  // Points to the size class
  struct malloc_2d_sc_struct_t *sc;
  union {
    // This is used for object arena
    struct {
      int free_count;
      int max_count;
    };
    // This is used for var-len arena, indicating physical size including the header
    struct {
      int free_size;
      int max_size;
    };
    // This is used for huge arena
    struct {
      // Page count of the body. Used for computing data size
      int page_count;
      // Actual pages mmap'ed. Used for munmap
      int alloc_page_count;
    };
  };
  int flags;
  // Points to the next free object in the arena
  void *free_list;
  // Chain arenas into a free list; Full arenas are not in any list
  struct malloc_2d_arena_struct_t *prev;
  struct malloc_2d_arena_struct_t *next;
} malloc_2d_arena_t;

// Initialize an arena, with optional argument specifying the number of pages
// obj_size may not be multiple of page size
malloc_2d_arena_t *malloc_2d_arena_obj_init(int obj_size);
void malloc_2d_arena_free(malloc_2d_arena_t *arena);

// Remove the given header from the free list of the given arena
// This operation only updates free list and does not update size
void malloc_2d_arena_varlen_free_list_remove(malloc_2d_arena_t *arena, malloc_2d_arena_varlen_header_t *header);
void malloc_2d_arena_varlen_free_list_insert_head(malloc_2d_arena_t *arena, malloc_2d_arena_varlen_header_t *header);
// Remove the arena from the free list of its associated SC object
void malloc_2d_arena_sc_free_list_remove(malloc_2d_arena_t *arena);
void malloc_2d_arena_sc_free_list_insert_head(malloc_2d_arena_t *arena);

// Initialize an varlen arena
malloc_2d_arena_t *malloc_2d_arena_varlen_init();
void malloc_2d_arena_obj_dealloc(malloc_2d_arena_t *arena, void *ptr);
void malloc_2d_arena_varlen_dealloc(malloc_2d_arena_t *arena, void *ptr);
void malloc_2d_arena_dealloc(void *ptr);

// Initialize huge arena given the size of the allocated object
malloc_2d_arena_t *malloc_2d_arena_huge_init(size_t sz);
void malloc_2d_arena_huge_dealloc(malloc_2d_arena_t *arena);
void malloc_2d_arena_huge_print(malloc_2d_arena_t *arena);

// Given a pointer, return allocated size (physical size)
uint64_t malloc_2d_arena_get_size(void *ptr);

inline static int malloc_2d_arena_is_full(malloc_2d_arena_t *arena) {
  return arena->free_count == 0;
}
inline static int malloc_2d_arena_get_free_count(malloc_2d_arena_t *arena) {
  return arena->free_count;
}
inline static int malloc_2d_arena_get_max_count(malloc_2d_arena_t *arena) {
  return arena->max_count;
}

inline static int malloc_2d_arena_get_type(malloc_2d_arena_t *arena) {
  return arena->flags & MALLOC_2D_ARENA_FLAGS_TYPE_MASK;
}
inline static void malloc_2d_arena_set_type(malloc_2d_arena_t *arena, int type) {
  arena->flags &= ~MALLOC_2D_ARENA_FLAGS_TYPE_MASK;
  arena->flags |= type;
}

inline static void malloc_2d_arena_set_obj(malloc_2d_arena_t *arena) {
  malloc_2d_arena_set_type(arena, MALLOC_2D_ARENA_FLAGS_OBJ);
}
inline static void malloc_2d_arena_set_varlen(malloc_2d_arena_t *arena) {
  malloc_2d_arena_set_type(arena, MALLOC_2D_ARENA_FLAGS_VARLEN);
}
inline static void malloc_2d_arena_set_huge(malloc_2d_arena_t *arena) {
  malloc_2d_arena_set_type(arena, MALLOC_2D_ARENA_FLAGS_HUGE);
}

// Allocate an object from the arena; Returns NULL if fails. 
void *malloc_2d_arena_obj_alloc(malloc_2d_arena_t *arena);
// Size can be arbitrary value less than max varlen size; we round it up to 4-byte boundary
void *malloc_2d_arena_varlen_alloc(malloc_2d_arena_t *arena, int size);

void malloc_2d_arena_obj_print(malloc_2d_arena_t *arena, int obj_size);
void malloc_2d_arena_varlen_print(malloc_2d_arena_t *arena);
void malloc_2d_arena_print(malloc_2d_arena_t *arena, int obj_size);

//
//* malloc_2d_sc_t
//

#define MALLOC_2D_SC_INDEX_VARLEN     -1
#define MALLOC_2D_SC_INDEX_HUGE       -2

// Size class object
typedef struct malloc_2d_sc_struct_t {
  uint64_t type_id;
  // Size class, 0 means 1--8 bytes, 1 means 9--16 bytes, etc.
  int sc_index;
  // Number of live objects; Used to determine whether the sc will be removed
  int count;
  // Arenas that have at least one free object
  malloc_2d_arena_t *free_list;
  // Current arena that serves allocation
  malloc_2d_arena_t *curr_arena;
  // Next and prev pointer
  struct malloc_2d_sc_struct_t *next;
  struct malloc_2d_sc_struct_t *prev;
} malloc_2d_sc_t;

void malloc_2d_sc_init_in_place(malloc_2d_sc_t *sc, uint64_t type_id, int sc_index);
malloc_2d_sc_t *malloc_2d_sc_init(uint64_t type_id, int sc_index);
void malloc_2d_sc_free_in_place(malloc_2d_sc_t *sc);
void malloc_2d_sc_free(malloc_2d_sc_t *sc);

void *malloc_2d_sc_obj_alloc(malloc_2d_sc_t *sc);
void *malloc_2d_sc_varlen_alloc(malloc_2d_sc_t *sc, size_t sz);
void *malloc_2d_sc_huge_alloc(malloc_2d_sc_t *sc, size_t sz);
inline static void malloc_2d_sc_dealloc(void *ptr) {
  malloc_2d_arena_dealloc(ptr);
}

void malloc_2d_sc_obj_print(malloc_2d_sc_t *sc);
void malloc_2d_sc_varlen_print(malloc_2d_sc_t *sc);
void malloc_2d_sc_huge_print(malloc_2d_sc_t *sc);
void malloc_2d_sc_print(malloc_2d_sc_t *sc);

typedef struct {
  // Allocation without size class - avoid affecting applications that do not use types
  malloc_2d_sc_t sc_no_type[MALLOC_2D_SC_COUNT];
  // This implements sc object allocation
  malloc_2d_sc_t meta_sc;
  // SC for allocation between 512 and arena size (MALLOC_2D_PAGE_SIZE * MALLOC_2D_ARENA_SIZE)
  malloc_2d_sc_t varlen_sc;
  // We only use its free list; The curr is always NULL
  malloc_2d_sc_t huge_sc;
  // Size class hash table pointer
  malloc_2d_sc_t **sc_ht;
  // Number of elements in the hash table
  int sc_ht_count;
  // Number of buckets
  int sc_ht_bucket_count;
  // Number of pages for storing all buckets
  int sc_ht_page_count;
  uint64_t hash_mask;
  malloc_2d_stat_t _stat;
  malloc_2d_stat_t *stat;
} malloc_2d_t;

// Initialize the static object in-place. Do not return anything
void malloc_2d_init_static();
void malloc_2d_free_static();

malloc_2d_sc_t *malloc_2d_add_new_sc(int ht_index, uint64_t type_id, int sz_index);
malloc_2d_sc_t *malloc_2d_find_sc(int ht_index, uint64_t type_id, int sc_index);

void *malloc_2d_alloc(uint64_t sz);
void *malloc_2d_typed_alloc_implicit(uint64_t sz);
void *malloc_2d_typed_alloc(uint64_t type_id, uint64_t sz);
inline static void malloc_2d_dealloc(void *ptr) {
  //fprintf(stderr, "malloc_2d_dealloc ptr %p\n", ptr);
  if(ptr != NULL) {
    malloc_2d_sc_dealloc(ptr);
  }
  //fprintf(stderr, "  ret\n");
}

uint64_t malloc_2d_get_net_mmap_count();

int malloc_2d_get_sc_ht_bucket_count();
int malloc_2d_get_sc_ht_count();
void malloc_2d_print();

malloc_2d_t *malloc_2d_get();

void malloc_2d_conf_print();
void malloc_2d_stat_print();

#ifdef MALLOC_2D_LIB

extern "C" {

void __attribute__ ((constructor)) init();
void __attribute__ ((destructor)) fini();

void *malloc(size_t sz);
void *calloc(size_t sz, size_t count);
void *realloc(void *old, size_t sz);
void free(void *ptr);

void *aligned_alloc(size_t alignment, size_t size);
void *memalign(size_t alignment, size_t size);
int posix_memalign(void **memptr, size_t alignment, size_t size);
void *valloc(size_t size);
void *pvalloc(size_t size);
size_t malloc_usable_size(void *ptr);

}

#endif

#endif