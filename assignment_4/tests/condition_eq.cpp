int fun(int a[5], int b[5], long condition) {
  long j = 0;
  long i = 0;
  if (j < condition) {
    do {
      a[i] = i;
      i++;
    } while (i < 5);
  }
  if (condition > j) {
    i = 0;
    do {
      b[i] = a[i] + 1;
      i++;
    } while (i < 5);
  }
  return 0;
}