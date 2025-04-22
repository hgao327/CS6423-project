/**
This file contains some utility functions that Image processing kernels require

Written by Pankaj Kukreja
Indian Institute of Technology Hyderabad
*/

#include "ImageHelper.h"
#include "glibc_compat_rand.h"
#include <stdlib.h>
#include <stdio.h>

// Initialize a random Image
void initializeRandomImage(int *image, int height, int width) {
  glibc_compat_srand(7);
  for (int i = 0; i < height; i++) {
    for (int j = 0; j < width; j++) {
      image[i * width + j] = glibc_compat_rand() % 256;
    }
  }
}

// Save Image to outputFile
void saveImage(int *image, char *outputFile, int height, int width) {
  FILE *outfile = fopen(outputFile, "w+");
  for (int i = 0; i < height; i++) {
    for (int j = 0; j < width; j++) {
      // Just for safety
      if (image[i * width + j] > 255) {
        fprintf(outfile, "255 ");
      } else if (image[i * width + j] < 0) {
        fprintf(outfile, "0 ");
      } else {
        fprintf(outfile, "%d ", image[i * width + j]);
      }
    }
    fprintf(outfile, "\n");
  }
  fclose(outfile);
}

// Initializes a random RGB Image
void initializeRandomColouredImage(int *image, int height, int width) {
  glibc_compat_srand(7);
  for (int i = 0; i < height; i++) {
    for (int j = 0; j < width; j++) {
      image[i * (width * 3) + j * 3 + 0] = glibc_compat_rand() % 256;
      image[i * (width * 3) + j * 3 + 1] = glibc_compat_rand() % 256;
      image[i * (width * 3) + j * 3 + 2] = glibc_compat_rand() % 256;
    }
  }
}

// Read Pixel values of a black n white Image from $inputFile
void initializeImage(int *image, char *inputFile, int height, int width) {
  FILE *inFile = fopen(inputFile, "r");

  if (!inFile) {
    printf(" Can't open file %s\n", inputFile);
    exit(1);
  }

  for (int i = 0; i < height; i++) {
    for (int j = 0; j < width; j++) {
      fread(&image[i * width + j], sizeof(int), 1, inFile);
    }
  }
  fclose(inFile);
}

// Read Pixel values of a coloured Image from $inputFile
void initializeColoredImage(int *image, char *inputFile, int height,
                            int width) {
  FILE *inFile = fopen(inputFile, "r");
  if (!inFile) {
    printf(" Can't open file %s\n", inputFile);
    exit(1);
  }

  for (int i = 0; i < height; i++) {
    for (int j = 0; j < width; j++) {
      fread(&image[i * width + j + 0], sizeof(int), 1, inFile);
      fread(&image[i * width + j + 1], sizeof(int), 1, inFile);
      fread(&image[i * width + j + 2], sizeof(int), 1, inFile);
    }
  }
  fclose(inFile);
}
