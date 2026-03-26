#ifndef SSTABLE_H
#define SSTABLE_H
#include<db.h>
#include<config.h>

typedef struct Footer {
    long long index_offset;
    long long total_entries;
} Footer;

void createFoldertructure();

void createSSTable(DB* db);

void writeNodeToSSTable(Node* node, FILE* sstable, long long* line, long long* offsets);

char* readValueFromSSTable(FILE* sstable, long long* indexs, const char* key, long long left, long long right);

char* searchSSTables(const char* key);

#endif