/**
  Pankaj Kukreja
  github.com/proton0001
  Indian Institute of Technology Hyderabad
*/
#include "ImageHelper.h"
#include "dither.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

int *inputImage;
//extern "C" {
void orderedDitherKernel(int height, int width, int *inpImage, int *outImage,
                         int *temp, int n, int m);
void floydDitherKernel(int height, int width, int *inpImage, int *outImage);
//}
int main(int argc, char **argv) {

  char *orderedOutputFilename = "./orderedOutput.txt";
  char *floydOutputFilename = "./floydOutput.txt";
  int length = (HEIGHT) * (WIDTH);
  inputImage = (int *)malloc(sizeof(int) * length);
  if (!inputImage) {
    printf("Insufficient memory\n");
    exit(1);
  }
  initializeRandomImage(inputImage, HEIGHT, WIDTH);

  int *outputImage = (int *)malloc(sizeof(int) * length);
  int *temp = (int *)malloc(sizeof(int) * length);
  if (!outputImage || !temp) {
    printf("Insufficient memory\n");
    exit(1);
  }
  orderedDitherKernel(HEIGHT, WIDTH, inputImage, outputImage, temp, 16, 4);
  saveImage(outputImage, orderedOutputFilename, HEIGHT, WIDTH);
  floydDitherKernel(HEIGHT, WIDTH, inputImage, outputImage);

  for (int i = 0; i < HEIGHT; i++) {
    outputImage[(i)*WIDTH + 0] = 0;
    outputImage[(i)*WIDTH + WIDTH - 1] = 0;
  }

  for (int j = 0; j < WIDTH; j++) {
    outputImage[(0) * WIDTH + j] = 0;
    outputImage[(HEIGHT - 1) * WIDTH + j] = 0;
  }

  saveImage(outputImage, floydOutputFilename, HEIGHT, WIDTH);
  free(temp);
  free(outputImage);
  free(inputImage);
  return EXIT_SUCCESS;
}