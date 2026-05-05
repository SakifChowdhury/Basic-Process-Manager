#include "process_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

static int is_pid_dir(const char *name) {
    if (!name || *name == '\0') return 0;
    for (const char *c = name; *c; c++)
        if (!isdigit((unsigned char)*c)) return 0;
    return 1;
}

int pt_init(ProcessTable *pt) {
    if (!pt) return -1;
    memset(pt, 0, sizeof(ProcessTable));
    if (pthread_mutex_init(&pt->lock, NULL) != 0) {
        perror("pthread_mutex_init");
        return -1;
    }
    return 0;
}

void pt_destroy(ProcessTable *pt) {
    if (pt) pthread_mutex_destroy(&pt->lock);
}

unsigned long read_total_cpu_ticks(void) {
    FILE *fp = fopen("/proc/stat", "r");
    if (!fp) return 0;
    unsigned long user, nice, system, idle, iowait, irq, softirq, steal;
    char label[16];
    if (fscanf(fp, "%15s %lu %lu %lu %lu %lu %lu %lu %lu",
               label, &user, &nice, &system, &idle,
               &iowait, &irq, &softirq, &steal) < 9) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    return user + nice + system + idle + iowait + irq + softirq + steal;
}

int read_proc_stat(pid_t pid, ProcessEntry *entry, unsigned long total_cpu_delta) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    char raw[1024];
    if (!fgets(raw, sizeof(raw), fp)) { fclose(fp); return -1; }
    fclose(fp);

    char *comm_start = strchr(raw, '(');
    char *comm_end   = strrchr(raw, ')');
    if (!comm_start || !comm_end || comm_end <= comm_start) return -1;

    int name_len = (int)(comm_end - comm_start - 1);
    if (name_len >= MAX_NAME_LEN) name_len = MAX_NAME_LEN - 1;
    strncpy(entry->name, comm_start + 1, name_len);
    entry->name[name_len] = '\0';

    char state;
    int  ppid;
    unsigned long utime, stime;
    long cutime, cstime, priority, nice, num_threads, itrealvalue;
    unsigned long long starttime;

    if (sscanf(comm_end + 2,
               "%c %d %*d %*d %*d %*d %*u "
               "%*u %*u %*u %*u "
               "%lu %lu %ld %ld "
               "%ld %ld %ld %ld "
               "%llu",
               &state, &ppid,
               &utime, &stime, &cutime, &cstime,
               &priority, &nice, &num_threads, &itrealvalue,
               &starttime) < 11) return -1;

    entry->pid        = pid;
    entry->ppid       = (pid_t)ppid;
    entry->state_char = state;
    entry->priority   = (int)priority;
    entry->nice       = (int)nice;
    entry->num_threads= num_threads;

    unsigned long proc_now  = utime + stime + (unsigned long)cutime
                                             + (unsigned long)cstime;
    unsigned long proc_prev = entry->prev_snapshot.utime
                            + entry->prev_snapshot.stime
                            + entry->prev_snapshot.cutime
                            + entry->prev_snapshot.cstime;

    if (total_cpu_delta > 0 && proc_prev > 0) {
        long ncpus = sysconf(_SC_NPROCESSORS_ONLN);
        if (ncpus < 1) ncpus = 1;
        entry->cpu_percent =
            ((double)(proc_now - proc_prev) / (double)total_cpu_delta)
            * 100.0 * (double)ncpus;
        if (entry->cpu_percent < 0.0) entry->cpu_percent = 0.0;
    } else {
        entry->cpu_percent = 0.0;
    }

    entry->prev_snapshot.utime  = utime;
    entry->prev_snapshot.stime  = stime;
    entry->prev_snapshot.cutime = (unsigned long)cutime;
    entry->prev_snapshot.cstime = (unsigned long)cstime;

    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    struct timespec boot_ts;
    clock_gettime(CLOCK_BOOTTIME, &boot_ts);
    entry->start_time = time(NULL) - boot_ts.tv_sec + (long)(starttime / hz);

    strncpy(entry->state_str, state_to_string(state), MAX_STATE_LEN - 1);
    entry->state_str[MAX_STATE_LEN - 1] = '\0';
    return 0;
}

int read_proc_status(pid_t pid, ProcessEntry *entry) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;

    char line[256];
    uid_t uid = 0; int got_uid = 0;

    while (fgets(line, sizeof(line), fp)) {
        if      (strncmp(line, "VmRSS:",  6) == 0) sscanf(line + 6,  " %ld", &entry->rss_kb);
        else if (strncmp(line, "VmSize:", 7) == 0) sscanf(line + 7,  " %ld", &entry->vsize_kb);
        else if (strncmp(line, "Uid:",    4) == 0) {
            unsigned int ruid;
            if (sscanf(line + 4, " %u", &ruid) == 1) { uid = ruid; got_uid = 1; }
        }
    }
    fclose(fp);

    if (got_uid) {
        struct passwd *pw = getpwuid(uid);
        if (pw) strncpy(entry->user, pw->pw_name, MAX_USER_LEN - 1);
        else    snprintf(entry->user, MAX_USER_LEN, "%u", uid);
    } else {
        strncpy(entry->user, "unknown", MAX_USER_LEN - 1);
    }
    return 0;
}

int pt_refresh(ProcessTable *pt) {
    if (!pt) return -1;

    unsigned long cpu_before = read_total_cpu_ticks();

    ProcessEntry new_entries[MAX_PROCESSES];
    int new_count = 0;

    DIR *proc_dir = opendir(PROC_PATH);
    if (!proc_dir) { perror("opendir /proc"); return -1; }

    struct dirent *de;
    while ((de = readdir(proc_dir)) != NULL && new_count < MAX_PROCESSES) {
        if (!is_pid_dir(de->d_name)) continue;
        pid_t pid = (pid_t)atoi(de->d_name);

        ProcessEntry *e = &new_entries[new_count];
        memset(e, 0, sizeof(ProcessEntry));

        pthread_mutex_lock(&pt->lock);
        ProcessEntry *prev = pt_find_by_pid(pt, pid);
        if (prev) e->prev_snapshot = prev->prev_snapshot;
        unsigned long cpu_delta = (pt->total_cpu_curr > pt->total_cpu_prev)
                                  ? pt->total_cpu_curr - pt->total_cpu_prev : 0;
        pthread_mutex_unlock(&pt->lock);

        if (read_proc_stat(pid, e, cpu_delta) != 0) continue;
        if (read_proc_status(pid, e) != 0)
            strncpy(e->user, "?", MAX_USER_LEN - 1);

        e->valid = 1;
        new_count++;
    }
    closedir(proc_dir);

    unsigned long cpu_after = read_total_cpu_ticks();

    pthread_mutex_lock(&pt->lock);
    pt->total_cpu_prev = cpu_before;
    pt->total_cpu_curr = cpu_after;
    pt->count          = new_count;
    memcpy(pt->entries, new_entries, sizeof(ProcessEntry) * new_count);
    pt->last_updated   = time(NULL);
    pthread_mutex_unlock(&pt->lock);

    return new_count;
}

ProcessEntry *pt_find_by_pid(ProcessTable *pt, pid_t pid) {
    for (int i = 0; i < pt->count; i++)
        if (pt->entries[i].valid && pt->entries[i].pid == pid)
            return &pt->entries[i];
    return NULL;
}

static int cmp_pid     (const void *a, const void *b) { return ((ProcessEntry*)a)->pid      - ((ProcessEntry*)b)->pid; }
static int cmp_cpu     (const void *a, const void *b) { double d = ((ProcessEntry*)b)->cpu_percent - ((ProcessEntry*)a)->cpu_percent; return (d>0)-(d<0); }
static int cmp_mem     (const void *a, const void *b) { long   d = ((ProcessEntry*)b)->rss_kb      - ((ProcessEntry*)a)->rss_kb;      return (d>0)-(d<0); }
static int cmp_name    (const void *a, const void *b) { return strcasecmp(((ProcessEntry*)a)->name, ((ProcessEntry*)b)->name); }
static int cmp_priority(const void *a, const void *b) { return ((ProcessEntry*)a)->priority - ((ProcessEntry*)b)->priority; }

void pt_sort(ProcessTable *pt, SortOrder order) {
    if (!pt || pt->count == 0) return;
    switch (order) {
        case SORT_BY_PID:      qsort(pt->entries, pt->count, sizeof(ProcessEntry), cmp_pid);      break;
        case SORT_BY_CPU:      qsort(pt->entries, pt->count, sizeof(ProcessEntry), cmp_cpu);      break;
        case SORT_BY_MEM:      qsort(pt->entries, pt->count, sizeof(ProcessEntry), cmp_mem);      break;
        case SORT_BY_NAME:     qsort(pt->entries, pt->count, sizeof(ProcessEntry), cmp_name);     break;
        case SORT_BY_PRIORITY: qsort(pt->entries, pt->count, sizeof(ProcessEntry), cmp_priority); break;
    }
}

const char *state_to_string(char sc) {
    switch (sc) {
        case 'R': return "Running";
        case 'S': return "Sleeping";
        case 'D': return "Disk Wait";
        case 'Z': return "Zombie";
        case 'T': return "Stopped";
        case 't': return "Tracing";
        case 'W': return "Paging";
        case 'X': return "Dead";
        case 'I': return "Idle";
        default:  return "Unknown";
    }
}

void format_memory(long kb, char *buf, int buf_size) {
    if      (kb < 0)       snprintf(buf, buf_size, "   0 KB");
    else if (kb < 1024)    snprintf(buf, buf_size, "%4ld KB", kb);
    else if (kb < 1048576) snprintf(buf, buf_size, "%4.1f MB", kb / 1024.0);
    else                   snprintf(buf, buf_size, "%4.2f GB", kb / 1048576.0);
}

void format_uptime(time_t seconds, char *buf, int buf_size) {
    if (seconds < 0) seconds = 0;
    long d = seconds / 86400, h = (seconds % 86400) / 3600;
    long m = (seconds % 3600) / 60, s = seconds % 60;
    if (d > 0) snprintf(buf, buf_size, "%ldd %02ldh%02ldm", d, h, m);
    else       snprintf(buf, buf_size, "%02ld:%02ld:%02ld", h, m, s);
}
