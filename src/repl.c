#include<repl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// void setValueCmd(Node** root, char* k, char* v) {
//     // insert(root, k, v);
//     print()
// }

void getValueCmd(Node* root, char* s) {
    char* val = getValue(root, s);
    printf("%s\n", val?val:"NULL");
}

void repl(DB* db){
    char* str = NULL;
    size_t len = 0;
    puts("Stared Repl! \n");
    while(1) {
        getline(&str, &len, stdin);

        char* saveptr;
        char* cmd = strtok_r(str, " \n", &saveptr);
        if(!cmd) continue;

        if(strcmp(cmd, "SET") == 0){
            char* key = strtok_r(NULL, " ", &saveptr);
            char* val = strtok_r(NULL, "\n", &saveptr);
            if(!key || !val) {
                puts("Usage: SET key value");
                continue;
            }
            insert(db, key, val);
        }
        else if(strcmp(cmd, "GET") == 0){
            char* key = strtok_r(NULL, " ", &saveptr);
            getValueCmd(db->root, key);
        }
        else if(strcmp(cmd, "PRINT") == 0){
            inorder(db->root);
            printf("\n");
        }
        else{
            puts("Help:\nSET key value\nGET key\nPRINT");
        }
    }
    free(str);
}