#ifndef DB_H
#define DB_H
#include<memTree.h>
#include<stdio.h>
#include<stdlib.h>
#include <config.h>


typedef struct DB{
    Node* root;
    FILE* wal;
    int size;
} DB;

DB* createEmptyDB();

void insert(DB* db, char* key, char* val);

char* get(DB* db, char* key);

void restoreTree(DB* db, FILE* wal);

FILE* getRecentWalFile(const char* dir);

FILE* createWalFile();

void clearWALFiles(const char* dir);

#endif