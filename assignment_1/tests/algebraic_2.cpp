int test_algebraic_identities(int a, int b) {

    int add_0 = a + 0;
    int sub_0 = a - 0;
    int shl_0 = a << 0;
    int ashr_0 = a >> 0; 
    int mul_1 = a * 1;
    int sdiv_1 = a / 1;
    int and_minus1 = a & -1;
    int or_0 = a | 0;
    int xor_0 = a ^ 0;

    int sub_self = a - a;       // -> 0
    int sdiv_self = a / a;      // -> 1
    int and_self = a & a;       // -> a
    int or_self = a | a;        // -> a
    int xor_self = a ^ a;       // -> 0
    int srem_self = a % a;      // -> 0
    int srem_1 = a % 1;         // -> 0
    int mul_0 = a * 0;          // -> 0
    int zero_mul = 0 * b;       // -> 0

    return zero_mul + add_0 + sdiv_self;
}