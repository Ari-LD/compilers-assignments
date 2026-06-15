int fun(int n) {
  int c = 0;
  for (int i = 0; i < n; i++) {
    c = i + 1;
  }

  // int a = z + 2;
  // c = 2;
  int d = c + 1;
  c++;
  // int b = d + 2;
  // int z = 5+2;

  // if (d == 3)
  //   d = 5;

  for (int i = 0; i < n; i++) {
    d = i + 2;
    c = 20;
  }

  // conditions to be moved:
  // 1) to move back -> its operands must not be overwritten in the previos loop
  // 2) to move after -> the next loop must not utilize/redefine the variable
  // 3) instructions dependent on each other must be on the same side
  int a = c + 12;
  int x = 10;
  // int x = 3 + a;

  for (int j = 0; j < n; j++) {
    int e = x / 2;
  }

  return a;
}