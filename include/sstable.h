#ifndef SSTABLE_H
#define SSTABLE_H
#define DIRNAME "tables"
#define levels 3
#include<db.h>

static int folderStructureExists = 0;

void createFoldertructure();

void createSSTable(DB* db);

void writeNodeToSSTable(Node* node, FILE* sstable);

#endif