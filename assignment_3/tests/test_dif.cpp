int fun(int a, int b){


    int g = 0;
    int f = 5;
    /*
        Con il for e while  l'istruzione non dominerà mai le uscite perchè la condizione è nell'header 
        e nel cfg quando controllo la dominanza non controllo la condizione ma controllo solo se esiste un percorso, 
        quindi posso spostarla solo se è dead after loop;
        col do while non succede
    */
    
    for(int i = 0; i < f; i++){
        
        g = f + 2;
        
    }
    
        /*
    int i = 0;
    do{
        g = f + 2;
        i++;
    }while(i < f);
    */

    b = g + a;

    return 0;
}