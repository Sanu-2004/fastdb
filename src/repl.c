#include<repl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include<sstable.h>
#include<ctype.h>

char* trim(char *str) {
    char *end;

    // Trim leading whitespace
    while(isspace((unsigned char)*str)) {
        str++;
    }

    // If all spaces
    if(*str == 0) {
        return str;
    }

    // Trim trailing whitespace
    end = str + strlen(str) - 1;
    while(end > str && isspace((unsigned char)*end)) {
        end--;
    }

    // Null terminate after last non-space character
    *(end + 1) = '\0';

    return str;
}

void getValueCmd(DB* db, char* s) {
    char* val = get(db, trim(s));
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
            getValueCmd(db, key);
        }
        else if(strcmp(cmd, "WRITE") == 0){
            createSSTable(db);
        }
        else if(strcmp(cmd, "PRINT") == 0){
            inorder(db->root);
            printf("\n");
        } else if(strcmp(cmd, "SIZE") == 0){
            printf("WAL Size: %d bytes\n", db->size);
        }
        else{
            puts("Help:\nSET key value\nGET key\nPRINT");
        }
    }
    free(str);
}