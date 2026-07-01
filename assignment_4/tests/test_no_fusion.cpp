int fun(int a, int b){

    int c[100];
    int d = 0;
    for(long i = 0; i<a; i++){
        c[i] = 3;
        
    }

    for(long j = 0; j<a; j++){
        d = c[j+1] + 1;
    }

    return 0;
}