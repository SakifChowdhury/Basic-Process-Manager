#ifndef PROCESS_TABLE_H
#define PROCESS_TABLE_H

#include <pthread.h>
#include <sys/types.h>
#include <time.h>

#define MAX_PROCESSES    1024
#define MAX_NAME_LEN     256
#define MAX_USER_LEN     64
#define MAX_STATE_LEN    16
#define PROC_PATH        "/proc"
#define REFRESH_INTERVAL 2

typedef enum {
    STATE_RUNNING  = 'R',
    STATE_SLEEPING = 'S',
    STATE_DISK     = 'D',
    STATE_ZOMBIE   = 'Z',
    STATE_STOPPED  = 'T',
    STATE_TRACING  = 't',
    STATE_PAGING   = 'W',
    STATE_DEAD     = 'X',
    STATE_IDLE     = 'I',
    STATE_UNKNOWN  = '?'
} ProcessState;

typedef struct {
    unsigned long utime;
    unsigned long stime;
    unsigned long cutime;
    unsigned long cstime;
    unsigned long total_cpu;
} CpuSnapshot;

typedef struct {
    pid_t         pid;
    pid_t         ppid;
    char          name[MAX_NAME_LEN];
    char          user[MAX_USER_LEN];
    char          state_char;
    char          state_str[MAX_STATE_LEN];
    int           priority;
    int           nice;
    long          num_threads;

    long          vsize_kb;
    long          rss_kb;

    double        cpu_percent;
    CpuSnapshot   prev_snapshot;

    time_t        start_time;

    int           valid;
} ProcessEntry;

typedef struct {
    ProcessEntry  entries[MAX_PROCESSES];
    int           count;
    pthread_mutex_t lock;
    time_t        last_updated;
    unsigned long total_cpu_prev;
    unsigned long total_cpu_curr;
} ProcessTable;

typedef enum {
    SORT_BY_PID     = 0,
    SORT_BY_CPU     = 1,
    SORT_BY_MEM     = 2,
    SORT_BY_NAME    = 3,
    SORT_BY_PRIORITY= 4
} SortOrder;

int  pt_init(ProcessTable *pt);
void pt_destroy(ProcessTable *pt);

int  pt_refresh(ProcessTable *pt);

ProcessEntry *pt_find_by_pid(ProcessTable *pt, pid_t pid);

void pt_sort(ProcessTable *pt, SortOrder order);

const char *state_to_string(char state_char);
void        format_memory(long kb, char *buf, int buf_size);
void        format_uptime(time_t seconds, char *buf, int buf_size);

int  read_proc_stat(pid_t pid, ProcessEntry *entry, unsigned long total_cpu_delta);
int  read_proc_status(pid_t pid, ProcessEntry *entry);
unsigned long read_total_cpu_ticks(void);

#endif
