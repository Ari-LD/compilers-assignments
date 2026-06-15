int fun(int n, int m) {
  int a = 0, b = 0;
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < m; j++) {
      a = j + i;
    }
  }
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < m; j++) {
      b = j * i;
    }
  }
  return a + b;
}