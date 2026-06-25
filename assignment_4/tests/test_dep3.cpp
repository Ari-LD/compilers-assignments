int fun(int a, int b){
    int d=0;
    int c[100]; // Assuming a maximum size of 100
    for (int i = 0; i < a; i++) {

        c[i] = a*b;
    }

    for(int i = 0; i< a; i++){
        d = c[i+1] + 1;
    }

    return 0;

}