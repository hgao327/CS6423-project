#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
  if (argc <= 2) {
    printf("sum [lower_bound] [upperbound]\n");
    return 1;
  }
  int lower = atoi(argv[1]);
  int upper = atoi(argv[2]);
  int sum = 0;
  for (int i = lower; i < upper; i++) {
    sum += i;
  }
  printf("sum(%d, %d) = %d\n", lower, upper, sum);
  return 0;
}