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
#include <glob.h>
#include <linux/nvme_ioctl.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define VERSION "2.2.0"
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

static char *join(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    int slash = la && a[la - 1] != '/';
    char *p = malloc(la + slash + lb + 1);
    memcpy(p, a, la);
    if (slash) p[la] = '/';
    memcpy(p + la + slash, b, lb + 1);
    return p;
}

/* ---------- file categories (by extension) ---------- */

enum { CAT_VIDEO, CAT_IMAGE, CAT_AUDIO, CAT_MODEL, CAT_DATA, CAT_ARCHIVE, CAT_DOC,
       CAT_CODE, CAT_BINARY, CAT_LOG, CAT_OTHER, NCAT };

static const char *cat_names[NCAT] = {
    "Video", "Images", "Audio", "AI models / weights", "Datasets / DB", "Archives / disk images",
    "Documents", "Source code", "Libraries / binaries", "Logs", "Other / no extension"};
static const char *cat_hint[NCAT] = {
    ".mp4 .mkv .mov", ".jpg .png .heic", ".mp3 .flac .wav", ".safetensors .gguf .pt .bin",
    ".npy .parquet .csv .db", ".zip .tar.gz .iso .ipsw", ".pdf .docx .hwp .md",
    ".c .py .js .rs", ".so .a .o .pyc", ".log .journal", ""};

static const struct { const char *ext; int cat; } ext_table[] = {
    {"mp4", CAT_VIDEO}, {"mkv", CAT_VIDEO}, {"avi", CAT_VIDEO}, {"mov", CAT_VIDEO},
    {"webm", CAT_VIDEO}, {"flv", CAT_VIDEO}, {"wmv", CAT_VIDEO}, {"m4v", CAT_VIDEO},
    {"mpg", CAT_VIDEO}, {"mpeg", CAT_VIDEO}, {"m2ts", CAT_VIDEO},
    {"jpg", CAT_IMAGE}, {"jpeg", CAT_IMAGE}, {"png", CAT_IMAGE}, {"gif", CAT_IMAGE},
    {"bmp", CAT_IMAGE}, {"webp", CAT_IMAGE}, {"tif", CAT_IMAGE}, {"tiff", CAT_IMAGE},
    {"heic", CAT_IMAGE}, {"svg", CAT_IMAGE}, {"psd", CAT_IMAGE}, {"raw", CAT_IMAGE},
    {"cr2", CAT_IMAGE}, {"nef", CAT_IMAGE}, {"dng", CAT_IMAGE}, {"ico", CAT_IMAGE},
    {"mp3", CAT_AUDIO}, {"flac", CAT_AUDIO}, {"wav", CAT_AUDIO}, {"ogg", CAT_AUDIO},
    {"m4a", CAT_AUDIO}, {"aac", CAT_AUDIO}, {"opus", CAT_AUDIO}, {"wma", CAT_AUDIO},
    {"safetensors", CAT_MODEL}, {"gguf", CAT_MODEL}, {"ggml", CAT_MODEL}, {"pt", CAT_MODEL},
    {"pth", CAT_MODEL}, {"ckpt", CAT_MODEL}, {"onnx", CAT_MODEL}, {"bin", CAT_MODEL},
    {"h5", CAT_MODEL}, {"keras", CAT_MODEL}, {"tflite", CAT_MODEL}, {"engine", CAT_MODEL},
    {"npy", CAT_DATA}, {"npz", CAT_DATA}, {"parquet", CAT_DATA}, {"arrow", CAT_DATA},
    {"csv", CAT_DATA}, {"tsv", CAT_DATA}, {"json", CAT_DATA}, {"jsonl", CAT_DATA},
    {"hdf5", CAT_DATA}, {"db", CAT_DATA}, {"sqlite", CAT_DATA}, {"sqlite3", CAT_DATA},
    {"pkl", CAT_DATA}, {"pickle", CAT_DATA}, {"feather", CAT_DATA}, {"mdb", CAT_DATA},
    {"tfrecord", CAT_DATA}, {"idx", CAT_DATA}, {"pack", CAT_DATA},
    {"zip", CAT_ARCHIVE}, {"tar", CAT_ARCHIVE}, {"gz", CAT_ARCHIVE}, {"tgz", CAT_ARCHIVE},
    {"xz", CAT_ARCHIVE}, {"zst", CAT_ARCHIVE}, {"bz2", CAT_ARCHIVE}, {"7z", CAT_ARCHIVE},
    {"rar", CAT_ARCHIVE}, {"iso", CAT_ARCHIVE}, {"img", CAT_ARCHIVE}, {"qcow2", CAT_ARCHIVE},
    {"vmdk", CAT_ARCHIVE}, {"vdi", CAT_ARCHIVE}, {"deb", CAT_ARCHIVE}, {"rpm", CAT_ARCHIVE},
    {"snap", CAT_ARCHIVE}, {"whl", CAT_ARCHIVE}, {"appimage", CAT_ARCHIVE},
    {"ipsw", CAT_ARCHIVE}, {"dmg", CAT_ARCHIVE}, {"vhd", CAT_ARCHIVE}, {"vhdx", CAT_ARCHIVE},
    {"ova", CAT_ARCHIVE}, {"xip", CAT_ARCHIVE}, {"pkg", CAT_ARCHIVE}, {"squashfs", CAT_ARCHIVE}, {"lz4", CAT_ARCHIVE},
    {"pdf", CAT_DOC}, {"doc", CAT_DOC}, {"docx", CAT_DOC}, {"xls", CAT_DOC},
    {"xlsx", CAT_DOC}, {"ppt", CAT_DOC}, {"pptx", CAT_DOC}, {"hwp", CAT_DOC},
    {"hwpx", CAT_DOC}, {"txt", CAT_DOC}, {"md", CAT_DOC}, {"epub", CAT_DOC},
    {"odt", CAT_DOC}, {"rtf", CAT_DOC}, {"tex", CAT_DOC},
    {"c", CAT_CODE}, {"h", CAT_CODE}, {"cc", CAT_CODE}, {"cpp", CAT_CODE},
    {"hpp", CAT_CODE}, {"cu", CAT_CODE}, {"py", CAT_CODE}, {"js", CAT_CODE},
    {"mjs", CAT_CODE}, {"cjs", CAT_CODE}, {"ts", CAT_CODE}, {"tsx", CAT_CODE},
    {"jsx", CAT_CODE}, {"java", CAT_CODE}, {"go", CAT_CODE}, {"rs", CAT_CODE},
    {"rb", CAT_CODE}, {"php", CAT_CODE}, {"sh", CAT_CODE}, {"html", CAT_CODE},
    {"css", CAT_CODE}, {"ipynb", CAT_CODE}, {"lua", CAT_CODE}, {"kt", CAT_CODE},
    {"swift", CAT_CODE}, {"yaml", CAT_CODE}, {"yml", CAT_CODE}, {"toml", CAT_CODE},
    {"xml", CAT_CODE}, {"map", CAT_CODE},
    {"so", CAT_BINARY}, {"a", CAT_BINARY}, {"o", CAT_BINARY}, {"ko", CAT_BINARY},
    {"dll", CAT_BINARY}, {"exe", CAT_BINARY}, {"pyc", CAT_BINARY},
    {"class", CAT_BINARY}, {"jar", CAT_BINARY}, {"node", CAT_BINARY}, {"wasm", CAT_BINARY},
    {"hsaco", CAT_BINARY}, {"dylib", CAT_BINARY}, {"elf", CAT_BINARY},
    {"log", CAT_LOG}, {"journal", CAT_LOG}, {"journal~", CAT_LOG},
};

// Extensions are packed (lowercased, first 8 bytes) into a u64 and binary-searched.
typedef struct { uint64_t key; int cat; } ext_ent;
static ext_ent *ext_idx;
static size_t ext_n;

static uint64_t ext_key(const char *e) {
    uint64_t k = 0;
    for (int i = 0; i < 8 && e[i]; i++) {
        unsigned char ch = (unsigned char)e[i];
        if (ch >= 'A' && ch <= 'Z') ch += 32;
        k = (k << 8) | ch;
    }
    return k;
}

static int cmp_ext(const void *a, const void *b) {
    uint64_t x = ((const ext_ent *)a)->key, y = ((const ext_ent *)b)->key;
    return x < y ? -1 : x > y;
}

static void ext_init(void) {
    size_t n = sizeof ext_table / sizeof *ext_table;
    ext_idx = malloc(n * sizeof *ext_idx);
    for (size_t i = 0; i < n; i++) ext_idx[ext_n++] = (ext_ent){ext_key(ext_table[i].ext), ext_table[i].cat};
    qsort(ext_idx, ext_n, sizeof *ext_idx, cmp_ext);
}

static int classify(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot || dot == name || !dot[1]) {
        // e.g. "foo.so.6" -> look for ".so." ; git objects, ELF binaries without ext -> other
        return strstr(name, ".so.") ? CAT_BINARY : CAT_OTHER;
    }
    if (dot[1] >= '0' && dot[1] <= '9' && strstr(name, ".so.")) return CAT_BINARY;
    ext_ent k = {ext_key(dot + 1), 0};
    ext_ent *r = bsearch(&k, ext_idx, ext_n, sizeof *ext_idx, cmp_ext);
    return r ? r->cat : CAT_OTHER;
}

/* ---------- per-thread context: category totals + largest files (min-heap) ---------- */

typedef struct { uint64_t size; char *path; } bigfile;

typedef struct {
    char *buf;
    work *batch;
    size_t bcap;
    uint64_t cat_bytes[NCAT], cat_count[NCAT];
    bigfile *heap;
    int heap_n;
} tctx;

static int opt_files = 15;

static void heap_push(tctx *t, uint64_t size, const char *dir, const char *name) {
    bigfile *h = t->heap;
    if (t->heap_n == opt_files) {
        if (size <= h[0].size) return;
        free(h[0].path);
        h[0] = (bigfile){size, join(dir, name)};
        for (int i = 0;;) {   // sift down
            int l = 2 * i + 1, r = l + 1, m = i;
            if (l < t->heap_n && h[l].size < h[m].size) m = l;
            if (r < t->heap_n && h[r].size < h[m].size) m = r;
            if (m == i) break;
            bigfile tmp = h[i]; h[i] = h[m]; h[m] = tmp; i = m;
        }
        return;
    }
    int i = t->heap_n++;
    h[i] = (bigfile){size, join(dir, name)};
    while (i && h[(i - 1) / 2].size > h[i].size) {   // sift up
        bigfile tmp = h[i]; h[i] = h[(i - 1) / 2]; h[(i - 1) / 2] = tmp;
        i = (i - 1) / 2;
    }
}

/* ---------- scanner ---------- */

struct linux_dirent64 {
    uint64_t d_ino;
    int64_t d_off;
    unsigned short d_reclen;
    unsigned char d_type;
    char d_name[];
};

static void scan_dir(work *w, tctx *t) {
    char *buf = t->buf;
    work **batch = &t->batch;
    size_t *bcap = &t->bcap;
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
            if (S_ISREG(st.st_mode)) {
                int c = classify(nm);
                t->cat_bytes[c] += sz;
                t->cat_count[c]++;
                if (opt_files && (t->heap_n < opt_files || sz > t->heap[0].size))
                    heap_push(t, sz, w->path, nm);
            }
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
    tctx *t = arg;
    t->buf = malloc(DENTS_BUF);
    t->bcap = 256;
    t->batch = malloc(t->bcap * sizeof *t->batch);
    t->heap = calloc(opt_files ? opt_files : 1, sizeof *t->heap);
    work w;
    while (wq_pop(&w)) {
        scan_dir(&w, t);
        free(w.path);
        wq_finish_one();
    }
    free(t->batch);
    free(t->buf);
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
    if (mount) printf("  %s[other filesystem · include with -c]%s", C("2"), C("0"));
    if (bad) printf("  %s! read errors%s", C("33"), C("0"));
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
                print_row(sz, whole, prefix, branch, "(files in this dir)", 0, 0, 0, NULL);
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
        snprintf(note, sizeof note, "%d more", hidden);
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

/* ---------- banner ---------- */

static const char *banner[] = {
    "      ___                       ___           ___                       ___           ___",
    "     /\\  \\          ___        /\\  \\         /\\  \\          ___        /\\  \\         /\\  \\",
    "    /::\\  \\        /\\  \\      /::\\  \\       /::\\  \\        /\\  \\       \\:\\  \\       /::\\  \\",
    "   /:/\\:\\  \\       \\:\\  \\    /:/\\:\\  \\     /:/\\ \\  \\       \\:\\  \\       \\:\\  \\     /:/\\:\\  \\",
    "  /:/  \\:\\__\\      /::\\__\\  /::\\~\\:\\  \\   _\\:\\~\\ \\  \\      /::\\__\\       \\:\\  \\   /::\\~\\:\\  \\",
    " /:/__/ \\:|__|  __/:/\\/__/ /:/\\:\\ \\:\\__\\ /\\ \\:\\ \\ \\__\\  __/:/\\/__/ _______\\:\\__\\ /:/\\:\\ \\:\\__\\",
    " \\:\\  \\ /:/  / /\\/:/  /    \\/_|::\\/:/  / \\:\\ \\:\\ \\/__/ /\\/:/  /    \\::::::::/__/ \\:\\~\\:\\ \\/__/",
    "  \\:\\  /:/  /  \\::/__/        |:|::/  /   \\:\\ \\:\\__\\   \\::/__/      \\:\\~~\\~~      \\:\\ \\:\\__\\",
    "   \\:\\/:/  /    \\:\\__\\        |:|\\/__/     \\:\\/:/  /    \\:\\__\\       \\:\\  \\        \\:\\ \\/__/",
    "    \\::/__/      \\/__/        |:|  |        \\::/  /      \\/__/        \\:\\__\\        \\:\\__\\",
    "     ~~                        \\|__|         \\/__/                     \\/__/         \\/__/",
};

static int term_width(void) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col) return ws.ws_col;
    return 80;
}

static void print_banner(void) {
    // cyan -> blue -> violet -> pink, one 256-color step per line
    static const int grad[] = {51, 45, 39, 33, 27, 63, 99, 135, 171, 207, 201};
    int n = sizeof banner / sizeof *banner;
    putchar('\n');
    for (int i = 0; i < n; i++) {
        if (use_color) printf("\033[1;38;5;%dm%s\033[0m\n", grad[i % 11], banner[i]);
        else puts(banner[i]);
    }
    printf("%s%*s%s\n\n", C("2"), 94, "v" VERSION " · fast parallel disk usage", C("0"));
}

/* ---------- generic helpers ---------- */

static void bar(double frac, int width, const char *color) {
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    int fill = (int)(frac * width + 0.5);
    if (frac > 0 && fill == 0) fill = 1;
    printf("%s", use_color ? color : "");
    for (int i = 0; i < width; i++) {
        if (i == fill && use_color) printf("\033[0;2m");
        fputs(i < fill ? "█" : "░", stdout);
    }
    printf("%s", C("0"));
}

// Print s left-aligned in `width` terminal columns (Hangul counts as 2 columns).
static void label(const char *s, int width) {
    int w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) w += (*p >= 0xE0) ? 2 : 1;
    printf("%s%*s", s, width > w ? width - w : 0, "");
}

static const char *usage_color(double frac) {
    return frac >= 0.9 ? "\033[31m" : frac >= 0.75 ? "\033[33m" : "\033[32m";
}

static void section(const char *title) {
    printf("%s▌ %s%s\n", C("1;35"), title, C("0"));
}

static int read_line(const char *path, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(out, (int)n, f)) { fclose(f); return -1; }
    fclose(f);
    size_t l = strlen(out);
    while (l && (out[l - 1] == '\n' || out[l - 1] == ' ')) out[--l] = 0;
    return 0;
}

/* ---------- physical disks + partitions (from /sys/block and mountinfo) ---------- */

typedef struct { dev_t dev; char mnt[512]; char fs[32]; } mountent;
static mountent *mounts;
static int nmounts;

static void load_mounts(void) {
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        unsigned maj, min;
        char m[512], fs[32];
        if (sscanf(line, "%*d %*d %u:%u %*s %511s", &maj, &min, m) != 3) continue;
        char *dash = strstr(line, " - ");
        if (!dash || sscanf(dash, " - %31s", fs) != 1) continue;
        dev_t d = makedev(maj, min);
        int found = -1;
        for (int i = 0; i < nmounts; i++) if (mounts[i].dev == d) found = i;
        if (found >= 0) {   // keep the shortest mount point per device
            if (strlen(m) < strlen(mounts[found].mnt)) snprintf(mounts[found].mnt, sizeof mounts->mnt, "%s", m);
            continue;
        }
        mounts = realloc(mounts, (nmounts + 1) * sizeof *mounts);
        mounts[nmounts].dev = d;
        snprintf(mounts[nmounts].mnt, sizeof mounts->mnt, "%s", m);
        snprintf(mounts[nmounts].fs, sizeof mounts->fs, "%s", fs);
        nmounts++;
    }
    fclose(f);
}

static mountent *mount_for_sysdir(const char *sysdir) {
    char p[1024], v[64];
    unsigned maj, min;
    snprintf(p, sizeof p, "%s/dev", sysdir);
    if (read_line(p, v, sizeof v) == 0 && sscanf(v, "%u:%u", &maj, &min) == 2)
        for (int i = 0; i < nmounts; i++) if (mounts[i].dev == makedev(maj, min)) return &mounts[i];
    // LUKS / LVM: the mounted device is a holder (dm-N) of this partition
    snprintf(p, sizeof p, "%s/holders", sysdir);
    DIR *d = opendir(p);
    if (!d) return NULL;
    struct dirent *e;
    mountent *r = NULL;
    while (!r && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char hp[1100];
        snprintf(hp, sizeof hp, "/sys/class/block/%s", e->d_name);
        r = mount_for_sysdir(hp);
    }
    closedir(d);
    return r;
}

static void print_part(const char *name, const char *sysdir, int last) {
    char p[1024], v[64];
    snprintf(p, sizeof p, "%s/size", sysdir);
    uint64_t sectors = read_line(p, v, sizeof v) == 0 ? strtoull(v, NULL, 10) : 0;
    char hs[16];
    mountent *m = mount_for_sysdir(sysdir);
    printf("  %s%s%s %-12s ", C("2"), last ? "└─" : "├─", C("0"), name);
    struct statvfs vfs;
    if (m && statvfs(m->mnt, &vfs) == 0 && vfs.f_blocks) {
        uint64_t sz = (uint64_t)vfs.f_blocks * vfs.f_frsize;
        uint64_t used = sz - (uint64_t)vfs.f_bfree * vfs.f_frsize;
        uint64_t avail = (uint64_t)vfs.f_bavail * vfs.f_frsize;
        double frac = (used + avail) ? (double)used / (double)(used + avail) : 0;
        char us[16], ss[16], as[16];
        human(used, us, sizeof us); human(sz, ss, sizeof ss); human(avail, as, sizeof as);
        printf("%-6s %s%-14s%s %6s / %-6s ", m->fs, C("1;34"), m->mnt, C("0"), us, ss);
        bar(frac, 20, usage_color(frac));
        printf(" %3.0f%%  %sfree %s%s\n", ceil(frac * 100), C("2"), as, C("0"));
    } else {
        human(sectors * 512, hs, sizeof hs);
        printf("%s%-6s %-14s %6s%s\n", C("2"), "-", m ? m->mnt : "(not mounted)", hs, C("0"));
    }
}

static int is_partition(const char *disk, const char *ent) {
    char p[1024];
    struct stat st;
    snprintf(p, sizeof p, "/sys/block/%s/%s/partition", disk, ent);
    return stat(p, &st) == 0;
}

/* ---------- NVMe health: temperature (hwmon, no root) + SMART log (needs root) ---------- */

static void human_si(double v, char *out, size_t n) {   // decimal units, as drive vendors rate TBW
    const char *u[] = {"B", "KB", "MB", "GB", "TB", "PB", "EB"};
    int i = 0;
    while (v >= 1000 && i < 6) { v /= 1000; i++; }
    snprintf(out, n, i == 0 ? "%.0f %s" : "%.1f %s", v, u[i]);
}

static double le128(const uint8_t *p) {   // 128-bit little-endian counter -> double
    double v = 0;
    for (int i = 15; i >= 0; i--) v = v * 256 + p[i];
    return v;
}

static void print_nvme_health(const char *disk) {
    const char *pad = "             ";
    char pat[512], v[64];

    // temperatures: temp1 = composite, others = individual sensors (millidegrees C)
    snprintf(pat, sizeof pat, "/sys/block/%s/device/hwmon*/temp*_input", disk);
    glob_t g;
    int have_temp = 0;
    if (glob(pat, 0, NULL, &g) == 0) {
        double comp = -1000, warn = 0;
        char sensors[256] = "";
        for (size_t i = 0; i < g.gl_pathc; i++) {
            if (read_line(g.gl_pathv[i], v, sizeof v) != 0) continue;
            double c = atof(v) / 1000;
            if (strstr(g.gl_pathv[i], "/temp1_input")) {
                comp = c;
                char mp[600];
                snprintf(mp, sizeof mp, "%.*s_max", (int)(strlen(g.gl_pathv[i]) - 6), g.gl_pathv[i]);
                if (read_line(mp, v, sizeof v) == 0) warn = atof(v) / 1000;
            } else {
                size_t l = strlen(sensors);
                snprintf(sensors + l, sizeof sensors - l, "%s%.0f", l ? "/" : "", c);
            }
        }
        globfree(&g);
        if (comp > -1000) {
            have_temp = 1;
            double lim = warn > 0 ? warn : 85;
            const char *col = comp >= lim ? "\033[1;31m" : comp >= lim - 15 ? "\033[33m" : "\033[32m";
            printf("%s%sTemp%s     %s%.0f°C%s  ", pad, C("1"), C("0"), use_color ? col : "", comp, C("0"));
            bar(comp / lim, 10, col);
            if (warn > 0) printf("  %swarn %.0f°C%s", C("2"), warn, C("0"));
            if (sensors[0]) printf("  %s· sensors %s°C%s", C("2"), sensors, C("0"));
            putchar('\n');
        }
    }
    (void)have_temp;

    // controller char device: /sys/block/nvme0n1/device -> .../nvme0
    snprintf(pat, sizeof pat, "/sys/block/%s/device", disk);
    char *ctrl = realpath(pat, NULL);
    if (!ctrl) return;
    char dev[300];
    snprintf(dev, sizeof dev, "/dev/%s", strrchr(ctrl, '/') + 1);
    free(ctrl);

    uint8_t log[512];
    memset(log, 0, sizeof log);
    struct nvme_admin_cmd cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.opcode = 0x02;                       // Get Log Page
    cmd.nsid = 0xffffffff;
    cmd.addr = (uint64_t)(uintptr_t)log;
    cmd.data_len = sizeof log;
    cmd.cdw10 = ((sizeof log / 4 - 1) << 16) | 0x02;   // NUMDL | LID 02h = SMART / Health
    int fd = open(dev, O_RDONLY | O_CLOEXEC);
    int ok = fd >= 0 && ioctl(fd, NVME_IOCTL_ADMIN_CMD, &cmd) == 0;
    int err = errno;
    if (fd >= 0) close(fd);

    if (!ok) {
        // without root we can still show what was written since boot
        snprintf(pat, sizeof pat, "/sys/block/%s/stat", disk);
        char st[512];
        unsigned long long f[8] = {0};
        if (read_line(pat, st, sizeof st) == 0 &&
            sscanf(st, "%llu %llu %llu %llu %llu %llu %llu", &f[0], &f[1], &f[2], &f[3], &f[4], &f[5], &f[6]) == 7) {
            char w[32], r[32];
            human_si(f[6] * 512.0, w, sizeof w);
            human_si(f[2] * 512.0, r, sizeof r);
            printf("%s%sWear%s     %s? (%s)%s  · since boot: %s written, %s read\n", pad, C("1"), C("0"),
                   C("2"), err == EACCES || err == EPERM ? "run with sudo for wear % / TBW" : strerror(err),
                   C("0"), w, r);
        }
        return;
    }

    int warnbits = log[0];
    int spare = log[3], spare_thr = log[4], used = log[5];
    double units_read = le128(log + 32), units_written = le128(log + 48);
    double hours = le128(log + 128), cycles = le128(log + 112);
    double unsafe = le128(log + 144), media_err = le128(log + 160);
    double written = units_written * 512000.0, readb = units_read * 512000.0;

    char w[32], r[32];
    human_si(written, w, sizeof w);
    human_si(readb, r, sizeof r);
    const char *col = used >= 90 ? "\033[1;31m" : used >= 70 ? "\033[33m" : "\033[32m";
    printf("%s%sWear%s     %s%d%% used%s  ", pad, C("1"), C("0"), use_color ? col : "", used, C("0"));
    bar(used / 100.0, 10, col);
    printf("  %s· spare %d%% (min %d%%)%s\n", C("2"), spare, spare_thr, C("0"));

    printf("%s%sTBW%s      %s%s written%s  %s· %s read", pad, C("1"), C("0"), C("1"), w, C("0"), C("2"), r);
    if (used > 0) {
        char e[32];
        human_si(written / (used / 100.0), e, sizeof e);
        printf(" · ~%s endurance at this wear rate", e);
    }
    printf("%s\n", C("0"));

    printf("%s%sHistory%s  %s%.0f h powered on (%.1f years) · %.0f power cycles · %.0f unsafe shutdowns%s\n",
           pad, C("1"), C("0"), C("2"), hours, hours / 8766, cycles, unsafe, C("0"));
    if (warnbits || media_err)
        printf("%s%sStatus%s   %scritical warning 0x%02x · %.0f media errors%s\n", pad, C("1"), C("0"),
               use_color ? "\033[1;31m" : "", warnbits, media_err, C("0"));
    else
        printf("%s%sStatus%s   %sOK · no critical warnings · 0 media errors%s\n", pad, C("1"), C("0"),
               use_color ? "\033[32m" : "", C("0"));
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static void print_disks(void) {
    load_mounts();
    DIR *d = opendir("/sys/block");
    if (!d) return;
    section("Storage devices");
    struct dirent *e;
    char **names = NULL;
    int n = 0;
    while ((e = readdir(d))) {
        const char *nm = e->d_name;
        if (nm[0] == '.' || !strncmp(nm, "loop", 4) || !strncmp(nm, "ram", 3) ||
            !strncmp(nm, "zram", 4) || !strncmp(nm, "dm-", 3) || !strncmp(nm, "sr", 2) ||
            !strncmp(nm, "md", 2) || !strncmp(nm, "nbd", 3))
            continue;
        names = realloc(names, (n + 1) * sizeof *names);
        names[n++] = strdup(nm);
    }
    closedir(d);
    qsort(names, n, sizeof *names, cmp_str);
    for (int i = 0; i < n; i++) {
        char p[1024], model[256] = "", v[64];
        snprintf(p, sizeof p, "/sys/block/%s/size", names[i]);
        uint64_t bytes = read_line(p, v, sizeof v) == 0 ? strtoull(v, NULL, 10) * 512 : 0;
        if (!bytes) continue;
        snprintf(p, sizeof p, "/sys/block/%s/device/model", names[i]);
        read_line(p, model, sizeof model);
        snprintf(p, sizeof p, "/sys/block/%s/queue/rotational", names[i]);
        int rot = read_line(p, v, sizeof v) == 0 && v[0] == '1';
        snprintf(p, sizeof p, "/sys/block/%s/removable", names[i]);
        int rem = read_line(p, v, sizeof v) == 0 && v[0] == '1';
        const char *kind = !strncmp(names[i], "nvme", 4) ? "NVMe SSD" : rot ? "HDD" : "SSD";
        char hs[16];
        human(bytes, hs, sizeof hs);
        printf("  %s%s%s  %s%s%s · %s%s · %s\n", C("1"), names[i], C("0"), C("36"),
               model[0] ? model : "unknown", C("0"), kind, rem ? " (removable)" : "", hs);
        if (!strncmp(names[i], "nvme", 4)) print_nvme_health(names[i]);

        // partitions, or the whole disk if it is used without a partition table
        char sysdir[1024];
        snprintf(sysdir, sizeof sysdir, "/sys/block/%s", names[i]);
        DIR *pd = opendir(sysdir);
        char **parts = NULL;
        int np = 0;
        while (pd && (e = readdir(pd))) {
            if (e->d_name[0] == '.' || !is_partition(names[i], e->d_name)) continue;
            parts = realloc(parts, (np + 1) * sizeof *parts);
            parts[np++] = strdup(e->d_name);
        }
        if (pd) closedir(pd);
        qsort(parts, np, sizeof *parts, cmp_str);
        if (!np) print_part(names[i], sysdir, 1);
        for (int j = 0; j < np; j++) {
            char pdir[2048];
            snprintf(pdir, sizeof pdir, "%s/%s", sysdir, parts[j]);
            print_part(parts[j], pdir, j == np - 1);
            free(parts[j]);
        }
        free(parts);
        free(names[i]);
    }
    free(names);
    putchar('\n');
}

/* ---------- where does "df Used" go ---------- */

static void print_breakdown(const char *root, const char *src, const char *mnt,
                            uint64_t scanned, double secs) {
    struct statvfs vfs;
    if (statvfs(root, &vfs) != 0) return;
    uint64_t fsz = (uint64_t)vfs.f_blocks * vfs.f_frsize;
    uint64_t favail = (uint64_t)vfs.f_bavail * vfs.f_frsize;
    uint64_t ffree = (uint64_t)vfs.f_bfree * vfs.f_frsize;
    uint64_t fused = fsz - ffree;
    uint64_t reserved = ffree - favail;
    char a[16], b[16], d[16], s[16], r[16];
    human(fsz, a, sizeof a); human(fused, b, sizeof b); human(favail, d, sizeof d);
    human(scanned, s, sizeof s); human(reserved, r, sizeof r);
    double frac = (fused + favail) ? (double)fused / (double)(fused + favail) : 0;

    char title[4096];
    snprintf(title, sizeof title, "%s usage  (%s → %s)", root, src, mnt);
    section(title);
    printf("  Size %s%s%s   Used %s%s%s   Avail %s%s%s   Use%% %s%.0f%%%s\n  ",
           C("1"), a, C("0"), C("1"), b, C("0"), C("1"), d, C("0"),
           use_color ? usage_color(frac) : "", ceil(frac * 100), C("0"));
    bar(frac, 50, usage_color(frac));
    printf("\n\n");

    int whole_fs = !opt_apparent && !opt_cross && strcmp(root, mnt) == 0 && fused;
    if (whole_fs) {
        uint64_t outside = fused > scanned ? fused - scanned : 0;
        char o[16];
        human(outside, o, sizeof o);
        printf("  "); label("Scanned files/dirs", 20);
        printf(" %7s  %5.1f%%  ", s, 100.0 * scanned / fused);
        bar((double)scanned / fused, 20, "\033[36m");
        printf("\n  "); label("Not scanned", 20);
        printf(" %7s  %5.1f%%  ", o, 100.0 * outside / fused);
        bar((double)outside / fused, 20, "\033[33m");
        printf("  %spermission denied · FS metadata · deleted-but-open files%s\n", C("2"), C("0"));
        if (reserved) {
            printf("  "); label("Reserved", 20);
            printf(" %7s          %sroot-only reserved blocks, not in Used/Avail%s\n", r, C("2"), C("0"));
        }
    } else {
        printf("  "); label("Scanned total", 20);
        printf(" %7s\n", s);
    }
    if (vfs.f_files) {
        uint64_t iu = vfs.f_files - vfs.f_ffree;
        printf("  "); label("Inodes used", 20);
        printf(" %6.1fM / %.1fM  (%.1f%%)\n", iu / 1e6, vfs.f_files / 1e6, 100.0 * iu / vfs.f_files);
    }
    printf("  %s%.2fs · %llu dirs · %llu files · %d threads%s%s\n\n", C("2"), secs,
           (unsigned long long)st_dirs, (unsigned long long)st_files, opt_threads,
           opt_apparent ? " · apparent size" : "", C("0"));
}

/* ---------- file types + largest files ---------- */

static void print_types(tctx *ctx, int nt) {
    uint64_t bytes[NCAT] = {0}, count[NCAT] = {0}, total = 0;
    for (int i = 0; i < nt; i++)
        for (int c = 0; c < NCAT; c++) { bytes[c] += ctx[i].cat_bytes[c]; count[c] += ctx[i].cat_count[c]; }
    for (int c = 0; c < NCAT; c++) total += bytes[c];
    if (!total) return;
    int order[NCAT];
    for (int c = 0; c < NCAT; c++) order[c] = c;
    for (int i = 1; i < NCAT; i++)   // insertion sort by bytes desc
        for (int j = i; j > 0 && bytes[order[j]] > bytes[order[j - 1]]; j--) {
            int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
        }
    section("By file type");
    for (int i = 0; i < NCAT; i++) {
        int c = order[i];
        if (!count[c]) continue;
        char hs[16], cnt[32];
        human(bytes[c], hs, sizeof hs);
        snprintf(cnt, sizeof cnt, "%llu files", (unsigned long long)count[c]);
        printf("  "); label(cat_names[c], 26);
        printf(" %s%7s%s %5.1f%%  ", size_color(bytes[c]), hs, C("0"), 100.0 * bytes[c] / total);
        bar((double)bytes[c] / total, 20, "\033[35m");
        printf(" %14s  %s%s%s\n", cnt, C("2"), cat_hint[c], C("0"));
    }
    putchar('\n');
}

static int cmp_big(const void *a, const void *b) {
    uint64_t x = ((const bigfile *)a)->size, y = ((const bigfile *)b)->size;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void print_bigfiles(tctx *ctx, int nt) {
    if (!opt_files) return;
    int n = 0;
    for (int i = 0; i < nt; i++) n += ctx[i].heap_n;
    if (!n) return;
    bigfile *all = malloc(n * sizeof *all);
    n = 0;
    for (int i = 0; i < nt; i++)
        for (int j = 0; j < ctx[i].heap_n; j++) all[n++] = ctx[i].heap[j];
    qsort(all, n, sizeof *all, cmp_big);
    if (n > opt_files) n = opt_files;
    char title[64];
    snprintf(title, sizeof title, "Largest %d files", n);
    section(title);
    for (int i = 0; i < n; i++) {
        char hs[16];
        human(all[i].size, hs, sizeof hs);
        printf("  %s%7s%s  ", size_color(all[i].size), hs, C("0"));
        const char *slash = strrchr(all[i].path, '/');
        size_t dl = slash ? (size_t)(slash - all[i].path + 1) : 0;
        printf("%s", C("2"));
        for (size_t k = 0; k < dl; k++) {
            unsigned char ch = all[i].path[k];
            if (ch < 0x20 || ch == 0x7f) printf("\\x%02x", ch); else putchar(ch);
        }
        printf("%s%s", C("0"), C("1"));
        safe_print(all[i].path + dl);
        printf("%s\n", C("0"));
    }
    free(all);
    putchar('\n');
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
        "Usage: dirsize [options] [path]   (default path: /)\n"
        "\n"
        "Scans the path in parallel and prints a size-sorted directory tree,\n"
        "plus storage devices, NVMe health, usage by file type and the largest files.\n"
        "Stays on the starting filesystem by default (matches one line of df -h).\n"
        "\n"
        "  -d, --depth N      tree depth to display (default 2)\n"
        "  -n, --top N        show the N largest entries per dir, 0 = all (default 10)\n"
        "  -m, --min SIZE     hide entries smaller than SIZE (e.g. 100M, 1G)\n"
        "  -j, --threads N    worker threads (default: 2 x CPUs, max 64)\n"
        "  -c, --cross        cross into other filesystems (mount points)\n"
        "  -f, --files N      show the N largest files, 0 = off (default 15)\n"
        "  -s, --simple       usage + tree only (no banner/devices/types/largest files)\n"
        "  -A, --apparent     apparent file sizes instead of allocated space\n"
        "  -q, --quiet        no progress line\n"
        "      --no-color     disable colors (NO_COLOR is honored too)\n"
        "      --no-banner    hide the banner\n"
        "  -h, --help         show this help\n"
        "  -V, --version      show version\n"
        "\n"
        "Unreadable dirs are skipped. For full totals and NVMe wear/TBW: sudo dirsize\n");
}

static volatile sig_atomic_t interrupted;
static void on_sigint(int s) { (void)s; interrupted = 1; }

int main(int argc, char **argv) {
    int no_color = 0, no_banner = 0, simple = 0;
    static struct option lo[] = {
        {"depth", 1, 0, 'd'}, {"top", 1, 0, 'n'}, {"min", 1, 0, 'm'},
        {"threads", 1, 0, 'j'}, {"cross", 0, 0, 'c'}, {"apparent", 0, 0, 'A'},
        {"quiet", 0, 0, 'q'}, {"no-color", 0, 0, 1}, {"help", 0, 0, 'h'},
        {"files", 1, 0, 'f'}, {"simple", 0, 0, 's'}, {"no-banner", 0, 0, 2},
        {"version", 0, 0, 'V'}, {0, 0, 0, 0}};
    int c;
    while ((c = getopt_long(argc, argv, "d:n:m:j:f:scAqhV", lo, NULL)) != -1) {
        switch (c) {
        case 'd': opt_depth = atoi(optarg); break;
        case 'n': opt_top = atoi(optarg); break;
        case 'm': opt_min = parse_size(optarg); break;
        case 'j': opt_threads = atoi(optarg); break;
        case 'c': opt_cross = 1; break;
        case 'A': opt_apparent = 1; break;
        case 'q': opt_progress = 0; break;
        case 1: no_color = 1; break;
        case 2: no_banner = 1; break;
        case 'f': opt_files = atoi(optarg); break;
        case 's': simple = 1; break;
        case 'h': usage(stdout); return 0;
        case 'V': puts("dirsize " VERSION); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (opt_depth < 0 || opt_top < 0 || opt_files < 0) { usage(stderr); return 2; }
    if (argc - optind > 1) { usage(stderr); return 2; }

    char *root = realpath(optind < argc ? argv[optind] : "/", NULL);
    if (!root) { perror(optind < argc ? argv[optind] : "/"); return 2; }
    struct stat rst;
    if (stat(root, &rst) < 0 || !S_ISDIR(rst.st_mode)) {
        fprintf(stderr, "not a directory: %s\n", root);
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
    if (simple) opt_files = 0;
    ext_init();

    signal(SIGINT, on_sigint);
    clock_gettime(CLOCK_MONOTONIC, &t0);

    node *top = node_new(root, NULL, 0);
    top->self = size_of(&rst);
    work first = {strdup(root), top, 0};
    wq_push_many(&first, 1);

    pthread_t *th = malloc(opt_threads * sizeof *th);
    tctx *ctx = calloc(opt_threads, sizeof *ctx);
    for (int i = 0; i < opt_threads; i++) pthread_create(&th[i], NULL, worker, &ctx[i]);

    if (opt_progress) {
        for (;;) {
            pthread_mutex_lock(&wq_mu);
            int done = wq_done;
            pthread_mutex_unlock(&wq_mu);
            if (done || interrupted) break;
            char hs[16];
            human(atomic_load(&st_bytes), hs, sizeof hs);
            fprintf(stderr, "\r\033[Kscanning… %s · %llu dirs · %llu files · %.1fs",
                    hs, (unsigned long long)st_dirs, (unsigned long long)st_files, elapsed());
            struct timespec ts = {0, 100 * 1000 * 1000};
            nanosleep(&ts, NULL);
        }
        fputs("\r\033[K", stderr);
    }
    if (interrupted) {
        fputs("\ninterrupted\n", stderr);
        _exit(130);
    }
    for (int i = 0; i < opt_threads; i++) pthread_join(th[i], NULL);
    if (interrupted) { fputs("\ninterrupted\n", stderr); return 130; }
    double secs = elapsed();

    compute_totals(top);

    char src[1024], mnt[2048];
    find_mount(root, src, sizeof src, mnt, sizeof mnt);
    if (!simple) {
        if (!no_banner && isatty(STDOUT_FILENO) && term_width() >= 96) print_banner();
        print_disks();
    }
    print_breakdown(root, src, mnt, top->total, secs);

    char title[64];
    snprintf(title, sizeof title, "Directory tree (depth %d · top %d per dir)", opt_depth, opt_top);
    section(title);
    char prefix[4096] = "";
    print_row(top->total, top->total, "", "", root, 1, top->errors > 0, 0, NULL);
    print_tree(top, top->total, prefix);
    putchar('\n');

    if (!simple) {
        print_types(ctx, opt_threads);
        print_bigfiles(ctx, opt_threads);
    }
    fflush(stdout);

    if (st_errors) {
        fprintf(stderr, "\nwarning: %llu read errors (totals may be low)%s\n",
                (unsigned long long)st_errors, geteuid() ? " — run with sudo for full totals" : "");
        int n = err_n < ERR_SAMPLES ? err_n : ERR_SAMPLES;
        for (int i = 0; i < n; i++) if (err_samples[i]) fprintf(stderr, "  %s\n", err_samples[i]);
        if (err_n > ERR_SAMPLES) fprintf(stderr, "  … %d more\n", err_n - ERR_SAMPLES);
    }
    if (st_mounts && !opt_cross)
        fprintf(stderr, "skipped %llu other filesystems (include with -c)\n",
                (unsigned long long)st_mounts);
    return st_errors ? 1 : 0;
}
