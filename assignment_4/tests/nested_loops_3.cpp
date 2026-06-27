int fun(int a, int b){
    int i = 0,c;
    do{
        for(int j = 0; j<a; j++){
            b = i+j;
        }
        for(int j = 0; j<a; j++){
            c = i-j;
        }
        i++;
    }while(i<a);

    return 0;
}