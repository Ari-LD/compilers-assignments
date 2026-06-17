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
    int d = arr[i + 2] +
            3; // if we operate on the variable before it is used for
               // indexing, it's necessary to use the long data type in order to
               // avoid the insertion of "sext" instructions which
               // could break the calculation for negative dependencies
  }
  return 0;
}
