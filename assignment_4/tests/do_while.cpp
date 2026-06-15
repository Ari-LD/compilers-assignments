int fun(int n) {
  int i = 0;
  int j = 0;
  int k = 0;

  do {
    int b = 3 + i;
    i++;
  } while (i < n);

  int z = 7 + i;

  do {
    int c = j * 2;
    j++;
  } while (j < n);

  do {
    int e = k + 6;
  } while (k < n * 2);

  return 0;
}