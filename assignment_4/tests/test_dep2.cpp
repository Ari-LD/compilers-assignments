int fun(int n, int arr[]) {
  int c = 10;
  for (int i = 0; i < n; i++) {
    c = i;
  }
  for (int i = 0; i < n; i++) {
    int d = i + c;
    arr[i] = d * 2;
  }
  for (int i = 0; i < n; i++) {
    int e = arr[i];
  }
  return 0;
}