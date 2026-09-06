#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "skills.h"

static time_t mtime_of(const char *path)
{
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 ? st.st_mtime : 0;
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

// Обрезанная snprintf строка может кончаться половиной буквы — рисуется
// она как «?». Срезаем хвост до границы символа.
static void cut_utf8(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] & 0xC0) == 0x80) n--;   // продолжения без начала
    // n указывает на начало последнего символа; проверим, полон ли он
    if (n > 0) {
        unsigned char c = (unsigned char)s[n - 1];
        size_t need = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        if (strlen(s) - (n - 1) < need) s[n - 1] = '\0';
        else if (strlen(s) - (n - 1) > need) { /* всё цело */ }
    }
}

// Значение поля шапки: без кавычек, одной строкой.
static void unquote(char *s)
{
    cut_utf8(s);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = '\0';
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    } else if (n >= 1 && (s[0] == '"' || s[0] == '\'')) {
        memmove(s, s + 1, n - 1);   // кавычка закрывается на другой строке
        s[n - 1] = '\0';
    }
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (r[0] == '\\' && r[1] == '"') continue;
        *w++ = *r;
    }
    *w = '\0';
}

// Шапка и первый абзац SKILL.md. Абзац — первые строки после шапки, не
// заголовки и не пустые, до первой пустой.
static void read_skill_md(const char *dir, Skill *sk)
{
    char file[PROJECT_PATH_MAX + 16];
    snprintf(file, sizeof(file), "%s/SKILL.md", dir);
    FILE *f = fopen(file, "r");
    if (!f) return;

    char line[1024];
    int  state = 0;   // 0 — до шапки, 1 — в шапке, 2 — тело
    bool in_intro = false;
    int  lines = 0;
    while (fgets(line, sizeof(line), f) && lines++ < 200) {
        if (state < 2 && !strncmp(line, "---", 3)) {
            if (state == 1) state = 2;
            else state = 1;
            continue;
        }
        if (state == 0) { state = 2; }   // шапки нет — сразу тело
        if (state == 1) {
            if (!strncmp(line, "name:", 5)) {
                const char *v = line + 5;
                while (*v == ' ') v++;
                snprintf(sk->name, sizeof(sk->name), "%s", v);
                unquote(sk->name);
            } else if (!strncmp(line, "description:", 12)) {
                const char *v = line + 12;
                while (*v == ' ') v++;
                if (*v == '>' || *v == '|') {
                    if (fgets(line, sizeof(line), f)) {
                        v = line;
                        while (*v == ' ') v++;
                        snprintf(sk->desc, sizeof(sk->desc), "%s", v);
                    }
                } else {
                    snprintf(sk->desc, sizeof(sk->desc), "%s", v);
                }
                unquote(sk->desc);
            }
            continue;
        }
        // Тело: ищем первый абзац.
        line[strcspn(line, "\r\n")] = '\0';
        bool blank = line[0] == '\0';
        if (!in_intro) {
            if (blank || line[0] == '#') continue;
            in_intro = true;
        } else if (blank || line[0] == '#') {
            break;
        }
        size_t used = strlen(sk->intro);
        if (used + strlen(line) + 2 >= sizeof(sk->intro)) break;
        snprintf(sk->intro + used, sizeof(sk->intro) - used, "%s%s", used ? " " : "", line);
    }
    fclose(f);
}

static int by_name(const void *a, const void *b)
{
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

// Имена папок со SKILL.md внутри dir, по алфавиту.
static int list_skills(const char *dir, char names[][SKILL_NAME_MAX], int cap)
{
    DIR *d = dir && dir[0] ? opendir(dir) : NULL;
    if (!d) return 0;
    char found[SKILLS_MAX][SKILL_NAME_MAX];
    char *sorted[SKILLS_MAX];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < cap && n < SKILLS_MAX) {
        if (e->d_name[0] == '.') continue;
        char file[PROJECT_PATH_MAX + 16];
        snprintf(file, sizeof(file), "%s/%s/SKILL.md", dir, e->d_name);
        if (!exists(file)) continue;
        snprintf(found[n], SKILL_NAME_MAX, "%s", e->d_name);
        sorted[n] = found[n];
        n++;
    }
    closedir(d);
    qsort(sorted, (size_t)n, sizeof(sorted[0]), by_name);
    for (int i = 0; i < n; i++) snprintf(names[i], SKILL_NAME_MAX, "%s", sorted[i]);
    return n;
}

static bool has_skill(const SkillList *sl, const char *name)
{
    for (int i = 0; i < sl->count; i++)
        if (!strcmp(sl->items[i].name, name)) return true;
    return false;
}

static void add_skill(SkillList *sl, const char *name, const char *dir,
                      SkillKind kind, const char *from)
{
    if (sl->count >= SKILLS_MAX) return;
    Skill *s = &sl->items[sl->count++];
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", name);
    snprintf(s->path, sizeof(s->path), "%s", dir);
    s->kind = kind;
    if (from) snprintf(s->from, sizeof(s->from), "%s", from);
    read_skill_md(dir, s);
    if (!s->name[0]) snprintf(s->name, sizeof(s->name), "%s", name);
}

static const char *base_name(const char *path)
{
    const char *p = path + strlen(path);
    while (p > path && p[-1] == '/') p--;
    const char *end = p;
    while (p > path && p[-1] != '/') p--;
    static char out[PROJECT_NAME_MAX];
    snprintf(out, sizeof(out), "%.*s", (int)(end - p), p);
    return out;
}

static void personal_dir(char *out, size_t cap)
{
    const char *home = getenv("HOME");
    if (home) snprintf(out, cap, "%s/.claude/skills", home);
    else out[0] = '\0';
}

void skills_load(SkillList *sl, const char *cwd, const char *parent)
{
    memset(sl, 0, sizeof(*sl));
    if (!cwd || !*cwd) return;
    snprintf(sl->cwd, sizeof(sl->cwd), "%s", cwd);
    if (parent && *parent) snprintf(sl->parent, sizeof(sl->parent), "%s", parent);

    char own_dir[PROJECT_PATH_MAX], parent_dir[PROJECT_PATH_MAX] = "", personal[PROJECT_PATH_MAX];
    snprintf(own_dir, sizeof(own_dir), "%s/.claude/skills", cwd);
    if (sl->parent[0]) snprintf(parent_dir, sizeof(parent_dir), "%s/.claude/skills", sl->parent);
    personal_dir(personal, sizeof(personal));

    char names[SKILLS_MAX][SKILL_NAME_MAX];
    int n = list_skills(own_dir, names, SKILLS_MAX);
    for (int i = 0; i < n; i++) {
        char dir[PROJECT_PATH_MAX];
        snprintf(dir, sizeof(dir), "%s/%s", own_dir, names[i]);
        add_skill(sl, names[i], dir, SKILL_OWN, NULL);
    }
    if (parent_dir[0]) {
        n = list_skills(parent_dir, names, SKILLS_MAX);
        for (int i = 0; i < n; i++) {
            if (has_skill(sl, names[i])) continue;   // своё перекрывает
            char dir[PROJECT_PATH_MAX];
            snprintf(dir, sizeof(dir), "%s/%s", parent_dir, names[i]);
            add_skill(sl, names[i], dir, SKILL_INHERITED, base_name(sl->parent));
        }
    }
    sl->personal_count = list_skills(personal, names, SKILLS_MAX);

    sl->own_mtime      = mtime_of(own_dir);
    sl->parent_mtime   = mtime_of(parent_dir);
    sl->personal_mtime = mtime_of(personal);
    sl->checked = time(NULL);
}

bool skills_changed(SkillList *sl)
{
    if (!sl->cwd[0]) return false;
    time_t now = time(NULL);
    if (now - sl->checked < 2) return false;
    sl->checked = now;

    char own_dir[PROJECT_PATH_MAX], parent_dir[PROJECT_PATH_MAX] = "", personal[PROJECT_PATH_MAX];
    snprintf(own_dir, sizeof(own_dir), "%s/.claude/skills", sl->cwd);
    if (sl->parent[0]) snprintf(parent_dir, sizeof(parent_dir), "%s/.claude/skills", sl->parent);
    personal_dir(personal, sizeof(personal));
    return mtime_of(own_dir) != sl->own_mtime
        || mtime_of(parent_dir) != sl->parent_mtime
        || mtime_of(personal) != sl->personal_mtime;
}

bool skills_create(const char *cwd, const char *name, const char *when,
                   char *err, size_t err_cap)
{
    if (err && err_cap) err[0] = '\0';
    if (!name || !*name || name[0] == '.' || strchr(name, '/') || strchr(name, ' ')) {
        if (err) snprintf(err, err_cap, "Имя скилла — это имя папки: латиницей или кириллицей, без пробелов и «/»");
        return false;
    }
    char dir[PROJECT_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/.claude", cwd);
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/.claude/skills", cwd);
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/.claude/skills/%s", cwd, name);
    if (exists(dir)) {
        if (err) snprintf(err, err_cap, "Скилл «%s» уже есть", name);
        return false;
    }
    if (mkdir(dir, 0755) != 0) {
        if (err) snprintf(err, err_cap, "Не удалось создать %s", dir);
        return false;
    }
    char file[PROJECT_PATH_MAX + 16];
    snprintf(file, sizeof(file), "%s/SKILL.md", dir);
    FILE *f = fopen(file, "w");
    if (!f) {
        if (err) snprintf(err, err_cap, "Папка создана, но SKILL.md записать не удалось");
        return false;
    }
    const char *w = when && *when ? when : "…";
    fprintf(f,
        "---\n"
        "name: %s\n"
        "description: \"%s\"\n"
        "---\n"
        "\n"
        "# %s\n"
        "\n"
        "Одним абзацем: что этот скилл делает и зачем. Этот абзац берт\n"
        "показывает на странице проекта.\n"
        "\n"
        "## Когда применять\n"
        "%s\n"
        "\n"
        "## Что делать\n"
        "1. …\n"
        "2. …\n"
        "\n"
        "## Чем проверить\n"
        "- …\n",
        name, w, name, w);
    fclose(f);
    return true;
}

void skills_builtin_desc(const char *skill_md, char *out, size_t cap)
{
    out[0] = '\0';
    const char *d = strstr(skill_md, "\ndescription:");
    if (!d) return;
    d += 13;
    while (*d == ' ') d++;
    const char *e = strchr(d, '\n');
    size_t n = e ? (size_t)(e - d) : strlen(d);
    if (n >= cap) n = cap - 1;
    memcpy(out, d, n);
    out[n] = '\0';
    unquote(out);
}
