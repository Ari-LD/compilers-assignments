int fun(int n) {
  int i = 0;
  int j = 0;

  if (i == 0) {
    do {
      int b = 3 + i;
      i++;
    } while (i < n);
  }

  int a = j + 20;

  if (j == 0) {
    do {
      int c = j * 2;
      j++;
    } while (j < n);
  }

  return 0;
}