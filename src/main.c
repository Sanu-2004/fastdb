#include<repl.h>
#include<db.h>
#include<stdlib.h>


void generateRandomString(char* str, size_t length) {
    static const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (size_t i = 0; i < length - 1; i++) {
        int key = rand() % (int)(sizeof(charset) - 1);
        str[i] = charset[key];
    }
    str[length - 1] = '\0';
}

void insertTestData(DB* db, int count) {
    char key[16];
    char value[16];
    insert(db, "SANU", "IS AWESOME");
    for (int i = 0; i < count; i++) {
        generateRandomString(key, sizeof(key));
        generateRandomString(value, sizeof(value));
        insert(db, key, value);
    }
}

int main(){
    DB* db = createEmptyDB();

    insertTestData(db, 11);

    repl(db);

    return 0;
}