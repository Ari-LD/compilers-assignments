int fun(int a,int b){

    int i = 0;
    int j = 0;
    int g[100];
    int d = 0;

    if(a>0){
        do{
        g[i+1] = 4;
        i++;
    }while(i<a);
    }
    

    if(a>0){
        do{
            d = g[j] + 1;
            j++;
        }while(j<a);
    }

    return 0;
}