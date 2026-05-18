int fun(int n, int x, int y) {
    int invariant_calc = 0;
    int sum = 0;
    int b = 3;

    int i = 0;

    do {

        int a = b+1; //loop invariant

        if (x > 10) {
            invariant_calc = x + y; //loop invariant
                                    //even if it doesn't dominate all exits (and is not dead after the loop), in SSA form this instruction can be moved
                                    //(phi instruction chooses which def. to use)
        }

        sum += i;

        i++;
    } while (i < n);

    return sum + invariant_calc;
}