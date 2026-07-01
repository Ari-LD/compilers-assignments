int fun(int a, int b){

    int g = 0;
    int f = 5;
    for(int i = 0; i < a; i++){
        if(i > a){
            g = f + 2;
        }
    }

    b = g + a;

    return 0;
}