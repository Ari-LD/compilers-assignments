int fun(int a,int b){
    int d[a];
    for(int i = 0;i<a; i++){
        d[i] = 3;
    }

    for(int i = 0; i<a; i++){
        d[i+1] = 5;
    }
    return 0;
}