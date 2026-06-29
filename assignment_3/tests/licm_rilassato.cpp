#include <stdio.h>
int fun(int a, int b){

    int d = a;
    int result = 0;
    for(int i = 0; i<a; i++){
        if(a == 2){
            result = d+1;
        }else{
            result = d+2;
        }
        int result2 = result +1;
    }
    //ora la uso
    //printf("%d", result);
    //int f = result + 1;
    return 0;
}