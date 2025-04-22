#include <stdio.h>
#define N 1000

int array[N];

int main(int argc, char *argv[]) {
  array[argc] = 0;
  for (int i = 0; i < N + 1; i++) {
    // should report a out-of-bound here
    array[i] = i;
  }
  
  int idx = N + 1;
  // should report a out-of-bound here
  printf("array[%d] = %d\n", idx, array[idx + 1]);
  return 0;
}