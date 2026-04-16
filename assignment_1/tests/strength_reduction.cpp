int fun (int a, int b)
{
    unsigned int z = a / 4;

    int c = a * 8;
    int d = b * 9;
    int e = a * 18;
    int f = b * 6;

    int g = (a+b) / 4;
    int h = a / -8;

    int x = a * 40;
    int y = b * 56;
    int w = a * -1;
    int v = b * -4;
    
    int r1 = b % 8;
    unsigned int r2 = b % 4;
    int r3 = -a % 8;
    
    //the return won't be optimized because while something akin to
    // v * 24 would be optimized it would take 3 instructions
    //because 24 is negative it becomes 4 instructions making it
    //never better than a standard mul -AK
    return v * - 24;
}