/**
 * This version is stamped on May 10, 2016
 *
 * Contact:
 *   Louis-Noel Pouchet <pouchet.ohio-state.edu>
 *   Tomofumi Yuki <tomofumi.yuki.fr>
 *
 * Web address: http://polybench.sourceforge.net
 */
/* jacobi-1d.c: this file is part of PolyBench/C */

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <math.h>

#define TSTEPS 10000
#define N 40000

int main(int argc, char** argv)
{
  /* Retrieve problem size. */
  int n = N;
  int tsteps = TSTEPS;

  double A[N];
  double B[N];

  /* Initialize array(s). */
  for (int i = 0; i < n; i++) {
	  A[i] = ((double) i+ 2) / n;
	  B[i] = ((double) i+ 3) / n;
  }


  /* Run kernel. */
  for (int t = 0; t < tsteps; t++) {
    for (int i = 1; i < n - 1; i++)
	    B[i] = 0.33333 * (A[i-1] + A[i] + A[i + 1]);
    for (int i = 1; i < n - 1; i++)
	    A[i] = 0.33333 * (B[i-1] + B[i] + B[i + 1]);
  }


  /* Prevent dead-code elimination. All live-out data must be printed
     by the function call in argument. */
  for (int i = 0; i < n; i++) {
    if (i % 20 == 0) 
      printf("0.2%lf ", A[i]);
  }
  printf("\n");


  return 0;
}
