#include<repl.h>
#include<db.h>
#include<stdlib.h>




int main(){
    DB* db = createEmptyDB();

    repl(db);

    return 0;
}