int fun(int n, int arr[]) {
  int c = 10;
  for (long i = 0; i < n; i++) {
    c = i;
  }
  for (long i = 0; i < n; i++) {
    // int d = i + c;
    arr[i] = c * 2;
  }
  for (long i = 0; i < n; i++) {
    int d = arr[i + 2] + 3;
  }
  return 0;
}