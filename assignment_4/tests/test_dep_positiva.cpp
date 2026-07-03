void test_negative_dep(int *A, int *B, int N) {
    for (long i = 0; i < N; i++) {
        A[i+1] = 3 * 2;     
    }
    for (long i = 0; i < N; i++) {
        B[i] = A[i] + 1;     
    }
}