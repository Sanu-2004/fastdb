#ifndef REPL_H
#define REPL_H
#include<db.h>

// void setValueCmd(Node** root, char* s, char* v);

void getValueCmd(Node* root, char* s);

void repl(DB* db);

#endif