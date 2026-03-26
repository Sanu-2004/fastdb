#ifndef REPL_H
#define REPL_H
#include<db.h>

// void setValueCmd(Node** root, char* s, char* v);

void getValueCmd(DB* db, char* s);

void repl(DB* db);

#endif