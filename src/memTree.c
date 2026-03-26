#include<stdio.h>
#include<stdlib.h>
#include<memTree.h>
#include<string.h>

Node* createNode(char* key, char* val) {
    Node* node = (Node*)malloc(sizeof(Node));
    node->key = strdup(key);
    node->value = strdup(val);
    node->color = RED;
    node->left = NULL;
    node->right = NULL;
    node->parent = NULL;
    return node;
}

void leftRotate(Node** root, Node* x) {
    Node* y = x->right;
    x->right = y->left;

    if(y->left) y->left->parent = x;

    y->parent = x->parent;
    if(!x->parent) *root = y;
    else if(x==x->parent->left) x->parent->left = y;
    else x->parent->right = y;

    y->left = x;
    x->parent = y;
}

void rightRotate(Node** root, Node* y) {
    Node* x = y->left;
    y->left = x->right;

    if(x->right) x->right->parent = y;

    x->parent = y->parent;
    if(!y->parent) *root = x;
    else if(y==y->parent->left) y->parent->left = x;
    else y->parent->right = x;

    x->right = y;
    y->parent = x;
}

void fixInsert(Node** root, Node* node) {
    while(node != *root && node->parent->color == RED) {
        Node* parent = node->parent;
        Node* grandParent = parent->parent;

        if(parent == grandParent->left) {
            Node* uncle = grandParent->right;

            if(uncle && uncle->color == RED) {
                parent->color = BLACK;
                uncle->color = BLACK;
                grandParent->color = RED;
                node = grandParent;
            } else {
                if(node == parent->right){
                    node = parent;
                    leftRotate(root, node);
                }
                parent->color = BLACK;
                grandParent->color = RED;
                rightRotate(root, grandParent);
            }
        } else {
            Node* uncle = grandParent->left;

            if(uncle && uncle->color == RED) {
                parent->color = BLACK;
                uncle->color = BLACK;
                grandParent->color = RED;
                node = grandParent;
            } else {
                if(node == parent->left) {
                    node = parent;
                    rightRotate(root, node);
                }
                parent->color = BLACK;
                grandParent->color = RED;
                leftRotate(root, grandParent);
            }
        }
    }
    (*root)->color = BLACK;
}

void insertTree(Node** root, char* key, char* val) {
    Node* parent = NULL;
    Node* current = *root;
    int cmp = 0;

    while(current) {
        parent = current;
        cmp = strcmp(key, current->key);

        if(cmp==0){
            free(current->value);
            current->value = strdup(val);
            return;
        } else if(cmp < 0) {
            current = current->left;
        } else {
            current = current->right;
        }
    }

    Node* node = createNode(key, val);
    node->parent = parent;

    if(!parent) *root = node;
    else if(cmp < 0) parent->left = node;
    else parent->right = node;

    fixInsert(root, node);
}

void inorder(Node* root){
    if(!root) return;
    inorder(root->left);
    printf("%s (%s), ", root->key, root->color==RED?"R":"B");
    inorder(root->right);
}

char*  getValue(Node* root, char* data) {
    while(root) {
        int cmp = strcmp(data, root->key);
        if(cmp == 0) return root->value; 
        else if(cmp > 0) root = root->right;
        else root = root->left;
    }
    return NULL;    
}

void FreeTree(Node* root) {
    if (!root) return;
    FreeTree(root->left);
    FreeTree(root->right);
    free(root->key);
    free(root->value);
    free(root);
}