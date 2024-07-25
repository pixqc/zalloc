**zalloc**: zig allocators implemented in c

example usage:

```c
// "Why do I have to pass allocators around in Zig?"
// because userland decides which allocation strategy to use
// and where the data should be placed
void alloc_hello(Allocator *allocator) {
  const char *str = "hello world\n";
  size_t str_size = strlen(str) + 1;
  MemoryBlock str1 = allocator->vtable->alloc(allocator, str_size);
  memcpy(str1.ptr, str, str_size);
}

int main() {
  char buf[1000];
  memset(buf, 0xAA, 1000);
  Allocator *fba = create_fixed_buffer_allocator(buf, sizeof(buf));
  alloc_hello(fba);

  Allocator *arena = create_arena_allocator();
  alloc_hello(arena);

  Allocator *gpa = create_gpa_allocator();
  alloc_hello(gpa);

  return 0;
}
```

for educational purposes only
