#ifndef MEMTREE_H
#define MEMTREE_H

typedef enum { RED, BLACK } Color;

typedef struct Node
{
    char* key;
    char* value;
    Color color;
    struct Node* left;
    struct Node* right;
    struct Node* parent;
} Node;

Node* createNode(char* key, char* val);

void leftRotate(Node** root, Node* x);

void rightRotate(Node** root, Node* y);

void fixInsert(Node** root, Node* node);

void insertTree(Node** root, char* key, char* val);

void inorder(Node* root);

char* getValue(Node* root, char* key);

void FreeTree(Node* root);

#endif