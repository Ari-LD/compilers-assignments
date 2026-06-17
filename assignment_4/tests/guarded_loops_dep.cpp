int fun(int n, int arr1[], int arr2[], int arr3[]) {
  int i = 0;
  int j = 0;
  int k = 0;

  if (i == 0) {
    do {
      arr1[i] = i;
      i++;
    } while (i < n);
  }

  // this should fuse
  if (j == 0) {
    do {
      arr2[j] = arr1[j];
      j++;
    } while (j < n);
  }

  // this should not fuse because of a negative dependency
  if (k == 0) {
    do {
      arr3[k] = arr1[k+2];
      k++;
    } while (k < n);
  }

  return 0;
}
