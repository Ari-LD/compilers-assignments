int fun (int a, int b)
{
    int c;
    a = b * 3;
    c = a / 3;

    int d = a << 10;
    int e = d >> 5;
    int ret = e >> 5;
    
    return e;
}