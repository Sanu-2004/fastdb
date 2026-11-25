#include "sstable.h"

#include <assert.h>
#include <dirent.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TABLE_0 "tables/level1/sstable_1700000000_0.dat"
#define TABLE_1 "tables/level1/sstable_1700000000_1.dat"

static bool order_tables;
static size_t table_index;
static struct dirent ordered_tables[2];

struct dirent *__real_readdir(DIR *dir);

struct dirent *__wrap_readdir(DIR *dir)
{
    if (!order_tables)
        return __real_readdir(dir);
    return table_index < 2 ? &ordered_tables[table_index++] : NULL;
}

time_t __wrap_time(time_t *result)
{
    const time_t fixed = 1700000000;
    if (result != NULL)
        *result = fixed;
    return fixed;
}

static void write_text(const char *path, const char *contents)
{
    FILE *file = fopen(path, "w");
    assert(file != NULL);
    assert(fputs(contents, file) >= 0);
    assert(fclose(file) == 0);
}

static void close_db(DB *db)
{
    assert(fclose(db->wal) == 0);
    FreeTree(db->root);
    free(db);
}

static void table(char *key, char *value)
{
    DB db = {0};
    insertTree(&db.root, key, value);
    createSSTable(&db);
    FreeTree(db.root);
}

static void failed_flush(void)
{
    write_text("tables", "blocks creation of SSTable directory");
    write_text("wal_1.log", "SET kept original\n");
    DB *db = createEmptyDB();
    assert(getValue(db->root, "kept") != NULL);
    char payload[MAX_WAL_SIZE + 1];
    memset(payload, 'x', sizeof(payload) - 1);
    payload[sizeof(payload) - 1] = 0;
    insert(db, "trigger", payload);
    assert(db->root == NULL);
    assert(access("wal_1.log", F_OK) == -1);
    close_db(db);
    db = createEmptyDB();
    assert(getValue(db->root, "kept") == NULL);
    assert(getValue(db->root, "trigger") == NULL);
    close_db(db);
    puts("failed-flush: both writes lost and original WAL removed");
}

static void multi_wal(void)
{
    write_text("wal_1.log", "SET older kept\nSET shared old\n");
    write_text("wal_2.log", "SET shared new\n");
    DB *db = createEmptyDB();
    assert(get(db, "older") == NULL);
    assert(strcmp(get(db, "shared"), "new") == 0);
    close_db(db);
    puts("multi-wal: key found only in first WAL is lost");
}

static void accounting(void)
{
    DB *db = createEmptyDB();
    for (int i = 0; i < 100; ++i)
        insert(db, "k", "v");
    assert(db->size == 200 && !db->root->left && !db->root->right);
    close_db(db);
    db = createEmptyDB();
    assert(db->size == 200 && !db->root->left && !db->root->right);
    close_db(db);
    puts("accounting: 100 overwrites count as 200 payload bytes for one 2-byte entry");
}

static void ownership(void)
{
    DB *db = createEmptyDB();
    insert(db, "key", "value");
    createSSTable(db);
    char *in_memory = get(db, "key");
    assert(in_memory == db->root->value);
    char *on_disk = lookIntoSSTables("key");
    assert(on_disk != NULL && on_disk != in_memory && strcmp(on_disk, in_memory) == 0);
    free(on_disk);
    close_db(db);
    puts("ownership: GET borrows the memtable value and allocates the disk value");
}

static void merge(void)
{
    table("a", "first");
    table("b", "second");
    compactSSTables(1);
    struct stat info;
    assert(stat("tables/level2/sstable_1700000000_0.dat", &info) == 0);
    assert(info.st_size == 0);
    assert(access(TABLE_0, F_OK) == 0 && access(TABLE_1, F_OK) == 0);
    puts("merge: published a zero-byte output and left both inputs");
}

static void wal_id(void)
{
    FILE *first = createWalFile();
    FILE *second = createWalFile();
    assert(first != NULL && second != NULL);
    struct stat a, b;
    assert(fstat(fileno(first), &a) == 0 && fstat(fileno(second), &b) == 0);
    assert(a.st_dev == b.st_dev && a.st_ino == b.st_ino);
    assert(fclose(first) == 0 && fclose(second) == 0);
    puts("wal-id: two same-second creations open one file");
}

static void child_table(char *value)
{
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        table("key", value);
        _exit(0);
    }
    int status;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void table_id(void)
{
    child_table("original");
    child_table("replacement");
    assert(sstableCount("tables/level1") == 1);
    char *value = lookIntoSSTables("key");
    assert(value != NULL && strcmp(value, "replacement") == 0);
    free(value);
    puts("table-id: a fresh process in the same second overwrites the original table");
}

static void stale(void)
{
    table("key", "old");
    table("key", "new");
    DIR *dir = opendir("tables/level1");
    assert(dir != NULL);
    const struct dirent *entry;
    size_t seen = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, "sstable_1700000000_0.dat") == 0) {
            strcpy(ordered_tables[0].d_name, entry->d_name);
            ++seen;
        }
        if (strcmp(entry->d_name, "sstable_1700000000_1.dat") == 0) {
            strcpy(ordered_tables[1].d_name, entry->d_name);
            ++seen;
        }
    }
    closedir(dir);
    assert(seen == 2);
    /* Exercise the old-first order that readdir is allowed to return. */
    order_tables = true;
    char *value = lookIntoSSTables("key");
    order_tables = false;
    assert(value != NULL && strcmp(value, "old") == 0);
    free(value);
    puts("stale: lookup returns the first enumerated version despite a newer table");
}

static void index_boundary(void)
{
    DB db = {0};
    insertTree(&db.root, "a", "1");
    insertTree(&db.root, "b", "2");
    createSSTable(&db);
    FreeTree(db.root);
    FILE *file = fopen(TABLE_0, "rb");
    Footer root;
    long long offsets[INDEX_SIZE + 1];
    assert(file != NULL);
    assert(fseek(file, -(long)sizeof(root), SEEK_END) == 0);
    assert(fread(&root, sizeof(root), 1, file) == 1);
    assert(root.isSparse && root.indexSize == 3);
    assert(fseek(file, (long)root.index_offset, SEEK_SET) == 0);
    assert(fread(offsets, sizeof(offsets[0]), 3, file) == 3);
    Footer empty_child;
    assert(fseek(file, (long)root.index_offset - (long)sizeof(empty_child), SEEK_SET) == 0);
    assert(fread(&empty_child, sizeof(empty_child), 1, file) == 1);
    assert(!empty_child.isSparse && empty_child.indexSize == 1);
    assert(offsets[1] == empty_child.index_offset && offsets[2] == root.index_offset);
    assert(fclose(file) == 0);
    puts("index: exact dense-block boundary publishes a sparse record offset into index metadata");
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    struct {
        const char *name;
        void (*run)(void);
    } probes[] = {
        {"failed-flush", failed_flush}, {"multi-wal", multi_wal},
        {"accounting", accounting}, {"ownership", ownership}, {"merge", merge},
        {"wal-id", wal_id}, {"table-id", table_id}, {"stale", stale}, {"index", index_boundary},
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) {
        if (strcmp(argv[1], probes[i].name) == 0) {
            probes[i].run();
            return 0;
        }
    }
    return 1;
}
