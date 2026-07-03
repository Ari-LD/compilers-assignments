void test_positive_dep(int *A, int *B, int N) {
    for (long i = 0; i < N; i++) {
        A[i] = i * 2;     
    }
    for (long i = 0; i < N; i++) {
        B[i] = A[i+1] + 1;     
    }
}