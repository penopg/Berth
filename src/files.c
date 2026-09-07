#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "files.h"

static const double SCAN_EVERY = 30.0;   // секунд между обходами папки
static const int    SCAN_LIMIT = 6000;   // записей каталога за обход
static const int    SCAN_DEPTH = 6;

static const char *base_of(const char *rel);

static void files_path(const char *cwd, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", cwd, FILES_FILE);
}

static time_t file_mtime(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

static void rtrim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\n'
                     || s[n - 1] == '\r'))
        s[--n] = '\0';
}

static void shown_path(const char *cwd, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", cwd, FILES_SHOWN_FILE);
}

static void shown_load(FileList *fl, const char *cwd)
{
    fl->shown_count = 0;
    char path[700];
    shown_path(cwd, path, sizeof(path));
    fl->shown_mtime = file_mtime(path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[FILE_PATH_MAX + 64];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = '\0';
        if (strcmp(line, "show")) continue;
        if (fl->shown_count >= FILES_SHOWN_MAX) break;
        char *p = tab + 1;
        if (!strncmp(p, "./", 2)) p += 2;
        snprintf(fl->shown[fl->shown_count], FILE_PATH_MAX, "%s", p);
        rtrim(fl->shown[fl->shown_count]);
        if (fl->shown[fl->shown_count][0]) fl->shown_count++;
    }
    fclose(f);
}

bool files_is_table(const char *rel)
{
    const char *dot = strrchr(base_of(rel), '.');
    return dot && !strcasecmp(dot, ".tsv");
}

bool files_shown(const FileList *fl, const char *rel)
{
    for (int i = 0; i < fl->shown_count; i++)
        if (!strcmp(fl->shown[i], rel)) return true;
    return false;
}

bool files_show_toggle(FileList *fl, const char *cwd, const char *rel)
{
    if (!cwd || !*cwd || !rel || !*rel) return false;

    int at = -1;
    for (int i = 0; i < fl->shown_count; i++)
        if (!strcmp(fl->shown[i], rel)) at = i;
    if (at >= 0) {
        for (int i = at; i + 1 < fl->shown_count; i++)
            memcpy(fl->shown[i], fl->shown[i + 1], FILE_PATH_MAX);
        fl->shown_count--;
    } else {
        if (fl->shown_count >= FILES_SHOWN_MAX) return false;
        snprintf(fl->shown[fl->shown_count++], FILE_PATH_MAX, "%s", rel);
    }

    char dir[700], path[700], tmp[720];
    snprintf(dir, sizeof(dir), "%s/.berth", cwd);
    mkdir(dir, 0755);
    shown_path(cwd, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fprintf(f, "# Таблицы, показанные на странице проекта. Ставит берт "
               "тумблером у документа.\n");
    for (int i = 0; i < fl->shown_count; i++)
        fprintf(f, "show\t%s\n", fl->shown[i]);
    fclose(f);
    if (rename(tmp, path) != 0) { remove(tmp); return false; }
    fl->shown_mtime = file_mtime(path);
    return true;
}

void files_load(FileList *fl, const char *cwd)
{
    memset(fl, 0, sizeof(*fl));
    fl->undescribed = -1;
    if (!cwd || !*cwd) return;

    char path[700];
    files_path(cwd, path, sizeof(path));
    shown_load(fl, cwd);
    FILE *f = fopen(path, "r");
    if (!f) { files_refresh(fl, cwd); return; }   // реестра нет — но папку смотрим
    fl->exists = true;
    fl->mtime = file_mtime(path);

    char line[FILE_PATH_MAX + FILE_NOTE_MAX + 64];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        if (fl->count >= FILES_MAX) { fl->partial = true; break; }
        *tab = '\0';
        FileEntry *e = &fl->items[fl->count++];
        memset(e, 0, sizeof(*e));
        const char *p = line;
        if (!strncmp(p, "./", 2)) p += 2;
        snprintf(e->path, sizeof(e->path), "%s", p);
        rtrim(e->path);
        snprintf(e->note, sizeof(e->note), "%s", tab + 1);
        rtrim(e->note);
    }
    fclose(f);
    files_refresh(fl, cwd);
}

bool files_changed(const FileList *fl, const char *cwd)
{
    if (!cwd || !*cwd) return false;
    char path[700];
    files_path(cwd, path, sizeof(path));
    if (file_mtime(path) != fl->mtime) return true;
    shown_path(cwd, path, sizeof(path));
    return file_mtime(path) != fl->shown_mtime;
}

// --- что считается документом ----------------------------------------------------

static const char *const DOC_EXT[] = {
    ".md", ".txt", ".tsv", ".csv", ".pdf", ".png", ".jpg", ".jpeg", ".gif",
    ".docx", ".xlsx", ".pptx", ".key", ".numbers", ".pages", ".rtf",
};

static const char *const SKIP_DIRS[] = {
    "node_modules", "build", "dist", "vendor", "target", "__pycache__",
};

static const char *base_of(const char *rel)
{
    const char *b = strrchr(rel, '/');
    return b ? b + 1 : rel;
}

bool files_is_document(const char *rel)
{
    const char *base = base_of(rel);
    const char *dot = strrchr(base, '.');
    if (!dot) return false;
    bool doc = false;
    for (size_t i = 0; i < sizeof(DOC_EXT) / sizeof(*DOC_EXT); i++)
        if (!strcasecmp(dot, DOC_EXT[i])) { doc = true; break; }
    if (!doc) return false;
    // Паспорт, README и лицензию и так знают; служебные файлы берта — на
    // своих страницах. Внутри .berth документами считаются только таблицы.
    if (!strcasecmp(base, "CLAUDE.md") || !strncasecmp(base, "README", 6)
        || !strncasecmp(base, "LICENSE", 7) || !strncasecmp(base, "COPYING", 7)
        || !strcasecmp(base, "OFL.txt") || !strcasecmp(base, "CMakeLists.txt")
        || !strncasecmp(base, "requirements", 12))
        return false;
    if (!strncmp(rel, ".berth/", 7))
        return !strncmp(rel, ".berth/data/", 12) && !strcasecmp(dot, ".tsv");
    return true;
}

static bool listed(const FileList *fl, const char *rel)
{
    for (int i = 0; i < fl->count; i++)
        if (!strcmp(fl->items[i].path, rel)) return true;
    return false;
}

typedef struct {
    FileList *fl;
    int   budget;
    int   found;
    bool  cut;
} Scan;

static void scan_dir(Scan *sc, const char *root, const char *rel, int depth)
{
    if (depth > SCAN_DEPTH) return;
    char abs[1024];
    snprintf(abs, sizeof(abs), "%s%s%s", root, *rel ? "/" : "", rel);
    DIR *d = opendir(abs);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d))) {
        if (--sc->budget <= 0) { sc->cut = true; break; }
        const char *n = de->d_name;
        if (n[0] == '.') {
            // Скрытое пропускаем, кроме .berth в корне — ради .berth/data.
            if (depth != 0 || strcmp(n, ".berth")) continue;
        }
        char sub[FILE_PATH_MAX];
        if (snprintf(sub, sizeof(sub), "%s%s%s", rel, *rel ? "/" : "", n) >= (int)sizeof(sub))
            continue;
        if (de->d_type == DT_DIR) {
            bool skip = false;
            for (size_t i = 0; i < sizeof(SKIP_DIRS) / sizeof(*SKIP_DIRS); i++)
                if (!strcmp(n, SKIP_DIRS[i])) { skip = true; break; }
            // Внутри .berth интересна только data.
            if (!strcmp(rel, ".berth") && strcmp(n, "data")) skip = true;
            // Вложенный репозиторий — чужой код (референс, сабмодуль):
            // его документы не наши.
            if (!skip && depth >= 0) {
                char git[1100];
                snprintf(git, sizeof(git), "%s/%s/.git", abs, n);
                struct stat gs;
                if (stat(git, &gs) == 0) skip = true;
            }
            if (!skip) scan_dir(sc, root, sub, depth + 1);
        } else if (de->d_type == DT_REG) {
            if (files_is_document(sub) && !listed(sc->fl, sub)) {
                if (sc->found < FILES_UNDESC_MAX)
                    snprintf(sc->fl->undesc[sc->found], FILE_PATH_MAX, "%s", sub);
                sc->found++;
            }
        }
    }
    closedir(d);
}

static int cmp_path(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

void files_refresh(FileList *fl, const char *cwd)
{
    if (!cwd || !*cwd) return;
    for (int i = 0; i < fl->count; i++) {
        FileEntry *e = &fl->items[i];
        char abs[1024];
        snprintf(abs, sizeof(abs), "%s/%s", cwd, e->path);
        struct stat st;
        e->exists = stat(abs, &st) == 0 && S_ISREG(st.st_mode);
        e->mtime = e->exists ? st.st_mtime : 0;
    }
    time_t now = time(NULL);
    if (fl->undescribed >= 0 && (double)(now - fl->scanned_at) < SCAN_EVERY) return;
    Scan sc = { fl, SCAN_LIMIT, 0, false };
    scan_dir(&sc, cwd, "", 0);
    fl->undescribed = sc.found;
    fl->undesc_count = sc.found < FILES_UNDESC_MAX ? sc.found : FILES_UNDESC_MAX;
    // Порядок readdir случаен; по алфавиту список читается и не прыгает
    // между обходами.
    qsort(fl->undesc, (size_t)fl->undesc_count, FILE_PATH_MAX, cmp_path);
    fl->scan_cut = sc.cut;
    fl->scanned_at = now;
}
