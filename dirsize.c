// dirsize — fast parallel disk usage tree for Linux (C11 + pthreads).
//
// Default: scan "/" on its own filesystem (like `df -h` for /), and print a
// size-sorted tree. Sizes are allocated bytes (st_blocks * 512) unless -A.
// Hard links are counted once. Symlinks are never followed.
//
// Build: make   (or: cc -O2 -pthread -o dirsize dirsize.c)

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define VERSION "2.0.0"
#define DENTS_BUF (64 * 1024)

/* ---------- options ---------- */

static int opt_depth = 2;
static int opt_top = 10;
static int opt_threads = 0;
static int opt_apparent = 0;
static int opt_cross = 0;
static int opt_progress = 1;
static uint64_t opt_min = 0;
static int use_color = 0;

/* ---------- tree ---------- */

typedef struct node {
    char *name;
    struct node *parent;
    struct node **kids;
    int nkids, capkids;
    int depth;
    int is_mount;       // other filesystem, not scanned
    _Atomic uint64_t self;   // own inode + direct files (+ everything below if depth == max)
    _Atomic uint64_t errors;
    uint64_t total;     // computed after scan
    uint64_t files_direct;   // direct files only (for display)
} node;

static node *node_new(const char *name, node *parent, int depth) {
    node *n = calloc(1, sizeof *n);
    n->name = strdup(name);
    n->parent = parent;
    n->depth = depth;
    return n;
}

static void node_add_kid(node *p, node *k) {
    if (p->nkids == p->capkids) {
        p->capkids = p->capkids ? p->capkids * 2 : 8;
        p->kids = realloc(p->kids, p->capkids * sizeof *p->kids);
    }
    p->kids[p->nkids++] = k;
}

/* ---------- work queue (LIFO stack, keeps memory low) ---------- */

typedef struct {
    char *path;
    node *target;   // node that receives sizes from this directory
    int depth;
} work;

static work *wq;
static size_t wq_len, wq_cap;
static int wq_active;
static int wq_done;
static pthread_mutex_t wq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wq_cv = PTHREAD_COND_INITIALIZER;

static void wq_push_many(work *items, size_t n) {
    if (!n) return;
    pthread_mutex_lock(&wq_mu);
    if (wq_len + n > wq_cap) {
        while (wq_len + n > wq_cap) wq_cap = wq_cap ? wq_cap * 2 : 1024;
        wq = realloc(wq, wq_cap * sizeof *wq);
    }
    memcpy(wq + wq_len, items, n * sizeof *items);
    wq_len += n;
    if (n == 1) pthread_cond_signal(&wq_cv);
    else pthread_cond_broadcast(&wq_cv);
    pthread_mutex_unlock(&wq_mu);
}

static int wq_pop(work *out) {
    pthread_mutex_lock(&wq_mu);
    while (wq_len == 0 && !wq_done) {
        if (wq_active == 0) {   // nobody can produce more work
            wq_done = 1;
            pthread_cond_broadcast(&wq_cv);
            break;
        }
        pthread_cond_wait(&wq_cv, &wq_mu);
    }
    if (wq_done && wq_len == 0) {
        pthread_mutex_unlock(&wq_mu);
        return 0;
    }
    *out = wq[--wq_len];
    wq_active++;
    pthread_mutex_unlock(&wq_mu);
    return 1;
}

static void wq_finish_one(void) {
    pthread_mutex_lock(&wq_mu);
    wq_active--;
    if (wq_active == 0 && wq_len == 0) {
        wq_done = 1;
        pthread_cond_broadcast(&wq_cv);
    }
    pthread_mutex_unlock(&wq_mu);
}

/* ---------- hard link dedupe (sharded hash set of dev/ino) ---------- */

#define SHARDS 256
typedef struct {
    pthread_mutex_t mu;
    uint64_t *keys;     // pairs: dev, ino; dev==0&&ino==0 means empty
    size_t cap, len;
} shard;
static shard hl[SHARDS];

static uint64_t mix(uint64_t x) {
    x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33; return x;
}

// Returns 1 if (dev, ino) was newly inserted, 0 if already seen.
static int hl_insert(uint64_t dev, uint64_t ino) {
    uint64_t h = mix(ino * 31 + dev);
    shard *s = &hl[h % SHARDS];
    pthread_mutex_lock(&s->mu);
    if ((s->len + 1) * 2 > s->cap) {
        size_t ncap = s->cap ? s->cap * 2 : 1024;
        uint64_t *nk = calloc(ncap * 2, sizeof *nk);
        for (size_t i = 0; i < s->cap; i++) {
            uint64_t d = s->keys[2 * i], in = s->keys[2 * i + 1];
            if (!d && !in) continue;
            size_t j = (mix(in * 31 + d) / SHARDS) & (ncap - 1);
            while (nk[2 * j] || nk[2 * j + 1]) j = (j + 1) & (ncap - 1);
            nk[2 * j] = d; nk[2 * j + 1] = in;
        }
        free(s->keys);
        s->keys = nk; s->cap = ncap;
    }
    size_t j = (h / SHARDS) & (s->cap - 1);
    for (;;) {
        uint64_t d = s->keys[2 * j], in = s->keys[2 * j + 1];
        if (!d && !in) {
            s->keys[2 * j] = dev ? dev : 1; s->keys[2 * j + 1] = ino;
            s->len++;
            pthread_mutex_unlock(&s->mu);
            return 1;
        }
        if (d == (dev ? dev : 1) && in == ino) {
            pthread_mutex_unlock(&s->mu);
            return 0;
        }
        j = (j + 1) & (s->cap - 1);
    }
}

/* ---------- stats ---------- */

static _Atomic uint64_t st_dirs, st_files, st_errors, st_bytes, st_mounts;
static dev_t root_dev;

#define ERR_SAMPLES 10
static char *err_samples[ERR_SAMPLES];
static _Atomic int err_n;

static void record_error(node *target, const char *path, int err) {
    atomic_fetch_add(&target->errors, 1);
    atomic_fetch_add(&st_errors, 1);
    int i = atomic_fetch_add(&err_n, 1);
    if (i < ERR_SAMPLES) {
        char *msg;
        if (asprintf(&msg, "%s: %s", path, strerror(err)) >= 0) err_samples[i] = msg;
    }
}

static inline uint64_t size_of(const struct stat *st) {
    if (opt_apparent)
        return (S_ISREG(st->st_mode) || S_ISLNK(st->st_mode)) ? (uint64_t)st->st_size : 0;
    return (uint64_t)st->st_blocks * 512;
}

/* ---------- scanner ---------- */

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

static char *join(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    int slash = la && a[la - 1] != '/';
    char *p = malloc(la + slash + lb + 1);
    memcpy(p, a, la);
    if (slash) p[la] = '/';
    memcpy(p + la + slash, b, lb + 1);
    return p;
}

static void scan_dir(work *w, char *buf, work **batch, size_t *bcap) {
    int fd = open(w->path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC | O_NOATIME);
    if (fd < 0 && errno == EPERM)
        fd = open(w->path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        record_error(w->target, w->path, errno);
        return;
    }
    atomic_fetch_add(&st_dirs, 1);

    uint64_t local = 0, files = 0;
    size_t nb = 0;
    int tracked_kids = w->depth < opt_depth && w->target->depth == w->depth;

    for (;;) {
        long n = syscall(SYS_getdents64, fd, buf, DENTS_BUF);
        if (n < 0) { record_error(w->target, w->path, errno); break; }
        if (n == 0) break;
        for (long off = 0; off < n;) {
            struct linux_dirent64 *d = (struct linux_dirent64 *)(buf + off);
            off += d->d_reclen;
            const char *nm = d->d_name;
            if (nm[0] == '.' && (!nm[1] || (nm[1] == '.' && !nm[2]))) continue;

            struct stat st;
            if (fstatat(fd, nm, &st, AT_SYMLINK_NOFOLLOW) < 0) {
                char *p = join(w->path, nm);
                record_error(w->target, p, errno);
                free(p);
                continue;
            }

            if (S_ISDIR(st.st_mode)) {
                if (!opt_cross && st.st_dev != root_dev) {
                    atomic_fetch_add(&st_mounts, 1);
                    if (tracked_kids) {
                        node *k = node_new(nm, w->target, w->depth + 1);
                        k->is_mount = 1;
                        node_add_kid(w->target, k);
                    }
                    continue;
                }
                node *tgt = w->target;
                if (tracked_kids) {
                    tgt = node_new(nm, w->target, w->depth + 1);
                    node_add_kid(w->target, tgt);
                }
                atomic_fetch_add(&tgt->self, size_of(&st));
                if (nb == *bcap) {
                    *bcap *= 2;
                    *batch = realloc(*batch, *bcap * sizeof **batch);
                }
                (*batch)[nb++] = (work){join(w->path, nm), tgt, w->depth + 1};
                continue;
            }

            if (st.st_nlink > 1 && !hl_insert(st.st_dev, st.st_ino)) continue;
            uint64_t sz = size_of(&st);
            local += sz;
            files += sz;
            atomic_fetch_add(&st_files, 1);
        }
    }
    close(fd);

    if (local) {
        atomic_fetch_add(&w->target->self, local);
        atomic_fetch_add(&st_bytes, local);
    }
    if (files && w->target->depth == w->depth) w->target->files_direct += files;
    wq_push_many(*batch, nb);
}

static void *worker(void *arg) {
    (void)arg;
    char *buf = malloc(DENTS_BUF);
    size_t bcap = 256;
    work *batch = malloc(bcap * sizeof *batch);
    work w;
    while (wq_pop(&w)) {
        scan_dir(&w, buf, &batch, &bcap);
        free(w.path);
        wq_finish_one();
    }
    free(batch);
    free(buf);
    return NULL;
}

/* ---------- output ---------- */

static void human(uint64_t v, char *out, size_t n) {
    const char *u[] = {"B", "K", "M", "G", "T", "P", "E"};
    double x = (double)v;
    int i = 0;
    while (x >= 1024 && i < 6) { x /= 1024; i++; }
    if (i == 0) snprintf(out, n, "%llu%s", (unsigned long long)v, u[i]);
    // Round up like df -h / du -h, so numbers match those tools.
    else if (x < 10) snprintf(out, n, "%.1f%s", ceil(x * 10) / 10, u[i]);
    else snprintf(out, n, "%.0f%s", ceil(x), u[i]);
}

static void safe_print(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) printf("\\x%02x", *p);
        else putchar(*p);
    }
}

static uint64_t compute_totals(node *n) {
    uint64_t t = atomic_load(&n->self);
    for (int i = 0; i < n->nkids; i++) {
        t += compute_totals(n->kids[i]);
        n->errors += atomic_load(&n->kids[i]->errors);
    }
    n->total = t;
    return t;
}

static int cmp_kids(const void *a, const void *b) {
    const node *x = *(node *const *)a, *y = *(node *const *)b;
    if (x->total != y->total) return x->total < y->total ? 1 : -1;
    return strcmp(x->name, y->name);
}

#define C(code) (use_color ? "\033[" code "m" : "")

static const char *size_color(uint64_t v) {
    if (!use_color) return "";
    if (v >= (1ULL << 40)) return "\033[1;31m";   // >= 1T
    if (v >= (100ULL << 30)) return "\033[31m";   // >= 100G
    if (v >= (10ULL << 30)) return "\033[33m";    // >= 10G
    if (v >= (1ULL << 30)) return "\033[32m";     // >= 1G
    return "\033[2m";
}

static void print_row(uint64_t size, uint64_t whole, const char *prefix,
                      const char *branch, const char *name, int dir, int bad,
                      int mount, const char *note) {
    char hs[16];
    human(size, hs, sizeof hs);
    double pct = whole ? 100.0 * (double)size / (double)whole : 0;
    int fill = (int)(pct / 10 + 0.5);
    if (fill > 10) fill = 10;
    printf("%s%7s%s %5.1f%% %s", size_color(size), mount ? "-" : hs, C("0"), pct, C("36"));
    for (int i = 0; i < 10; i++) fputs(i < fill ? "█" : "░", stdout);
    printf("%s  %s%s", C("0"), prefix, branch);
    if (dir) printf("%s", C("1;34"));
    else printf("%s", C("2"));
    safe_print(name);
    size_t nl = strlen(name);
    printf("%s%s", dir && nl && name[nl - 1] != '/' ? "/" : "", C("0"));
    if (mount) printf("  %s[다른 파일시스템 · -c 로 포함]%s", C("2"), C("0"));
    if (bad) printf("  %s! 읽기오류%s", C("33"), C("0"));
    if (note) printf("  %s%s%s", C("2"), note, C("0"));
    putchar('\n');
}

static void print_tree(node *n, uint64_t whole, char *prefix) {
    if (n->nkids) qsort(n->kids, n->nkids, sizeof *n->kids, cmp_kids);

    // Entries: kids (+ a pseudo entry for direct files), top-N, rest folded.
    int shown = 0, hidden = 0;
    uint64_t hidden_sz = 0;
    int files_shown = n->files_direct == 0 || n->depth >= opt_depth;
    int limit = opt_top > 0 ? opt_top : 1 << 30;

    // count visible to know which is last
    int total_entries = 0;
    for (int i = 0; i < n->nkids; i++)
        if (n->kids[i]->total >= opt_min || n->kids[i]->is_mount) total_entries++;
    if (!files_shown && n->files_direct >= opt_min) total_entries++;
    int visible = total_entries < limit ? total_entries : limit;
    int rest_line = 0;
    for (int i = 0; i < n->nkids; i++) if (!(n->kids[i]->total >= opt_min || n->kids[i]->is_mount)) rest_line = 1;
    if (total_entries > limit) rest_line = 1;
    if (!files_shown && n->files_direct < opt_min) rest_line = 1;

    size_t plen = strlen(prefix);
    int ki = 0;
    while (ki < n->nkids || !files_shown) {
        node *k = ki < n->nkids ? n->kids[ki] : NULL;
        int take_files = !files_shown && (!k || n->files_direct >= k->total);
        uint64_t sz = take_files ? n->files_direct : k->total;
        int eligible = sz >= opt_min || (!take_files && k->is_mount);

        if (eligible && shown < limit) {
            shown++;
            int last = shown == visible && !rest_line;
            const char *branch = last ? "└── " : "├── ";
            if (take_files) {
                print_row(sz, whole, prefix, branch, "(이 폴더의 파일들)", 0, 0, 0, NULL);
            } else {
                print_row(sz, whole, prefix, branch, k->name, 1, k->errors > 0, k->is_mount, NULL);
                if (k->nkids || k->files_direct) {
                    strcpy(prefix + plen, last ? "    " : "│   ");
                    print_tree(k, whole, prefix);
                    prefix[plen] = 0;
                }
            }
        } else {
            hidden++;
            hidden_sz += sz;
        }
        if (take_files) files_shown = 1;
        else ki++;
    }
    if (hidden) {
        char note[64];
        snprintf(note, sizeof note, "외 %d개", hidden);
        print_row(hidden_sz, whole, prefix, "└── ", "…", 0, 0, 0, note);
    }
}

/* ---------- df-like header ---------- */

static void find_mount(const char *path, char *src, size_t srcn, char *mnt, size_t mntn) {
    snprintf(src, srcn, "?");
    snprintf(mnt, mntn, "?");
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (!f) return;
    char line[4096];
    size_t best = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned maj, min;
        char m[2048], s[1024];
        if (sscanf(line, "%*d %*d %u:%u %*s %2047s", &maj, &min, m) != 3) continue;
        if (makedev(maj, min) != root_dev) continue;
        char *dash = strstr(line, " - ");
        if (!dash || sscanf(dash, " - %*s %1023s", s) != 1) continue;
        size_t l = strlen(m);
        if (strncmp(path, m, l) == 0 && (path[l] == '/' || path[l] == 0 || l == 1) && l >= best) {
            best = l;
            snprintf(mnt, mntn, "%s", m);
            snprintf(src, srcn, "%s", s);
        }
    }
    fclose(f);
}

/* ---------- main ---------- */

static struct timespec t0;
static double elapsed(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
}

static uint64_t parse_size(const char *s) {
    char *end;
    double v = strtod(s, &end);
    uint64_t mul = 1;
    switch (*end) {
    case 'k': case 'K': mul = 1ULL << 10; break;
    case 'm': case 'M': mul = 1ULL << 20; break;
    case 'g': case 'G': mul = 1ULL << 30; break;
    case 't': case 'T': mul = 1ULL << 40; break;
    }
    return (uint64_t)(v * mul);
}

static void usage(FILE *f) {
    fprintf(f,
        "사용법: dirsize [옵션] [경로]   (기본 경로: /)\n"
        "\n"
        "경로부터 디렉터리 용량을 병렬로 계산해 큰 순서의 트리로 보여줍니다.\n"
        "기본은 시작 경로와 같은 파일시스템만 탐색합니다 (df -h 의 한 줄과 대응).\n"
        "\n"
        "  -d, --depth N      트리 표시 깊이 (기본 2)\n"
        "  -n, --top N        폴더마다 큰 항목 N개만 표시, 0 = 전부 (기본 10)\n"
        "  -m, --min SIZE     SIZE 미만 항목 숨김 (예: 100M, 1G)\n"
        "  -j, --threads N    스레드 수 (기본: CPU 수 x 2, 최대 64)\n"
        "  -c, --cross        다른 파일시스템(마운트)도 따라 들어감\n"
        "  -A, --apparent     할당 공간 대신 논리적 파일 크기\n"
        "  -q, --quiet        진행 표시 끄기\n"
        "      --no-color     색상 끄기\n"
        "  -h, --help         도움말\n"
        "  -V, --version      버전\n"
        "\n"
        "권한 없는 폴더는 건너뜁니다. 전체를 보려면: sudo dirsize\n");
}

static volatile sig_atomic_t interrupted;
static void on_sigint(int s) { (void)s; interrupted = 1; }

int main(int argc, char **argv) {
    int no_color = 0;
    static struct option lo[] = {
        {"depth", 1, 0, 'd'}, {"top", 1, 0, 'n'}, {"min", 1, 0, 'm'},
        {"threads", 1, 0, 'j'}, {"cross", 0, 0, 'c'}, {"apparent", 0, 0, 'A'},
        {"quiet", 0, 0, 'q'}, {"no-color", 0, 0, 1}, {"help", 0, 0, 'h'},
        {"version", 0, 0, 'V'}, {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "d:n:m:j:cAqhV", lo, NULL)) != -1) {
        switch (c) {
        case 'd': opt_depth = atoi(optarg); break;
        case 'n': opt_top = atoi(optarg); break;
        case 'm': opt_min = parse_size(optarg); break;
        case 'j': opt_threads = atoi(optarg); break;
        case 'c': opt_cross = 1; break;
        case 'A': opt_apparent = 1; break;
        case 'q': opt_progress = 0; break;
        case 1: no_color = 1; break;
        case 'h': usage(stdout); return 0;
        case 'V': puts("dirsize " VERSION); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (opt_depth < 0 || opt_top < 0) { usage(stderr); return 2; }
    if (argc - optind > 1) { usage(stderr); return 2; }

    char *root = realpath(optind < argc ? argv[optind] : "/", NULL);
    if (!root) { perror(optind < argc ? argv[optind] : "/"); return 2; }
    struct stat rst;
    if (stat(root, &rst) < 0 || !S_ISDIR(rst.st_mode)) {
        fprintf(stderr, "디렉터리가 아닙니다: %s\n", root);
        return 2;
    }
    root_dev = rst.st_dev;
    use_color = isatty(STDOUT_FILENO) && !no_color && !getenv("NO_COLOR");
    if (!isatty(STDERR_FILENO)) opt_progress = 0;

    if (opt_threads <= 0) {
        long nc = sysconf(_SC_NPROCESSORS_ONLN);
        opt_threads = nc > 0 ? (int)nc * 2 : 8;
    }
    if (opt_threads > 64) opt_threads = 64;
    for (int i = 0; i < SHARDS; i++) pthread_mutex_init(&hl[i].mu, NULL);

    signal(SIGINT, on_sigint);
    clock_gettime(CLOCK_MONOTONIC, &t0);

    node *top = node_new(root, NULL, 0);
    top->self = size_of(&rst);
    work first = {strdup(root), top, 0};
    wq_push_many(&first, 1);

    pthread_t *th = malloc(opt_threads * sizeof *th);
    for (int i = 0; i < opt_threads; i++) pthread_create(&th[i], NULL, worker, NULL);

    if (opt_progress) {
        for (;;) {
            pthread_mutex_lock(&wq_mu);
            int done = wq_done;
            pthread_mutex_unlock(&wq_mu);
            if (done || interrupted) break;
            char hs[16];
            human(atomic_load(&st_bytes), hs, sizeof hs);
            fprintf(stderr, "\r\033[K탐색 중… %s · 폴더 %llu · 파일 %llu · %.1fs",
                    hs, (unsigned long long)st_dirs, (unsigned long long)st_files, elapsed());
            struct timespec ts = {0, 100 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
        fputs("\r\033[K", stderr);
    }
    if (interrupted) {
        fputs("\n중단했습니다.\n", stderr);
        _exit(130);
    }
    for (int i = 0; i < opt_threads; i++) pthread_join(th[i], NULL);
    if (interrupted) { fputs("\n중단했습니다.\n", stderr); return 130; }
    double secs = elapsed();

    compute_totals(top);

    // df-like header
    struct statvfs vfs;
    char src[1024], mnt[2048];
    find_mount(root, src, sizeof src, mnt, sizeof mnt);
    if (statvfs(root, &vfs) == 0) {
        uint64_t fsz = (uint64_t)vfs.f_blocks * vfs.f_frsize;
        uint64_t favail = (uint64_t)vfs.f_bavail * vfs.f_frsize;
        uint64_t fused = fsz - (uint64_t)vfs.f_bfree * vfs.f_frsize;
        char a[16], b[16], d[16];
        human(fsz, a, sizeof a); human(fused, b, sizeof b); human(favail, d, sizeof d);
        int pct = (fused + favail) ? (int)((fused * 100 + fused + favail - 1) / (fused + favail)) : 0;
        printf("%sFilesystem%s      %-24s %sMounted on%s %s\n", C("1"), C("0"), src, C("1"), C("0"), mnt);
        printf("%sSize%s %s   %sUsed%s %s   %sAvail%s %s   %sUse%%%s %d%%\n",
               C("1"), C("0"), a, C("1"), C("0"), b, C("1"), C("0"), d, C("1"), C("0"), pct);
        char t[16];
        human(top->total, t, sizeof t);
        if (!opt_apparent && opt_cross == 0 && strcmp(root, mnt) == 0 && fused)
            printf("스캔 합계 %s (df Used 의 %.1f%%)", t, 100.0 * top->total / fused);
        else
            printf("스캔 합계 %s", t);
        printf(" · %.2fs · 폴더 %llu · 파일 %llu · 스레드 %d%s\n\n", secs,
               (unsigned long long)st_dirs, (unsigned long long)st_files, opt_threads,
               opt_apparent ? " · 논리 크기" : "");
    }

    char prefix[4096] = "";
    print_row(top->total, top->total, "", "", root, 1, top->errors > 0, 0, NULL);
    print_tree(top, top->total, prefix);

    if (st_errors) {
        fprintf(stderr, "\n경고: 읽기 오류 %llu건 (합계가 실제보다 작을 수 있음)%s\n",
                (unsigned long long)st_errors, geteuid() ? " — 전체를 보려면 sudo 로 실행" : "");
        int n = err_n < ERR_SAMPLES ? err_n : ERR_SAMPLES;
        for (int i = 0; i < n; i++) if (err_samples[i]) fprintf(stderr, "  %s\n", err_samples[i]);
        if (err_n > ERR_SAMPLES) fprintf(stderr, "  … 외 %d건\n", err_n - ERR_SAMPLES);
    }
    if (st_mounts && !opt_cross)
        fprintf(stderr, "다른 파일시스템 %llu곳은 건너뜀 (포함하려면 -c)\n",
                (unsigned long long)st_mounts);
    return st_errors ? 1 : 0;
}
