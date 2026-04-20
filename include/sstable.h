#ifndef SSTABLE_H
#define SSTABLE_H
#include<db.h>
#include<config.h>
#include<stdbool.h>

typedef struct Footer {
    long long index_offset;
    bool isSparse;
    long long indexSize;
} Footer;

typedef struct indexList {
    long long* key_offset;
    int ptr;
    int level;
    struct indexList* next;
} indexList;

indexList* createIndexList(int level);

void freeIndexList(indexList* list);

void writeIndexListToFile(indexList* list, FILE* file, bool forceWriteLastNode);

void createFoldertructure();

void createSSTable(DB* db);

void writeNodeToSSTable(Node* node, FILE* sstable, long long* line, indexList* indexHead);

char* searchValueFromSSTable(FILE* sstable, long long* indexs, const char* key, bool isSparse, long long left, long long right);


char* lookIntoSSTables(const char* key);

int sstableCount(const char* dir);

void compactSSTables(int level);

void mergeSSTables(int level, char* file1, char* file2);

#endif