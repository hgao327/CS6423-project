#include <stdio.h>
// if this function is defined in a cpp file, then the function declaration
// should be: extern "C" void checkInput(int argc, char *argv[])
void checkInput(int argc, char *argv[]) {
  printf("Num of input parameters: %d\n", argc);
  for (int i = 0; i < argc; i++) {
    printf("  Arg %d: %s\n", i, argv[i]);
  }
}