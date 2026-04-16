int fun (int x)
{
    int a = x & 5;
    int b = a & 5;

    int c = x | 5;
    int d = c | 5;

    int e = x ^ 5;
    int f = e ^ 5;

    //con solo due istruzioni funziona correttamente
    //con queste tre per qualche motivo elimina la seconda e fa una AND tra 
    //la costante dell'ultima e 0 (non dovrebbe)
    //se al posto di 8 mettiamo una roba tipo 1 allora funziona, quindi boh
    int g = x & 8;
    int h = g & 5;
    int i = h & 2;

    int w = x ^ 3;
    int y = w ^ 7;
    int z = y ^ 4;

    return i + z;
}