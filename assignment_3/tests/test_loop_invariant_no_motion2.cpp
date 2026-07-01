#include <stdio.h>
int fun(int a, int b){

    int d = 0;
    int g = 0;
    for(int i = 0; i<a; i++){
        d = g + 1;
        printf("%d\n", d);
    }


    return 0;
}