int fun(int n) {
  int i = 0;
  int j = 0;
  int k = 0;

  while (i < n) {
    int a = i + 3;
    i++;
  }
  while (j < n) {
    int b = j * 2;
    j++;
  }

  while (k < n + 2) {
    int c = k / 2;
    k++;
  }
  return 0;
}