#include<db.h>
#include<dirent.h>
#include<string.h>
#include <time.h>

DB* createEmptyDB(){
    DB* db = (DB*)malloc(sizeof(DB));
    db->root = NULL;
    FILE* recentWal = getRecentWalFile(".");
    if(!recentWal){
        recentWal = createWalFile();
    } else {
        restoreTree(db, recentWal);
    }
    db->wal = recentWal;
    return db;
}

void insert(DB* db, char* key, char* val) {
    fprintf(db->wal, "SET %s %s\n", key, val);
    fflush(db->wal);
    insertTree(&(db->root), key, val);
}

FILE* getRecentWalFile(const char* dir) {
    DIR* dp = opendir(dir);
    if(!dp) return NULL;

    struct dirent* entry;
    char* latest = NULL;
    long max_ts = 0;

    while((entry = readdir(dp)) != NULL) {
        if(strncmp(entry->d_name, "wal_", 4) != 0) continue;
        size_t len = strlen(entry->d_name);
        if(len < 8) continue;
        if(strcmp(entry->d_name + len - 4, ".log") != 0) continue;

        // extract timestamp
        char tsbuf[32];
        strncpy(tsbuf, entry->d_name + 4, len - 8);
        tsbuf[len - 8] = '\0';

        long ts = atol(tsbuf);
        if(ts > max_ts) {
            max_ts = ts;
            free(latest);
            latest = strdup(entry->d_name);
        }
    }
    closedir(dp);
    
    FILE* wal = fopen(latest, "a+");
    if (!wal) {
        perror("fopen");
        return NULL;
    }
    return wal;
}

FILE* createWalFile() {
    time_t now = time(NULL); 
    if (now == ((time_t)-1)) {
        perror("time");
        return NULL;
    }

    char filename[64];

    sprintf(filename, "wal_%ld.log", now);

    FILE* wal = fopen(filename, "a+");
    if (!wal) {
        perror("fopen");
        return NULL;
    }

    printf("Created WAL file: %s\n", filename);
    return wal;
}

void restoreTree(DB* db, FILE* wal){
    char* buffer = NULL;
    size_t len = 0;
    char* saveptr;
    while(getline(&buffer, &len, wal) != -1){
        char* cmd = strtok_r(buffer, " \n", &saveptr);
        if(strcmp(cmd, "SET") == 0){
            char* key = strtok_r(NULL, " ", &saveptr);
            char* val = strtok_r(NULL, "\n", &saveptr);
            insertTree(&(db->root), key, val);
        }
    }
    free(buffer);
}