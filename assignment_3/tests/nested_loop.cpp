void fun(int n, int m){

    int b=3;
    int c=4;
    int e;

    int y;

    for(int i=0;i<n;i++){
        int a = b+c; //loop invariant

        if(a<=10){
            e = 2+a; //in SSA form this operation can be moved outside, even if technically
                    //it redefines another variable present in the loop (phi instruction decides which def. will be used)
        }
        else {
            e = 3;
        }
        for(int j=0; j < m; ++j) {
            int x = a + 3; //outer loop invariant
            y = i + x; //inner loop invariant

            int z = j + 2;
        }
        
        int d = a+1; //loop invariant
        int f = e+2; //not loop invariant (more than one reaching def. for e)
                    // in SSA form it depends on a phi instruction
    }
}