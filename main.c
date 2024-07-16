//
//  .     .   .      o       .          .       *  . .     .
//    .  *  |     .    .            .   .     .   .     * .    .
//        --o--       zig allocators       *    |      ..    .
//     *    |       *  .        .    .   .    --*--  .     *  .
//  .     .    .    .   . . .      .        .   |   .    .  .
//

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

static void *new_page_memory(size_t size) {
  void *page = mmap(NULL, size, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (page == MAP_FAILED) {
    perror("mmap failed");
    exit(1);
  }
  memset(page, 0xAA, size);
  return page;
}

typedef struct Allocator Allocator;

// won't live on stack or heap but on a secret third thing
typedef struct AllocatorVTable {
  void *(*alloc)(Allocator *self, size_t size);
  void (*free)(Allocator *self, void *ptr);
  bool (*resize)(Allocator *self, void *ptr, size_t old_size, size_t new_size);
} AllocatorVTable;

struct Allocator {
  const AllocatorVTable *vtable;
};

typedef struct {
  Allocator base;
  size_t size;
  void *offset;
} FixedBufferAllocator;

static void *fixed_buffer_alloc(Allocator *self, size_t size) {
  FixedBufferAllocator *fba = (FixedBufferAllocator *)self;
  size = (size + 7) & ~7; // 8-byte alignment
  if ((char *)fba->offset + size > (char *)fba + fba->size)
    return NULL;
  void *result = fba->offset;
  fba->offset = (char *)fba->offset + size;
  return result;
}

static void fixed_buffer_free(Allocator *self, void *ptr) {
  (void)self;
  (void)ptr;
}

static bool fixed_buffer_resize(Allocator *self, void *ptr, size_t old_size,
                                size_t new_size) {
  FixedBufferAllocator *fba = (FixedBufferAllocator *)self;
  new_size = (new_size + 7) & ~7; // 8-byte alignment
  if ((char *)ptr + old_size != fba->offset)
    return false;
  if ((char *)fba->offset - old_size + new_size > (char *)fba + fba->size)
    return false;
  fba->offset = (char *)fba->offset - old_size + new_size;
  return true;
}

AllocatorVTable fixed_buffer_vtable = {.alloc = fixed_buffer_alloc,
                                       .free = fixed_buffer_free,
                                       .resize = fixed_buffer_resize};

Allocator *create_fixed_buffer_allocator(void *buffer, size_t size) {
  FixedBufferAllocator *fba = (FixedBufferAllocator *)buffer;
  fba->base.vtable = &fixed_buffer_vtable;
  fba->size = size;
  fba->offset = (char *)buffer + sizeof(FixedBufferAllocator);
  return (Allocator *)fba;
}

void test_fba(Allocator *allocator) {
  FixedBufferAllocator *fba = (FixedBufferAllocator *)allocator;
  char *str1 = (char *)allocator->vtable->alloc(allocator, 20);
  assert(str1 != NULL);
  memcpy(str1, "aaaaaaaaaaaaaaaaaaa\0", 20);
  assert(strlen(str1) == 19);
  assert(str1[20] == (char)0xAA);

  char *str2 = (char *)allocator->vtable->alloc(allocator, 11);
  bool resize1 = allocator->vtable->resize(allocator, str1, 24, 1);
  assert(resize1 == false); // only last allocation can resize
  assert(str2 != NULL);
  assert(str2 == str1 + 24); // 8byte alignment
  memcpy(str2, "xxxxxxxxxx\0", 11);
  assert(strlen(str2) == 10);
  assert(str2[11] == (char)0xAA);
  bool resize2 = allocator->vtable->resize(allocator, str2, 16, 5);
  assert(resize2 == true);
  assert(fba->offset == (void *)((char *)str2 + 8));

  char *str3 = (char *)allocator->vtable->alloc(allocator, 2);
  assert(str3 != NULL);
  assert(str3 == str2 + 8); // should be right after the resized str2
  memcpy(str3, "z\0", 2);
  assert(strlen(str3) == 1);
  assert((char *)str3 + 8 == (char *)fba->offset);
  printf("all fixed buffer allocator tests passed\n");
}

// arena is linked list of pages
typedef struct ArenaAllocator {
  Allocator base;
  void *offset;
  struct ArenaAllocator *next;
} ArenaAllocator;

static void *arena_alloc(Allocator *self, size_t size) {
  if (size <= 0 || size > 4096) {
    perror("mmap failed");
    exit(1);
  }

  ArenaAllocator *arena = (ArenaAllocator *)self;
  size = (size + 7) & ~7; // 8-byte alignment

  // try to find space in existing pages
  ArenaAllocator *current = arena;
  while (current != NULL) {
    if (current->offset + size <= (void *)self + 4096) {
      void *result = current->offset;
      current->offset = (char *)current->offset + size;
      return result;
    }
    current = current->next;
  }

  ArenaAllocator *new_arena = new_page_memory(4096);
  new_arena->base = arena->base;
  size_t arena_size = (sizeof(ArenaAllocator) + 7) & ~7;
  new_arena->offset = (void *)new_arena + arena_size;
  new_arena->next = NULL;

  current = arena;
  while (current->next != NULL) {
    current = current->next;
  }
  current->next = new_arena;

  void *result = new_arena->offset;
  new_arena->offset = (char *)new_arena->offset + size;
  return result;
}

// technically "destroy"
void arena_free(Allocator *allocator, void *ptr) {
  // do nothing with ptr since we just free up everything
  ArenaAllocator *arena = (ArenaAllocator *)allocator;
  while (arena) {
    ArenaAllocator *next = arena->next;
    munmap(arena, 4096);
    arena = next;
  }
}

static bool arena_resize(Allocator *self, void *ptr, size_t old_size,
                         size_t new_size) {
  ArenaAllocator *arena = (ArenaAllocator *)self;
  new_size = (new_size + 7) & ~7; // 8-byte alignment

  // last allocation?
  if ((char *)ptr + old_size == arena->offset) {
    if ((char *)ptr + new_size <= (char *)arena + 4096) {
      arena->offset = (char *)ptr + new_size;
      return true;
    }
  }
  return false;
}

AllocatorVTable arena_vtable = {
    .alloc = arena_alloc, .free = arena_free, .resize = arena_resize};

Allocator *create_arena_allocator() {
  ArenaAllocator *arena = new_page_memory(4096);
  arena->base.vtable = &arena_vtable;
  size_t arena_size = (sizeof(ArenaAllocator) + 7) & ~7; // 8-byte alignment
  arena->offset = arena + arena_size;
  arena->next = NULL;
  return (Allocator *)arena;
}

void test_arena(Allocator *allocator) {
  ArenaAllocator *arena = (ArenaAllocator *)allocator;

  char *str1 = (char *)allocator->vtable->alloc(allocator, 20);
  assert(str1 != NULL);
  memcpy(str1, "aaaaaaaaaaaaaaaaaaa\0", 20);
  assert(strlen(str1) == 19);
  assert(str1[24] == (char)0xAA);

  char *str2 = (char *)allocator->vtable->alloc(allocator, 11);
  bool resize1 = allocator->vtable->resize(allocator, str1, 24, 1);
  assert(resize1 == false);
  assert(str2 != NULL);
  assert(str2 == str1 + 24); // 8-byte alignment
  memcpy(str2, "xxxxxxxxxx\0", 11);
  assert(strlen(str2) == 10);
  bool resize2 = allocator->vtable->resize(allocator, str2, 16, 5);
  assert(resize2 == true);
  assert(arena->offset == str2 + 8);

  char *str3 = (char *)allocator->vtable->alloc(allocator, 2);
  assert(str3 != NULL);
  assert(str3 == str2 + 8); // 8-byte alignment
  memcpy(str3, "z\0", 2);
  assert(strlen(str3) == 1);

  // does it allocate new page
  assert(arena->next == NULL);
  char *str4 = (char *)allocator->vtable->alloc(allocator, 4040);
  assert(arena->next != NULL);
  assert(str4 != NULL);
  memcpy(str4, "bbb\0", 4);
  // str4 should be on next page
  size_t arena_size = (sizeof(ArenaAllocator) + 7) & ~7; // 8-byte alignment
  assert(str4 == (char *)arena->next + arena_size);
  assert(str4 != str3 + 8);

  // fill existing available bucket
  char *str5 = (char *)allocator->vtable->alloc(allocator, 3);
  assert(str5 != NULL);
  assert(str5 != str4 + 8);
  assert(str5 == str3 + 8);
  memcpy(str5, "55\0", 2);

  // free page
  allocator->vtable->free(allocator, NULL);
  // uncomment to check whether mem has been destroyed
  // memset(str1, 1, 1);
  // memset(str4, 1, 1);

  printf("all arena allocator tests passed\n");
}

typedef struct Allocator Allocator;
typedef struct GPABucket GPABucket;
typedef struct GeneralPurposeAllocator GeneralPurposeAllocator;

struct GPABucket {
  void *offset;
  size_t bucket_size;
  GPABucket *next;
};

struct GeneralPurposeAllocator {
  Allocator base;
  GPABucket *buckets[12];
};

static inline int log2_ceil(size_t x) {
  int result = 0;
  size_t value = 1;
  if (x == 0)
    return 0;
  while (value < x) {
    value <<= 1;
    result++;
  }
  return result;
}

static void *gpa_alloc(Allocator *self, size_t size) {
  GeneralPurposeAllocator *gpa = (GeneralPurposeAllocator *)self;
  if (size <= 0 || size > 4096) {
    perror("mmap failed");
    exit(1);
  }

  int bucket_index = log2_ceil(size);
  size_t bucket_size = 1 << bucket_index;

  if (bucket_index >= 12) {
    perror("allocation size too large for available buckets");
    exit(1);
  }

  // if bucket doesn't exist, create it
  if (gpa->buckets[bucket_index] == NULL) {
    gpa->buckets[bucket_index] = new_page_memory(4096);
    gpa->buckets[bucket_index]->bucket_size = bucket_size;
    gpa->buckets[bucket_index]->offset =
        (char *)gpa->buckets[bucket_index] + sizeof(GPABucket);
    gpa->buckets[bucket_index]->next = NULL;
  }
  GPABucket *bucket = gpa->buckets[bucket_index];

  bool oom = (char *)bucket->offset + bucket_size > (char *)bucket + 4096;
  if (oom) {
    GPABucket *new_bucket = new_page_memory(4096);
    new_bucket->bucket_size = bucket_size;
    new_bucket->offset = (char *)new_bucket + sizeof(GPABucket);
    new_bucket->next = bucket;
    gpa->buckets[bucket_index] = new_bucket;
    bucket = new_bucket;
  }

  void *result = bucket->offset;
  bucket->offset = (char *)bucket->offset + bucket_size;
  return result;
}

static void gpa_free(Allocator *self, void *ptr) {
  (void)self;
  (void)ptr;
}

static bool gpa_resize(Allocator *self, void *ptr, size_t old_size,
                       size_t new_size) {
  GeneralPurposeAllocator *gpa = (GeneralPurposeAllocator *)self;
  size_t old_aligned_size = (size_t)1 << log2_ceil(old_size);
  int old_bucket_idx = log2_ceil(old_size);
  GPABucket *old_bucket = gpa->buckets[old_bucket_idx];

  // last allocation?
  if ((char *)ptr + old_aligned_size != old_bucket->offset) {
    return false;
  }

  // resize can only happen in same bucket
  // for different-bucket resize, use alloc+free on callsite
  size_t new_aligned_size = (size_t)1 << log2_ceil(new_size);
  int new_bucket_idx = log2_ceil(new_size);
  if (new_bucket_idx > old_bucket_idx) {
    return false;
  }

  if (new_size < old_size) {
    memset((char *)ptr + new_size, 0xAA, old_size - new_size);
  }
  return true;
}

AllocatorVTable gpa_vtable = {
    .alloc = gpa_alloc, .free = gpa_free, .resize = gpa_resize};

Allocator *create_gpa_allocator() {
  // FIXME: inefficient because it's using the 4kb to store GPA's metadata
  GeneralPurposeAllocator *gpa = new_page_memory(4096);
  gpa->base.vtable = &gpa_vtable;
  size_t gpa_size = (sizeof(GeneralPurposeAllocator) + 7) & ~7;
  for (int i = 0; i < 12; i++) {
    gpa->buckets[i] = NULL;
  }
  return (Allocator *)gpa;
}

void test_gpa(Allocator *allocator) {
  GeneralPurposeAllocator *gpa = (GeneralPurposeAllocator *)allocator;
  char *str1 = (char *)allocator->vtable->alloc(allocator, 1);
  assert(str1 != NULL);
  assert(gpa->buckets[0] != NULL);
  assert(gpa->buckets[0]->bucket_size == 1);
  *str1 = 'a';
  char *str2 = (char *)allocator->vtable->alloc(allocator, 20);
  assert(str2 != NULL);
  assert(gpa->buckets[5] != NULL);
  memcpy(str2, "bucket5\n", 8);
  char *str3 = (char *)allocator->vtable->alloc(allocator, 300);
  assert(str3 != NULL);
  assert(gpa->buckets[9] != NULL);
  assert(gpa->buckets[9]->bucket_size == 512);
  memcpy(str3, "bucket9\n", 8);
  // can't resize to different bucket, use alloc+free for that
  bool resize1 = allocator->vtable->resize(allocator, str1, 1, 2);
  assert(resize1 == false);
  bool resize2 = allocator->vtable->resize(allocator, str2, 20, 30);
  assert(resize2 == true);
  bool resize2_2 = allocator->vtable->resize(allocator, str2, 30, 1);
  assert(resize2 == true);
  assert(str2[0] == 'b');
  assert(str2[1] == (char)0xAA);

  // // Test allocation that should create a new page in the same bucket
  // char *str4 = (char *)gpa->vtable->alloc(gpa, 3000);
  // assert(str4 != NULL);
  // assert(gpa_alloc->buckets[11] != NULL);
  // assert(gpa_alloc->buckets[11]->bucket_size == 2048);
  // assert(gpa_alloc->buckets[11]->next != NULL);
  //
  // // Test maximum allocation size
  // char *str5 = (char *)gpa->vtable->alloc(gpa, 4000);
  // assert(str5 != NULL);
  // assert(gpa_alloc->buckets[11] != NULL);
  // assert(gpa_alloc->buckets[11]->bucket_size == 2048);
  //
  // // Attempt to allocate more than the maximum size (should fail)
  // char *str6 = (char *)gpa->vtable->alloc(gpa, 5000);
  // assert(str6 == NULL);
  //
  // // Free all allocations (note: GPA doesn't actually free memory)
  // gpa->vtable->free(gpa, str1);
  // gpa->vtable->free(gpa, str2);
  // gpa->vtable->free(gpa, str3);
  // gpa->vtable->free(gpa, str4);
  // gpa->vtable->free(gpa, str5);
  //
  printf("all gpa allocator tests passed\n");
}

// "Why do I have to pass allocators around in Zig?"
// because userland decides which allocation strategy to use
// and where the data should be placed
void alloc_hello(Allocator *allocator) {
  const char *str = "hello world\n";
  size_t str_size = strlen(str) + 1;
  char *str1 = (char *)allocator->vtable->alloc(allocator, str_size);
  memcpy(str1, str, str_size);
}

int main() {
  char buf[1000];
  memset(buf, 0xAA, 1000);
  Allocator *fba = create_fixed_buffer_allocator(buf, sizeof(buf));
  alloc_hello(fba);

  Allocator *arena = (Allocator *)create_arena_allocator();
  alloc_hello(arena);

  Allocator *gpa = create_gpa_allocator();
  alloc_hello(gpa);

  test_fba(fba);
  test_arena(arena);
  test_gpa(gpa);

  return 0;
}
