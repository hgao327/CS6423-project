#include <stdio.h>

int a[50];
int b[100];
int c[200];

int main(int argc, char *argv[]) {
  int i = argc;
  if (argc > 3) {
    a[i] = argc;
  } else {
    b[i] = argc;
  }
  c[i] = argc;
  return 0;
}