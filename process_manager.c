#include "process_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <pthread.h>
#include <errno.h>
#include <ctype.h>

#define COL_RESET   "\033[0m"
#define COL_BOLD    "\033[1m"
#define COL_RED     "\033[31m"
#define COL_GREEN   "\033[32m"
#define COL_YELLOW  "\033[33m"
#define COL_CYAN    "\033[36m"
#define COL_MAGENTA "\033[35m"
#define COL_BLUE    "\033[34m"
#define COL_WHITE   "\033[37m"
#define COL_BG_BLUE "\033[44m"

static ProcessTable g_table;
static volatile int g_running   = 1;
static volatile int g_dirty     = 1;
static SortOrder    g_sort      = SORT_BY_CPU;
static pid_t        g_demo_child = -1;

static void *refresh_thread_func(void *arg) {
    (void)arg;
    while (g_running) {
        pt_refresh(&g_table);
        g_dirty = 1;
        sleep(REFRESH_INTERVAL);
    }
    return NULL;
}

static void clear_screen(void)  { printf("\033[2J\033[H"); fflush(stdout); }

static void print_banner(void) {
    printf(COL_BG_BLUE COL_BOLD
           "  ╔══════════════════════════════════════════════════════════════╗  \n"
           "  ║          BASIC PROCESS MANAGER  –  OS Design Project        ║  \n"
           "  ╚══════════════════════════════════════════════════════════════╝  "
           COL_RESET "\n");
}

static void print_sort_legend(void) {
    const char *labels[] = {"PID","CPU","MEM","NAME","PRIO"};
    printf(COL_CYAN "  Sort: " COL_RESET);
    for (int i = 0; i < 5; i++) {
        if ((SortOrder)i == g_sort)
            printf(COL_YELLOW COL_BOLD "[%s]" COL_RESET " ", labels[i]);
        else
            printf(COL_WHITE  "%s" COL_RESET " ", labels[i]);
    }
    printf("\n");
}

static void print_column_header(void) {
    printf(COL_BOLD COL_CYAN
           "  %-7s %-20s %-10s %-9s %6s %9s %9s %5s %5s\n"
           COL_RESET,
           "PID","NAME","USER","STATE","CPU%","RSS","VIRT","PRI","NI");
    printf(COL_CYAN
           "  %s\n" COL_RESET,
           "─────────────────────────────────────────────────────────────────────────");
}

static const char *state_colour(char sc) {
    switch (sc) {
        case 'R': return COL_GREEN;
        case 'Z': return COL_RED;
        case 'T': return COL_MAGENTA;
        case 'D': return COL_YELLOW;
        default:  return COL_WHITE;
    }
}

static void print_process_list(int max_rows) {
    char rss_buf[16], virt_buf[16];
    int shown = 0;

    pthread_mutex_lock(&g_table.lock);
    pt_sort(&g_table, g_sort);

    for (int i = 0; i < g_table.count && shown < max_rows; i++) {
        ProcessEntry *e = &g_table.entries[i];
        if (!e->valid) continue;

        format_memory(e->rss_kb,   rss_buf,  sizeof(rss_buf));
        format_memory(e->vsize_kb, virt_buf, sizeof(virt_buf));

        const char *row_col = COL_RESET;

        printf("  " COL_BOLD "%s%-7d" COL_RESET
               " %-20.20s %-10.10s "
               "%s%-9.9s" COL_RESET
               " %6.1f %9s %9s %5d %5d\n",
               row_col, e->pid,
               e->name, e->user,
               state_colour(e->state_char), e->state_str,
               e->cpu_percent,
               rss_buf, virt_buf,
               e->priority, e->nice);
        shown++;
    }

    int total = g_table.count;
    time_t upd = g_table.last_updated;
    pthread_mutex_unlock(&g_table.lock);

    if (shown < total)
        printf(COL_CYAN "  ... %d more processes (scroll not shown)\n" COL_RESET,
               total - shown);
    printf(COL_CYAN "  Total: %d processes  |  Last updated: %s" COL_RESET,
           total, ctime(&upd));
}

static void print_tree_node(pid_t pid, int depth, int *printed, int max) {
    pthread_mutex_lock(&g_table.lock);
    ProcessEntry *e = pt_find_by_pid(&g_table, pid);
    if (!e) { pthread_mutex_unlock(&g_table.lock); return; }

    char rss[16];
    format_memory(e->rss_kb, rss, sizeof(rss));
    char name_copy[MAX_NAME_LEN]; strncpy(name_copy, e->name, MAX_NAME_LEN);
    char sc = e->state_char;
    int nice = e->nice;
    pthread_mutex_unlock(&g_table.lock);

    for (int i = 0; i < depth; i++) printf(i < depth-1 ? "  │ " : "  ├─");
    printf(COL_CYAN "%d" COL_RESET " %s%s" COL_RESET
           " [%s%c" COL_RESET "] ni=%d rss=%s\n",
           pid,
           state_colour(sc), name_copy,
           state_colour(sc), sc,
           nice, rss);
    (*printed)++;

    pthread_mutex_lock(&g_table.lock);
    pid_t children[MAX_PROCESSES]; int nc = 0;
    for (int i = 0; i < g_table.count && nc < MAX_PROCESSES; i++)
        if (g_table.entries[i].valid && g_table.entries[i].ppid == pid
                && g_table.entries[i].pid != pid)
            children[nc++] = g_table.entries[i].pid;
    pthread_mutex_unlock(&g_table.lock);

    for (int i = 0; i < nc && *printed < max; i++)
        print_tree_node(children[i], depth + 1, printed, max);
}

static void show_tree(void) {
    clear_screen();
    print_banner();
    printf(COL_YELLOW COL_BOLD "\n  PROCESS TREE\n\n" COL_RESET);
    int printed = 0;
    print_tree_node(1, 0, &printed, 120);
    printf(COL_CYAN "\n  Showing %d processes in tree.\n" COL_RESET, printed);
    printf(COL_YELLOW "\n  Press ENTER to return..." COL_RESET);
    getchar();
}

static void send_signal_to_pid(pid_t pid, int sig, const char *sig_name) {
    errno = 0;
    if (kill(pid, sig) == 0) {
        printf(COL_GREEN "  ✓ Signal %s sent to PID %d successfully.\n"
               COL_RESET, sig_name, pid);
        sleep(1);
        pt_refresh(&g_table);
    } else {
        printf(COL_RED "  ✗ Failed to send %s to PID %d: %s\n" COL_RESET,
               sig_name, pid, strerror(errno));
    }
}

static void menu_kill_process(void) {
    pid_t pid;
    printf(COL_YELLOW "\n  Enter PID to signal: " COL_RESET);
    if (scanf("%d", &pid) != 1) { while(getchar()!='\n'); return; }
    while(getchar()!='\n');

    pthread_mutex_lock(&g_table.lock);
    ProcessEntry *e = pt_find_by_pid(&g_table, pid);
    int found = (e != NULL);
    if (found)
        printf(COL_CYAN "  Process: %s (State: %s)\n" COL_RESET,
               e->name, e->state_str);
    pthread_mutex_unlock(&g_table.lock);

    if (!found) { printf(COL_RED "  PID %d not found.\n" COL_RESET, pid); return; }

    printf("\n"
           COL_YELLOW "  [1]" COL_RESET " SIGTERM  – Graceful termination request\n"
           COL_YELLOW "  [2]" COL_RESET " SIGKILL  – Immediate forced termination\n"
           COL_YELLOW "  [3]" COL_RESET " SIGSTOP  – Pause / suspend process\n"
           COL_YELLOW "  [4]" COL_RESET " SIGCONT  – Resume a stopped process\n"
           COL_YELLOW "  [0]" COL_RESET " Cancel\n"
           COL_YELLOW "  Choice: " COL_RESET);

    int choice;
    if (scanf("%d", &choice) != 1) { while(getchar()!='\n'); return; }
    while(getchar()!='\n');

    switch (choice) {
        case 1: send_signal_to_pid(pid, SIGTERM, "SIGTERM"); break;
        case 2: send_signal_to_pid(pid, SIGKILL, "SIGKILL"); break;
        case 3: send_signal_to_pid(pid, SIGSTOP, "SIGSTOP"); break;
        case 4: send_signal_to_pid(pid, SIGCONT, "SIGCONT"); break;
        case 0: printf("  Cancelled.\n"); break;
        default: printf(COL_RED "  Invalid choice.\n" COL_RESET);
    }
}

static void menu_renice(void) {
    pid_t pid;
    int   new_nice;
    printf(COL_YELLOW "\n  Enter PID to renice: " COL_RESET);
    if (scanf("%d", &pid) != 1) { while(getchar()!='\n'); return; }
    while(getchar()!='\n');

    pthread_mutex_lock(&g_table.lock);
    ProcessEntry *e = pt_find_by_pid(&g_table, pid);
    if (!e) {
        pthread_mutex_unlock(&g_table.lock);
        printf(COL_RED "  PID %d not found.\n" COL_RESET, pid);
        return;
    }
    printf(COL_CYAN "  Process: %s  Current nice: %d\n" COL_RESET,
           e->name, e->nice);
    pthread_mutex_unlock(&g_table.lock);

    printf(COL_YELLOW "  New nice value (-20 to 19): " COL_RESET);
    if (scanf("%d", &new_nice) != 1) { while(getchar()!='\n'); return; }
    while(getchar()!='\n');

    if (new_nice < -20 || new_nice > 19) {
        printf(COL_RED "  Nice value out of range.\n" COL_RESET);
        return;
    }

    errno = 0;
    if (setpriority(PRIO_PROCESS, (id_t)pid, new_nice) == 0) {
        printf(COL_GREEN "  ✓ Nice value of PID %d set to %d.\n" COL_RESET,
               pid, new_nice);
        pt_refresh(&g_table);
    } else {
        printf(COL_RED "  ✗ setpriority failed: %s\n" COL_RESET, strerror(errno));
    }
}

static void menu_search(void) {
    char query[128];
    printf(COL_YELLOW "\n  Search by [1] Name  [2] User: " COL_RESET);
    int mode;
    if (scanf("%d", &mode) != 1) { while(getchar()!='\n'); return; }
    while(getchar()!='\n');
    if (mode != 1 && mode != 2) return;

    printf(COL_YELLOW "  Enter search term: " COL_RESET);
    if (!fgets(query, sizeof(query), stdin)) return;
    query[strcspn(query, "\n")] = '\0';

    clear_screen();
    print_banner();
    printf(COL_YELLOW COL_BOLD "\n  SEARCH RESULTS for \"%s\"\n\n" COL_RESET, query);
    print_column_header();

    char rss_buf[16], virt_buf[16];
    int found = 0;
    pthread_mutex_lock(&g_table.lock);
    for (int i = 0; i < g_table.count; i++) {
        ProcessEntry *e = &g_table.entries[i];
        if (!e->valid) continue;
        const char *field = (mode == 1) ? e->name : e->user;
        if (strcasestr(field, query) == NULL) continue;

        format_memory(e->rss_kb,   rss_buf,  sizeof(rss_buf));
        format_memory(e->vsize_kb, virt_buf, sizeof(virt_buf));
        printf("  " COL_BOLD "%-7d" COL_RESET
               " %-20.20s %-10.10s "
               "%s%-9.9s" COL_RESET
               " %6.1f %9s %9s %5d %5d\n",
               e->pid, e->name, e->user,
               state_colour(e->state_char), e->state_str,
               e->cpu_percent, rss_buf, virt_buf,
               e->priority, e->nice);
        found++;
    }
    pthread_mutex_unlock(&g_table.lock);

    if (found == 0)
        printf(COL_RED "  No processes matched \"%s\".\n" COL_RESET, query);
    else
        printf(COL_CYAN "\n  Found %d matching process(es).\n" COL_RESET, found);

    printf(COL_YELLOW "\n  Press ENTER to return..." COL_RESET);
    getchar();
}

static void menu_detail(void) {
    pid_t pid;
    printf(COL_YELLOW "\n  Enter PID for details: " COL_RESET);
    if (scanf("%d", &pid) != 1) { while(getchar()!='\n'); return; }
    while(getchar()!='\n');

    pt_refresh(&g_table);
    clear_screen();
    print_banner();

    pthread_mutex_lock(&g_table.lock);
    ProcessEntry *e = pt_find_by_pid(&g_table, pid);
    if (!e) {
        pthread_mutex_unlock(&g_table.lock);
        printf(COL_RED "\n  PID %d not found.\n" COL_RESET, pid);
        printf(COL_YELLOW "  Press ENTER..." COL_RESET); getchar();
        return;
    }

    char rss_buf[16], virt_buf[16], up_buf[32];
    format_memory(e->rss_kb,   rss_buf,  sizeof(rss_buf));
    format_memory(e->vsize_kb, virt_buf, sizeof(virt_buf));
    time_t age = time(NULL) - e->start_time;
    format_uptime(age, up_buf, sizeof(up_buf));

    printf(COL_YELLOW COL_BOLD "\n  PROCESS DETAIL – PID %d\n\n" COL_RESET, pid);
    printf("  %-20s %s%s%s\n",   "Name:",      COL_BOLD, e->name,       COL_RESET);
    printf("  %-20s %d\n",       "PID:",        e->pid);
    printf("  %-20s %d\n",       "Parent PID:", e->ppid);
    printf("  %-20s %s%s (%c)%s\n","State:",    state_colour(e->state_char),
                                                 e->state_str, e->state_char, COL_RESET);
    printf("  %-20s %s\n",       "User:",       e->user);
    printf("  %-20s %d\n",       "Priority:",   e->priority);
    printf("  %-20s %d\n",       "Nice value:", e->nice);
    printf("  %-20s %ld\n",      "Threads:",    e->num_threads);
    printf("  %-20s %.2f%%\n",   "CPU usage:",  e->cpu_percent);
    printf("  %-20s %s\n",       "RSS (RAM):",  rss_buf);
    printf("  %-20s %s\n",       "Virtual mem:",virt_buf);
    printf("  %-20s %s\n",       "Running for:",up_buf);

    pthread_mutex_unlock(&g_table.lock);

    printf(COL_YELLOW "\n  Press ENTER to return..." COL_RESET);
    getchar();
}

static void menu_spawn_demo(void) {
    pid_t my_pid = getpid();
    printf(COL_YELLOW "\n  [getpid()] This process manager's PID: "
           COL_BOLD "%d" COL_RESET "\n", my_pid);

    if (g_demo_child > 0) {
        int status;
        pid_t r = waitpid(g_demo_child, &status, WNOHANG);
        if (r == 0) {
            printf(COL_YELLOW
                   "  Previous demo child (PID %d) is still running.\n"
                   "  Use option [8] to wait/reap it first.\n" COL_RESET,
                   g_demo_child);
            return;
        }
        g_demo_child = -1;
    }

    printf(COL_YELLOW "  [fork()] Spawning demo child (sleep 30)...\n" COL_RESET);

    pid_t child = fork();
    if (child < 0) {
        perror("fork");
        return;
    }
    if (child == 0) {
        execlp("sleep", "sleep", "30", (char *)NULL);
        perror("execlp");
        _exit(1);
    }

    g_demo_child = child;
    printf(COL_GREEN
           "  [fork()] ✓ Child spawned  → PID %d  (parent PID %d)\n"
           "  [exec()]   Child image replaced with 'sleep 30'\n"
           "  → It will appear in the process list\n"
           "  → Use option [8] to wait() and reap it (demonstrates wait())\n"
           "  → Or send SIGTERM/SIGKILL from option [5]\n"
           COL_RESET, child, my_pid);
    sleep(1);
    pt_refresh(&g_table);
    g_dirty = 1;
}

static void menu_wait_demo(void) {
    if (g_demo_child <= 0) {
        printf(COL_YELLOW
               "\n  No demo child is active. Use option [7] to spawn one first.\n"
               COL_RESET);
        printf(COL_YELLOW "  Press ENTER to return..." COL_RESET);
        getchar();
        return;
    }

    printf(COL_YELLOW
           "\n  Demo child PID: %d\n"
           "  [1] Non-blocking check  (WNOHANG – poll only)\n"
           "  [2] Blocking wait       (waits until child exits/is killed)\n"
           "  [0] Cancel\n"
           "  Choice: " COL_RESET, g_demo_child);

    int choice;
    if (scanf("%d", &choice) != 1) { while(getchar()!='\n'); return; }
    while(getchar()!='\n');

    int status;
    pid_t result;

    switch (choice) {
        case 1:
            result = waitpid(g_demo_child, &status, WNOHANG);
            if (result == 0) {
                printf(COL_CYAN
                       "  [waitpid(WNOHANG)] Child %d is still running (not yet exited).\n"
                       COL_RESET, g_demo_child);
            } else if (result == g_demo_child) {
                printf(COL_GREEN
                       "  [waitpid(WNOHANG)] ✓ Child %d has been reaped.\n"
                       "  Exit status: %d\n" COL_RESET,
                       g_demo_child, WEXITSTATUS(status));
                g_demo_child = -1;
            } else {
                perror("waitpid");
            }
            break;

        case 2:
            printf(COL_CYAN
                   "  [waitpid(block)] Waiting for child %d to exit...\n"
                   "  (Send SIGTERM/SIGKILL from another terminal or option [5])\n"
                   COL_RESET, g_demo_child);
            result = waitpid(g_demo_child, &status, 0);
            if (result == g_demo_child) {
                printf(COL_GREEN
                       "  [waitpid(block)] ✓ Child %d reaped. Exit status: %d\n"
                       COL_RESET, g_demo_child, WEXITSTATUS(status));
                g_demo_child = -1;
            } else {
                perror("waitpid");
            }
            break;

        case 0:
            printf("  Cancelled.\n");
            break;

        default:
            printf(COL_RED "  Invalid choice.\n" COL_RESET);
    }

    pt_refresh(&g_table);
    g_dirty = 1;
    printf(COL_YELLOW "\n  Press ENTER to return..." COL_RESET);
    getchar();
}

static void print_main_menu(void) {
    const char *child_status = (g_demo_child > 0)
        ? COL_GREEN  "[active]"
        : COL_RED    "[none]  ";

    printf("\n" COL_BOLD COL_CYAN
           "  ┌──────────────────── MENU ────────────────────────┐\n"
           "  │" COL_RESET COL_YELLOW " [1]" COL_RESET " List processes             "
           COL_YELLOW "[2]" COL_RESET " Process tree             " COL_BOLD COL_CYAN "│\n"
           "  │" COL_RESET COL_YELLOW " [3]" COL_RESET " Search / filter            "
           COL_YELLOW "[4]" COL_RESET " Process detail           " COL_BOLD COL_CYAN "│\n"
           "  │" COL_RESET COL_YELLOW " [5]" COL_RESET " Send signal (kill)         "
           COL_YELLOW "[6]" COL_RESET " Renice (priority)        " COL_BOLD COL_CYAN "│\n"
           "  │" COL_RESET COL_YELLOW " [7]" COL_RESET " Spawn demo (fork/exec)     "
           COL_YELLOW "[8]" COL_RESET " Wait/reap demo child %s" COL_BOLD COL_CYAN " │\n"
           "  │" COL_RESET COL_YELLOW " [9]" COL_RESET " Sort order                 "
           COL_YELLOW "[r]" COL_RESET " Refresh now              " COL_BOLD COL_CYAN "│\n"
           "  │" COL_RESET COL_YELLOW " [0]" COL_RESET " Quit                                                 " COL_BOLD COL_CYAN "│\n"
           "  └──────────────────────────────────────────────────┘\n"
           COL_RESET,
           child_status);
    printf(COL_RESET COL_YELLOW "  > " COL_RESET);
    fflush(stdout);
}

static void menu_change_sort(void) {
    printf(COL_YELLOW
           "\n  Sort by: [1] PID  [2] CPU  [3] MEM  [4] NAME  [5] PRIORITY\n"
           "  Choice: " COL_RESET);
    int c;
    if (scanf("%d", &c) != 1) { while(getchar()!='\n'); return; }
    while(getchar()!='\n');
    if (c >= 1 && c <= 5) g_sort = (SortOrder)(c - 1);
}

static void handle_sigint(int sig) {
    (void)sig;
    g_running = 0;
}

int main(void) {
    signal(SIGINT, handle_sigint);

    if (pt_init(&g_table) != 0) {
        fprintf(stderr, "Failed to initialise process table.\n");
        return 1;
    }

    printf(COL_CYAN "  Loading process data..." COL_RESET "\n");
    pt_refresh(&g_table);

    pthread_t refresh_tid;
    if (pthread_create(&refresh_tid, NULL, refresh_thread_func, NULL) != 0) {
        perror("pthread_create");
        pt_destroy(&g_table);
        return 1;
    }

    char input_buf[16];
    while (g_running) {
        if (g_dirty) {
            clear_screen();
            print_banner();
            print_sort_legend();
            printf("\n");
            print_column_header();
            print_process_list(30);
            g_dirty = 0;
        }
        print_main_menu();

        if (!fgets(input_buf, sizeof(input_buf), stdin)) break;
        input_buf[strcspn(input_buf, "\n")] = '\0';

        if (input_buf[0] == 'r' || input_buf[0] == 'R') {
            pt_refresh(&g_table);
            g_dirty = 1;
            printf(COL_GREEN "  ✓ Refreshed.\n" COL_RESET);
            continue;
        }

        int choice = atoi(input_buf);

        switch (choice) {
            case 1:
                pt_refresh(&g_table);
                g_dirty = 1;
                break;
            case 2:
                show_tree();
                g_dirty = 1;
                break;
            case 3:
                menu_search();
                g_dirty = 1;
                break;
            case 4:
                menu_detail();
                g_dirty = 1;
                break;
            case 5:
                menu_kill_process();
                break;
            case 6:
                menu_renice();
                break;
            case 7:
                menu_spawn_demo();
                g_dirty = 1;
                break;
            case 8:
                menu_wait_demo();
                g_dirty = 1;
                break;
            case 9:
                menu_change_sort();
                g_dirty = 1;
                break;
            case 0:
                g_running = 0;
                break;
            default:
                if (input_buf[0] != '\0')
                    printf(COL_RED "  Invalid option.\n" COL_RESET);
        }
    }

    printf(COL_CYAN "\n  Shutting down... waiting for refresh thread.\n" COL_RESET);
    pthread_join(refresh_tid, NULL);
    pt_destroy(&g_table);
    printf(COL_GREEN "  Goodbye.\n" COL_RESET);
    return 0;
}
