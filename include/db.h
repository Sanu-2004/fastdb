#ifndef DB_H
#define DB_H
#include<memTree.h>
#include<stdio.h>
#include<stdlib.h>

typedef struct DB{
    Node* root;
    FILE* wal;
} DB;

DB* createEmptyDB();

void insert(DB* db, char* key, char* val);

void restoreTree(DB* db, FILE* wal);

FILE* getRecentWalFile(const char* dir);

FILE* createWalFile();

#endif