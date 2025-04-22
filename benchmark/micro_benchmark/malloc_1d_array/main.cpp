#include <stdio.h>
#include <stdlib.h>

#define CMD_USAGE                                        \
do {                                                     \
  printf("malloc_id_array.exe [ARRAY_SIZE]\n");          \
} while (0)

int main(int argc, char *argv[]) {
  if (argc <= 1) {
    CMD_USAGE;
    return 1;
  }
  int size = atoi(argv[1]);
  if (!size) {
    printf("invalid size: %s\n", argv[1]);
    CMD_USAGE;
    return 1;
  }
  int *array = (int *)malloc(size * sizeof(int));
  array[argc] = 0;
  for (int i = 0; i < size + 1; i++) {
    // should report a out-of-bound here
    array[i] = i;
  }
  
  int idx = size + 1;
  // should report a out-of-bound here
  printf("array[%d] = %d\n", idx, array[idx + 1]);
  return 0;
}