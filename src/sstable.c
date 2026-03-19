#include<sstable.h>
#include<dirent.h>
#include<string.h>
#include<time.h>
#include<sys/stat.h>

void createFoldertructure() {
    DIR* dir = opendir(DIRNAME);
    if (!dir) {
        mkdir(DIRNAME, 0755);
        for(int i = 1; i <= levels; i++) {
            char levelDir[256];
            snprintf(levelDir, sizeof(levelDir), "%s/level%d", DIRNAME, i);
            DIR* levelDirPtr = opendir(levelDir);
            if (!levelDirPtr) {
                mkdir(levelDir, 0755);
            } else {
                closedir(levelDirPtr);
            }
        }
    } else {
        closedir(dir);
    }
}

void createSSTable(DB* db) {
    if(!folderStructureExists) {
        createFoldertructure();
        folderStructureExists = 1;
    }
        
        char filename[256];
        snprintf(filename, sizeof(filename), "%s/level1/sstable_%ld.dat", DIRNAME, time(NULL));
    
        FILE* sstable = fopen(filename, "wb");
        if (!sstable) {
            perror("fopen");
            return;
        }
    
        writeNodeToSSTable(db->root, sstable);

        fflush(sstable);
    
        fclose(sstable);
        printf("SSTable created: %s\n", filename);
}

void writeNodeToSSTable(Node* node, FILE* sstable) {
    if (!node) return;
    
    writeNodeToSSTable(node->left, sstable);
    size_t key_len = strlen(node->key);
    size_t val_len = strlen(node->value);
    fwrite(&key_len, sizeof(size_t), 1, sstable);
    fwrite(node->key, sizeof(char), key_len, sstable);
    fwrite(&val_len, sizeof(size_t), 1, sstable);
    fwrite(node->value, sizeof(char), val_len, sstable);
    writeNodeToSSTable(node->right, sstable);
}