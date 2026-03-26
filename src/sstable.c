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
    static int folderStructureExists = 0;
    static long counter = 0;
    long long i = 0;
    long long* offsets = malloc(1000 * sizeof(long long));
    if(!folderStructureExists) {
        createFoldertructure();
        folderStructureExists = 1;
    }
        
        char filename[256];
        snprintf(filename, sizeof(filename), "%s/level1/sstable_%ld_%ld.dat", DIRNAME, time(NULL), counter++);
    
        FILE* sstable = fopen(filename, "wb");
        if (!sstable) {
            free(offsets);
            perror("fopen");
            return;
        }

        writeNodeToSSTable(db->root, sstable, &i, offsets);
        long long index_offset = ftell(sstable);
        for(long long j = 0; j < i; j++) {
            fwrite(&offsets[j], sizeof(long long), 1, sstable);
        }
        Footer footer = { .index_offset = index_offset, .total_entries = i };
        fwrite(&footer, sizeof(Footer), 1, sstable);

        fflush(sstable);
    
        fclose(sstable);
        free(offsets);
}

void writeNodeToSSTable(Node* node, FILE* sstable, long long* line, long long* offsets) {
    if (!node) return;

    writeNodeToSSTable(node->left, sstable, line, offsets);
    size_t key_len = strlen(node->key);
    size_t val_len = strlen(node->value);
    offsets[(*line)++] = ftell(sstable);
    fwrite(&key_len, sizeof(size_t), 1, sstable);
    fwrite(node->key, sizeof(char), key_len, sstable);
    fwrite(&val_len, sizeof(size_t), 1, sstable);
    fwrite(node->value, sizeof(char), val_len, sstable);
    writeNodeToSSTable(node->right, sstable, line, offsets);
}

char* searchSSTables(const char* key) {
    for(int level = 1; level <= levels; level++) {
        char levelDir[256];
        snprintf(levelDir, sizeof(levelDir), "%s/level%d", DIRNAME, level);
        DIR* dir = opendir(levelDir);
        if (!dir) continue;

        struct dirent* entry;
        while ((entry = readdir(dir)) != NULL) {
            if (strncmp(entry->d_name, "sstable_", 8) == 0) {
                char filepath[512];
                snprintf(filepath, sizeof(filepath), "%s/%s", levelDir, entry->d_name);
                FILE* sstable = fopen(filepath, "rb");
                if (!sstable) {
                    perror("fopen this");                    
                    continue;
                }
                Footer footer;
                fseek(sstable, -sizeof(Footer), SEEK_END);
                fread(&footer, sizeof(Footer), 1, sstable);

                long long* indexs = malloc(footer.total_entries * sizeof(long long));
                if (!indexs) {
                    fclose(sstable);
                    continue;
                }
                fseek(sstable, footer.index_offset, SEEK_SET);
                fread(indexs, sizeof(long long), footer.total_entries, sstable);

                char* row = readValueFromSSTable(sstable, indexs, key, 0, footer.total_entries - 1);
                
                free(indexs);
                fclose(sstable);
                if (row) {
                    closedir(dir);
                    return row;
                }
            }
        }
        closedir(dir);
    }
    return NULL;
}

char* readValueFromSSTable(FILE* sstable, long long* indexs, const char* key, long long left, long long right) {
    while (left <= right)
    {
        long long mid = (left+right) / 2;
        fseek(sstable, indexs[mid], SEEK_SET);
        size_t key_len;
        fread(&key_len, sizeof(size_t), 1, sstable);
        char* mid_key = malloc(key_len + 1);
        fread(mid_key, sizeof(char), key_len, sstable);
        mid_key[key_len] = '\0';
        int cmp = strcmp(key, mid_key);
        if (cmp == 0) {
            free(mid_key);
            size_t val_len;
            fread(&val_len, sizeof(size_t), 1, sstable);
            char* valueCopy = malloc(val_len + 1);
            fread(valueCopy, sizeof(char), val_len, sstable);
            valueCopy[val_len] = '\0';
            return valueCopy;
        } else if (cmp < 0) {
            right = mid - 1;
        } else {
            left = mid + 1;
        }
        free(mid_key);
    }
    return NULL;
} 