#include <db.h>
#include <dirent.h>
#include <string.h>
#include <time.h>
#include <sstable.h>

DB *createEmptyDB()
{
    DB *db = (DB *)malloc(sizeof(DB));
    db->root = NULL;
    db->size = 0;
    FILE *recentWal = getRecentWalFile(WALL_DIR);
    
    if (!recentWal) recentWal = createWalFile();
    else restoreTree(db, recentWal);

    db->wal = recentWal;
    return db;
}

void insert(DB *db, char *key, char *val)
{
    db->size += (int)(strlen(key) + strlen(val));
    fprintf(db->wal, "SET %s %s\n", key, val);
    fflush(db->wal);
    insertTree(&(db->root), key, val);
    if (db->size >= MAX_WAL_SIZE)
    {
        createSSTable(db);
        fclose(db->wal);
        clearWALFiles(WALL_DIR);
        FreeTree(db->root);
        db->root = NULL;
        db->size = 0;
        db->wal = createWalFile();
    }
}

FILE *getRecentWalFile(const char *dir)
{
    DIR *dp = opendir(dir);
    if (!dp)
        return NULL;

    struct dirent *entry;
    char *latest = NULL;
    long max_ts = 0;

    while ((entry = readdir(dp)) != NULL)
    {
        if (strncmp(entry->d_name, "wal_", 4) != 0)
            continue;
        size_t len = strlen(entry->d_name);
        if (len < 8)
            continue;
        if (strcmp(entry->d_name + len - 4, ".log") != 0)
            continue;

        // extract timestamp
        char tsbuf[32];
        strncpy(tsbuf, entry->d_name + 4, len - 8);
        tsbuf[len - 8] = '\0';

        long ts = atol(tsbuf);
        if (ts > max_ts)
        {
            max_ts = ts;
            free(latest);
            latest = strdup(entry->d_name);
        }
    }
    closedir(dp);

    if (!latest)
        return NULL;

    size_t pathLen = strlen(dir) + strlen(latest) + 2;
    char *path = (char *)malloc(pathLen);
    if (!path)
    {
        free(latest);
        return NULL;
    }
    snprintf(path, pathLen, "%s/%s", dir, latest);

    FILE *wal = fopen(path, "a+");
    free(path);
    free(latest);
    if (!wal)
    {
        perror("fopen");
        return NULL;
    }

    rewind(wal);
    return wal;
}

FILE *createWalFile()
{
    time_t now = time(NULL);
    if (now == ((time_t)-1))
    {
        perror("time");
        return NULL;
    }

    char filename[64];

    sprintf(filename, "wal_%ld.log", now);

    FILE *wal = fopen(filename, "a+");
    if (!wal)
    {
        perror("fopen");
        return NULL;
    }

    return wal;
}

void restoreTree(DB *db, FILE *wal)
{
    char *buffer = NULL;
    size_t len = 0;

    rewind(wal);
    while (getline(&buffer, &len, wal) != -1)
    {
        char *saveptr = NULL;
        char *cmd = strtok_r(buffer, " \n", &saveptr);
        if (!cmd)
            continue;

        if (strcmp(cmd, "SET") == 0)
        {
            char *key = strtok_r(NULL, " ", &saveptr);
            char *val = strtok_r(NULL, "\n", &saveptr);

            if (!key || !val)
                continue;

            db->size += (int)(strlen(key) + strlen(val));
            insertTree(&(db->root), key, val);
        }
    }
    free(buffer);
}

void clearWALFiles(const char* dir) {
    DIR* dp = opendir(dir);
    if (!dp) return;

    struct dirent* entry;
    while ((entry = readdir(dp)) != NULL) {
        if (strncmp(entry->d_name, "wal_", 4) == 0) {
            size_t len = strlen(dir) + strlen(entry->d_name) + 2;
            char *filepath = malloc(len);
            if (!filepath) continue;
            snprintf(filepath, len, "%s/%s", dir, entry->d_name);
            remove(filepath);
            free(filepath);
        }
    }
    closedir(dp);
}

char* get(DB* db, char* key) {
    char* val = getValue(db->root, key);
    if(val != NULL) {
        return val;
    }

    return lookIntoSSTables(key);
}