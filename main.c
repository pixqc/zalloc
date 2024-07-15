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

void test_fba(Allocator *fba) {
  char *str1 = (char *)fba->vtable->alloc(fba, 20);
  assert(str1 != NULL);
  memcpy(str1, "aaaaaaaaaaaaaaaaaaa\0", 20);
  assert(strlen(str1) == 19);
  assert(str1[20] == (char)0xAA);

  char *str2 = (char *)fba->vtable->alloc(fba, 11);
  bool resize1 = fba->vtable->resize(fba, str1, 24, 1);
  assert(resize1 == false); // only last allocation can resize
  assert(str2 != NULL);
  assert(str2 == str1 + 24); // 8byte alignment
  memcpy(str2, "xxxxxxxxxx\0", 11);
  assert(strlen(str2) == 10);
  assert(str2[11] == (char)0xAA);

  bool resize2 = fba->vtable->resize(fba, str2, 16, 5);
  assert(resize2 == true);
  assert(((FixedBufferAllocator *)fba)->offset == (void *)((char *)str2 + 8));

  char *str3 = (char *)fba->vtable->alloc(fba, 2);
  assert(str3 != NULL);
  assert(str3 == str2 + 8); // should be right after the resized str2
  memcpy(str3, "z\0", 2);
  assert(strlen(str3) == 1);
  assert((char *)str3 + 8 == (char *)((FixedBufferAllocator *)fba)->offset);

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

  // create new page
  ArenaAllocator *new_arena = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (new_arena == MAP_FAILED) {
    perror("mmap failed");
    exit(1);
  }

  memset(new_arena, 0xAA, 4096);
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
  ArenaAllocator *arena = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (arena == MAP_FAILED) {
    perror("mmap failed");
    exit(1);
  }
  memset(arena, 0xAA, 1000);

  arena->base.vtable = &arena_vtable;
  size_t arena_size = (sizeof(ArenaAllocator) + 7) & ~7; // 8-byte alignment
  arena->offset = arena + arena_size;
  arena->next = NULL;
  return (Allocator *)arena;
}

void test_arena(Allocator *arena) {
  ArenaAllocator *arena_alloc = (ArenaAllocator *)arena;

  char *str1 = (char *)arena->vtable->alloc(arena, 20);
  assert(str1 != NULL);
  memcpy(str1, "aaaaaaaaaaaaaaaaaaa\0", 20);
  assert(strlen(str1) == 19);
  assert(str1[24] == (char)0xAA);

  char *str2 = (char *)arena->vtable->alloc(arena, 11);
  bool resize1 = arena->vtable->resize(arena, str1, 24, 1);
  assert(resize1 == false);
  assert(str2 != NULL);
  assert(str2 == str1 + 24); // 8-byte alignment
  memcpy(str2, "xxxxxxxxxx\0", 11);
  assert(strlen(str2) == 10);

  bool resize2 = arena->vtable->resize(arena, str2, 16, 5);
  assert(resize2 == true);
  assert(arena_alloc->offset == str2 + 8);

  char *str3 = (char *)arena->vtable->alloc(arena, 2);
  assert(str3 != NULL);
  assert(str3 == str2 + 8); // 8-byte alignment
  memcpy(str3, "z\0", 2);
  assert(strlen(str3) == 1);

  // does it allocate new page
  assert(arena_alloc->next == NULL);
  char *str4 = (char *)arena->vtable->alloc(arena, 4040);
  assert(arena_alloc->next != NULL);
  assert(str4 != NULL);
  memcpy(str4, "bbb\0", 4);

  // str4 should be on next page
  size_t arena_size = (sizeof(ArenaAllocator) + 7) & ~7; // 8-byte alignment
  assert(str4 == (char *)arena_alloc->next + arena_size);
  assert(str4 != str3 + 8);

  // fill existing available bucket
  char *str5 = (char *)arena->vtable->alloc(arena, 3);
  assert(str5 != NULL);
  assert(str5 != str4 + 8);
  assert(str5 == str3 + 8);
  memcpy(str5, "55\0", 2);

  // free page
  arena->vtable->free(arena, NULL);
  // uncomment to check whether mem has been destroyed
  // memset(str1, 1, 1);
  // memset(str4, 1, 1);

  printf("all arena allocator tests passed\n");
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

  test_fba(fba);
  test_arena(arena);

  // TODO: general purpose allocator

  return 0;
}
