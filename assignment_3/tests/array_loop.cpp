int main() {
  int arr[100];

  for (int i = 0; i < 100; i++) {
    arr[i] = 10;
    arr[0] = 1;
    int c = arr[i] + 2;
  }
}