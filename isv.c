#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
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

static char path[4096];
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

int
sprint_uptime(char buf[9], unsigned long seconds)
{
    char suffix = 's';
    unsigned long value = seconds;

    if (value >= 60) {
        value /= 60;
        suffix = 'm';
    }
    if (value >= 60) {
        value /= 60;
        suffix = 'h';
    }
    if (value >= 24) {
        value /= 24;
        suffix = 'd';
    }
    return snprintf(buf, 9, "%5lu %c", value, suffix);
}

void
load_services(const char *base_dir, int nservices)
{
    int i, fd;
    unsigned char stt[18];
    uint64_t when;
    struct stat st;

    for (i = 0; i < nservices; i++) {
        strcpy(path, base_dir);
        strcat(path, "/");
        strcat(path, services[i].name);
        strcat(path, "/supervise/status");
        fd = open(path, O_RDONLY);
        read(fd, stt, sizeof stt);
        close(fd);
        services[i].pid = read_lei32(&stt[12]);
        when = read_beu64(&stt[0]);
        services[i].uptime = time(NULL) + 4611686018427387914ULL - when;
        strcpy(path, base_dir);
        strcat(path, "/");
        strcat(path, services[i].name);
        strcat(path, "/log/supervise/status");
        fd = open(path, O_RDONLY);
        read(fd, stt, sizeof stt);
        close(fd);
        services[i].log_pid = read_lei32(&stt[12]);
        strcpy(path, base_dir);
        strcat(path, "down");
        services[i].active = !!stat(path, &st);
    }
}

void
show_services(int nservices)
{
    int i;
    char uptime_str[8];

    printf("%*s active   run   log  uptime\n", name_col_width, "name");
    for (i = 0; i < nservices; i++) {
        sprint_uptime(uptime_str, services[i].uptime);
        printf("%*s %6s %5d %5d %s\n",
               name_col_width, services[i].name,
               services[i].active ? "yes" : "no",
               services[i].pid, services[i].log_pid,
               uptime_str);
    }
}

int
main(int argc, char *argv[])
{
    DIR *dir;
    const char *base_dir = NULL;
    struct dirent *entry;
    int nservices;
    int name_size;
    struct winsize term_size;
    struct termios term_prev, term_raw;
    char byte;

    if (!isatty(0))
        return 1;   /* quit if stdin is not a terminal */
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
        fprintf(stderr, "no services in '%s'\n", base_dir);
        return 1;
    }
    ioctl(0, TIOCGWINSZ, &term_size);
    if (term_size.ws_col < (name_col_width + 29) || term_size.ws_row < (nservices + 3)) {
        fprintf(stderr, "sorry, terminal too small\n");
        return 1;
    }
    tcgetattr(0, &term_prev);
    term_raw = term_prev;
    term_raw.c_lflag &= ~(ECHO | ICANON);
    term_raw.c_cc[VMIN] = 0x00;
    term_raw.c_cc[VTIME] = 0x01; /* in deciseconds */
    tcsetattr(0, TCSAFLUSH, &term_raw);
    load_services(base_dir, nservices);
    show_services(nservices);
    while (1) {
        if (read(0, &byte, 1) == 1) {
            if (byte == 'q')
                break;
        }
    }
    tcsetattr(0, TCSAFLUSH, &term_prev);
    return 0;
}
