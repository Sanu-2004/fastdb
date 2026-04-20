#include <sstable.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <limits.h>

indexList *createIndexList(int level)
{
    indexList *list = malloc(sizeof(struct indexList));
    list->key_offset = malloc((INDEX_SIZE + 1) * sizeof(long long));
    list->ptr = 0;
    list->level = level;
    list->next = NULL;
    return list;
}

void freeIndexList(indexList *list)
{
    if (list)
    {
        free(list->key_offset);
        freeIndexList(list->next);
        free(list);
    }
}

void writeIndexListToFile(indexList *list, FILE *file, bool forceWriteLastNode)
{
    if (list->ptr >= INDEX_SIZE || forceWriteLastNode)
    {
        list->key_offset[list->ptr++] = ftell(file);
        Footer *footer = malloc(sizeof(Footer));
        footer->isSparse = (list->level > 0);
        footer->indexSize = list->ptr;
        footer->index_offset = ftell(file);

        if (list->next == NULL && list->ptr >= INDEX_SIZE && !forceWriteLastNode)
        {
            list->next = createIndexList(list->level + 1);
        }

        fwrite(list->key_offset, sizeof(long long), list->ptr, file);
        fwrite(footer, sizeof(Footer), 1, file);
        free(footer);
        list->ptr = 0;
        if (list->next != NULL)
        {
            list->next->key_offset[list->next->ptr++] = list->key_offset[0];
            writeIndexListToFile(list->next, file, forceWriteLastNode);
        }
    }
}

void createFoldertructure()
{
    DIR *dir = opendir(DIRNAME);
    if (!dir)
    {
        mkdir(DIRNAME, 0755);
        for (int i = 1; i <= levels; i++)
        {
            char levelDir[256];
            snprintf(levelDir, sizeof(levelDir), "%s/level%d", DIRNAME, i);
            DIR *levelDirPtr = opendir(levelDir);
            if (!levelDirPtr)
            {
                mkdir(levelDir, 0755);
            }
            else
            {
                closedir(levelDirPtr);
            }
        }
    }
    else
    {
        closedir(dir);
    }
}

void createSSTable(DB *db)
{
    static int folderStructureExists = 0;
    static long counter = 0;
    long long i = 0;
    indexList *indexHead = createIndexList(0);
    if (!folderStructureExists)
    {
        createFoldertructure();
        folderStructureExists = 1;
    }

    char filename[256];
    snprintf(filename, sizeof(filename), "%s/level1/sstable_%ld_%ld.dat", DIRNAME, time(NULL), counter++);

    FILE *sstable = fopen(filename, "wb");
    if (!sstable)
    {
        free(indexHead->key_offset);
        free(indexHead);
        perror("fopen");
        return;
    }

    writeNodeToSSTable(db->root, sstable, &i, indexHead);
    writeIndexListToFile(indexHead, sstable, true);
    freeIndexList(indexHead);

    fflush(sstable);

    fclose(sstable);

    if (sstableCount(DIRNAME "/level1") > MAX_LEVEL_ENTRIES)
    {
        compactSSTables(1);
    }
}

void writeNodeToSSTable(Node *node, FILE *sstable, long long *line, indexList *indexHead)
{
    if (!node)
        return;

    writeNodeToSSTable(node->left, sstable, line, indexHead);
    size_t key_len = strlen(node->key);
    size_t val_len = strlen(node->value);
    indexHead->key_offset[indexHead->ptr++] = ftell(sstable);
    fwrite(&key_len, sizeof(size_t), 1, sstable);
    fwrite(node->key, sizeof(char), key_len, sstable);
    fwrite(&val_len, sizeof(size_t), 1, sstable);
    fwrite(node->value, sizeof(char), val_len, sstable);
    fflush(sstable);
    writeIndexListToFile(indexHead, sstable, false);
    writeNodeToSSTable(node->right, sstable, line, indexHead);
}

char *lookIntoSSTables(const char *key)
{
    for (int level = 1; level <= levels; level++)
    {
        char levelDir[256];
        snprintf(levelDir, sizeof(levelDir), "%s/level%d", DIRNAME, level);
        DIR *dir = opendir(levelDir);
        if (!dir)
            continue;

        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL)
        {
            if (strncmp(entry->d_name, "sstable_", 8) == 0)
            {
                char filepath[512];
                snprintf(filepath, sizeof(filepath), "%s/%s", levelDir, entry->d_name);
                FILE *sstable = fopen(filepath, "rb");
                if (!sstable)
                {
                    perror("fopen this");
                    continue;
                }
                Footer footer;
                fseek(sstable, -sizeof(Footer), SEEK_END);
                fread(&footer, sizeof(Footer), 1, sstable);

                long long *indexs = malloc((INDEX_SIZE + 1) * sizeof(long long));
                if (!indexs)
                {
                    fclose(sstable);
                    continue;
                }
                fseek(sstable, footer.index_offset, SEEK_SET);
                fread(indexs, sizeof(long long), footer.indexSize, sstable);

                // I am doing insexSize - 2 because last index is for current sstable, So last or rightmost index is size - 1 but excluding the last element it size - 2.
                char *row = searchValueFromSSTable(sstable, indexs, key, footer.isSparse, 0, footer.indexSize - 2);

                free(indexs);
                fclose(sstable);
                if (row)
                {
                    closedir(dir);
                    return row;
                }
            }
        }
        closedir(dir);
    }
    return NULL;
}

char *searchValueFromSSTable(FILE *sstable, long long *indexs, const char *key, bool isSparse, long long left, long long right)
{
    int cmp = 0;
    long long mid = 0;
    long long nearestHigh = 0;
    while (left <= right)
    {
        mid = (left + right) / 2;
        fseek(sstable, indexs[mid], SEEK_SET);
        size_t key_len = 0;
        fread(&key_len, sizeof(size_t), 1, sstable);
        char *mid_key = malloc(key_len + 1);
        fread(mid_key, sizeof(char), key_len, sstable);
        mid_key[key_len] = '\0';
        cmp = strcmp(key, mid_key);
        if (cmp == 0)
        {
            free(mid_key);
            size_t val_len;
            fread(&val_len, sizeof(size_t), 1, sstable);
            char *valueCopy = malloc(val_len + 1);
            fread(valueCopy, sizeof(char), val_len, sstable);
            valueCopy[val_len] = '\0';
            return valueCopy;
        }
        else if (cmp < 0)
        {
            right = mid - 1;
            nearestHigh = mid;
        }
        else
        {
            left = mid + 1;
            nearestHigh = mid + 1;
        }
        free(mid_key);
    }
    if (!isSparse) return NULL;

    // If it's sparse, we need to check the inner indexs.
    fseek(sstable, indexs[nearestHigh], SEEK_SET);
    Footer footer;
    fseek(sstable, -sizeof(Footer), SEEK_CUR);
    fread(&footer, sizeof(Footer), 1, sstable);
    fseek(sstable, footer.index_offset, SEEK_SET);
    fread(indexs, sizeof(long long), footer.indexSize, sstable);
    return searchValueFromSSTable(sstable, indexs, key, footer.isSparse, 0, footer.indexSize - 2);
}

int sstableCount(const char *dir)
{
    int count = 0;
    DIR *d = opendir(dir);
    if (!d)
        return 0;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL)
    {
        if (strncmp(entry->d_name, "sstable_", 8) == 0)
        {
            count++;
        }
    }
    closedir(d);
    return count;
}

void compactSSTables(int level)
{
    char levelDir[256];
    snprintf(levelDir, sizeof(levelDir), "%s/level%d", DIRNAME, level);
    DIR *dir = opendir(levelDir);
    if (!dir)
        return;
    char oldest1[512] = {0};
    char oldest2[512] = {0};
    time_t time1 = LONG_MAX;
    time_t time2 = LONG_MAX;
    long counter1 = LONG_MAX;
    long counter2 = LONG_MAX;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL)
    {
        if (strncmp(entry->d_name, "sstable_", 8) == 0)
        {

            long timestamp;
            long counter;

            if (sscanf(entry->d_name, "sstable_%ld_%ld.dat", &timestamp, &counter) != 2)
            {
                continue;
            }

            char fullpath[512];
            snprintf(fullpath, sizeof(fullpath), "%s/%s", levelDir, entry->d_name);

            if (timestamp < time1 || (timestamp == time1 && counter < counter1))
            {
                time2 = time1;
                counter2 = counter1;
                strcpy(oldest2, oldest1);

                // update first
                time1 = timestamp;
                counter1 = counter;
                strcpy(oldest1, fullpath);
            }
            else if (timestamp < time2 || (timestamp == time2 && counter < counter2))
            {
                time2 = timestamp;
                counter2 = counter;
                strcpy(oldest2, fullpath);
            }
        }
    }
    closedir(dir);
    if (oldest1[0] == '\0' || oldest2[0] == '\0')
        return;
    mergeSSTables(level + 1, oldest1, oldest2);
}

void mergeSSTables(int level, char *file1, char *file2)
{
    static long mergeCounter = 0;
    FILE *sstable1 = fopen(file1, "rb");
    FILE *sstable2 = fopen(file2, "rb");
    if (!sstable1 || !sstable2)
    {
        perror("fopen in merge");
        if (sstable1)
            fclose(sstable1);
        if (sstable2)
            fclose(sstable2);
        return;
    }
    char filename[256];
    snprintf(filename, sizeof(filename), "%s/level%d/sstable_%ld_%ld.dat", DIRNAME, level, time(NULL), mergeCounter++);
    FILE *merged = fopen(filename, "wb");
    if (!merged)
    {
        perror("fopen merged");
        fclose(sstable1);
        fclose(sstable2);
        return;
    }
    Footer footer1, footer2;
    fseek(sstable1, -sizeof(Footer), SEEK_END);
    fread(&footer1, sizeof(Footer), 1, sstable1);
    fseek(sstable1, 0, SEEK_SET);
    fseek(sstable2, -sizeof(Footer), SEEK_END);
    fread(&footer2, sizeof(Footer), 1, sstable2);
    fseek(sstable2, 0, SEEK_SET);
    // while(footer1.total_entries > 0 && footer2.total_entries > 0) {
    // }
}