#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdio.h>
#include <fcntl.h>
#include <termios.h>
#include <dirent.h>

#define DEFAULT_BASE_DIR    "/service"
#define MAX_NAME            32
#define MAX_SERV            64

static struct service {
    bool active;            /* false when a "down" file is present */
    pid_t pid, log_pid;
    unsigned long uptime;   /* in seconds */
    char name[MAX_NAME];
} services[MAX_SERV];

static int name_col_width;

int32_t
read_lei32(unsigned char *buf)
{
    int32_t  n  = buf[3];
    n <<= 8; n += buf[2];
    n <<= 8; n += buf[1];
    n <<= 8; n += buf[0];
    return n;
}

uint64_t
read_beu64(unsigned char *buf)
{
    uint64_t n  = buf[0];
    n <<= 8; n += buf[1];
    n <<= 8; n += buf[2];
    n <<= 8; n += buf[3];
    n <<= 8; n += buf[4];
    n <<= 8; n += buf[5];
    n <<= 8; n += buf[6];
    n <<= 8; n += buf[7];
    return n;
}

void
setup_terminal(struct termios *term_prev)
{
    struct termios term_raw;

    tcgetattr(0, term_prev);
    term_raw = *term_prev;
    term_raw.c_lflag &= ~(ECHO | ICANON);
    term_raw.c_cc[VMIN] = 0x00;
    term_raw.c_cc[VTIME] = 0x01; /* in deciseconds */
    tcsetattr(0, TCSAFLUSH, &term_raw);
}

void
restore_terminal(struct termios *term_prev)
{
    tcsetattr(0, TCSAFLUSH, term_prev);
}

int
cmp_name(const void *a, const void *b)
{
    return strcmp(((struct service *) a)->name, ((struct service *) b)->name);
}

void
load_services(const char *base_dir, int nservices)
{
    int i, fd;
    unsigned char stt[18];
    uint64_t when;
    struct stat st;

    for (i = 0; i < nservices; i++) {
        chdir(base_dir);
        chdir(services[i].name);
        fd = open("supervise/status", O_RDONLY);
        read(fd, stt, sizeof stt);
        close(fd);
        services[i].pid = read_lei32(&stt[12]);
        when = read_beu64(&stt[0]);
        services[i].uptime = time(NULL) + 4611686018427387914ULL - when;
        if ((fd = open("log/supervise/status", O_RDONLY)) < 0) {
            services[i].log_pid = 0;
        } else {
            read(fd, stt, sizeof stt);
            close(fd);
            services[i].log_pid = read_lei32(&stt[12]);
        }
        services[i].active = !!stat("down", &st);
    }
}

void
init_screen(int nservices)
{
    printf(" %-*s active  main   log uptime\n", name_col_width, "name");
    while (nservices--) printf("\n");
}

void
format_uptime(unsigned long *value, char *suffix)
{
    *suffix = 's';
    if (*value >= 60) {
        *value /= 60;
        *suffix = 'm';
        if (*value >= 60) {
            *value /= 60;
            *suffix = 'h';
            if (*value >= 24) {
                *value /= 24;
                *suffix = 'd';
            }
        }
    }
}

void
show_service(struct service *service, bool selected)
{
    unsigned long value;
    char suffix;
    char lsel, rsel;

    lsel = selected ? '<' : ' ';
    rsel = selected ? '>' : ' ';
    printf("%c%-*s ", lsel, name_col_width, service->name);
    printf("%-6s ", service->active ? "yes" : "no");
    if (service->pid)
        printf("%5d ", service->pid);
    else
        printf("%5s ", "---");
    if (service->log_pid)
        printf("%5d ", service->log_pid);
    else
        printf("%5s ", "---");
    if (service->pid) {
        value = service->uptime;
        format_uptime(&value, &suffix);
        printf("%4lu %c", value, suffix);
    } else {
        printf("%6s", "---");
    }
    printf("%c\n", rsel);
}

void
show_services(int nservices, int selection)
{
    int i;

    printf("\x1B[%dA", nservices);
    for (i = 0; i < nservices; i++)
        show_service(&services[i], selection == i);
}

void
send_command(const char *base_dir, int nservices, int selection, char cmd)
{
    struct service *service;
    int fd;

    (void) nservices;
    if (selection == -1)
        return;
    service = &services[selection];
    chdir(base_dir);
    chdir(service->name);
    fd = open("supervise/control", O_WRONLY);
    write(fd, &cmd, 1);
    close(fd);
}

int
main(int argc, char *argv[])
{
    DIR *dir;
    const char *base_dir = NULL;
    struct dirent *entry;
    struct stat st;
    int nservices, selection;
    int name_size;
    struct winsize term_size;
    struct termios term_prev;
    char byte;
    bool running, got_cmd;
    int i;

    if (!isatty(1))
        return 1;   /* quit if stdout is not a terminal */
    if (argc == 2)
        base_dir = argv[1];
    else
        base_dir = getenv("SVDIR");
    if (base_dir == NULL)
        base_dir = DEFAULT_BASE_DIR;
    if ((dir = opendir(base_dir)) == NULL) {
        fprintf(stderr, "could not read directory '%s'\n", base_dir);
        return 1;
    }
    nservices = 0;
    name_col_width = 4;
    while ((entry = readdir(dir)) != NULL) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        chdir(base_dir);
        if (chdir(entry->d_name) < 0)
            continue;
        if (stat("supervise/ok", &st) < 0)
            continue;
        strncpy(services[nservices].name, entry->d_name, MAX_NAME-1);
        name_size = strlen(entry->d_name);
        if (name_size > name_col_width && name_col_width < MAX_NAME)
            name_col_width = name_size;
        nservices++;
        if (nservices == MAX_SERV)
            break;
    }
    closedir(dir);
    if (nservices == 0) {
        fprintf(stderr, "no services to supervise in '%s'\n", base_dir);
        return 1;
    }
    ioctl(0, TIOCGWINSZ, &term_size);
    if (term_size.ws_col < (name_col_width + 28) || term_size.ws_row < (nservices + 3)) {
        fprintf(stderr, "sorry, terminal too small\n");
        return 1;
    }
    qsort(services, nservices, sizeof *services, cmp_name);
    setup_terminal(&term_prev);
    init_screen(nservices);
    selection = -1; /* select all services */
    running = true;
    while (running) {
        load_services(base_dir, nservices);
        show_services(nservices, selection);
        for (i = 0; i < 10; i++) {
            if (read(0, &byte, 1) == 1) {
                if (isupper(byte)) {
                    send_command(base_dir, nservices, selection, tolower(byte));
                    break;
                }
                got_cmd = true;
                switch (byte) {
                case 'q':
                    running = false;
                    break;
                case 'j':
                    selection = (selection + 2) % (nservices + 1) - 1;
                    break;
                case 'k':
                    selection = (selection + nservices + 1) % (nservices + 1) - 1;
                    break;
                default:
                    got_cmd = false;
                }
                if (got_cmd) break;
            }
        }
    }
    restore_terminal(&term_prev);
    return 0;
}
