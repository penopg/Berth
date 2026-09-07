#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "actions.h"

static void actions_path(const char *cwd, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", cwd, ACTIONS_FILE);
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

// Область: «запись:<путь>» или «таблица:<путь>». Латинские row/table тоже
// понимаем — файл могут написать по-английски.
static bool parse_scope(const char *field, Action *a)
{
    const char *colon = strchr(field, ':');
    if (!colon) return false;
    size_t len = (size_t)(colon - field);
    if (!strncmp(field, "запись", len) && len == strlen("запись"))      a->scope = ACTION_ROW;
    else if (!strncmp(field, "row", len) && len == 3)                    a->scope = ACTION_ROW;
    else if (!strncmp(field, "таблица", len) && len == strlen("таблица")) a->scope = ACTION_TABLE;
    else if (!strncmp(field, "table", len) && len == 5)                  a->scope = ACTION_TABLE;
    else return false;
    const char *p = colon + 1;
    while (*p == ' ') p++;
    if (!strncmp(p, "./", 2)) p += 2;
    snprintf(a->target, sizeof(a->target), "%s", p);
    rtrim(a->target);
    return a->target[0] != '\0';
}

void actions_load(ActionList *al, const char *cwd)
{
    memset(al, 0, sizeof(*al));
    if (!cwd || !*cwd) return;

    char path[700];
    actions_path(cwd, path, sizeof(path));
    al->mtime = file_mtime(path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    al->exists = true;

    char line[ACTION_TEXT_MAX + ACTION_TARGET_MAX + 128];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') continue;
        if (al->count >= ACTIONS_MAX) break;

        char *f1 = line, *f2 = strchr(f1, '\t');
        if (!f2) continue;
        *f2++ = '\0';
        char *f3 = strchr(f2, '\t');
        if (!f3) continue;
        *f3++ = '\0';
        char *f4 = strchr(f3, '\t');
        if (!f4) continue;
        *f4++ = '\0';

        Action *a = &al->items[al->count];
        memset(a, 0, sizeof(*a));
        if (!parse_scope(f1, a)) continue;
        snprintf(a->name, sizeof(a->name), "%s", f2);
        rtrim(a->name);
        rtrim(f3);
        // Пусто — разговор: чаще хочется увидеть ответ, а не молчаливую
        // правку файла.
        a->to_task = !strcmp(f3, "задача") || !strcmp(f3, "task");
        snprintf(a->text, sizeof(a->text), "%s", f4);
        rtrim(a->text);
        if (a->name[0] && a->text[0]) al->count++;
    }
    fclose(f);
}

bool actions_changed(const ActionList *al, const char *cwd)
{
    if (!cwd || !*cwd) return false;
    char path[700];
    actions_path(cwd, path, sizeof(path));
    return file_mtime(path) != al->mtime;
}

static bool known(const char *name, size_t len, const char *const *names,
                  const char *const *values, int n, const char **val)
{
    for (int i = 0; i < n; i++) {
        if (strlen(names[i]) != len || strncmp(names[i], name, len)) continue;
        *val = values[i];
        return true;
    }
    return false;
}

int action_expand(const char *text, const char *const *names, const char *const *values,
                  int n, char *out, size_t cap,
                  char ask[][ACTION_LABEL_MAX], int ask_cap)
{
    size_t used = 0;
    int asked = 0;
    for (const char *p = text; *p; ) {
        if (*p != '{') {
            if (used + 1 < cap) out[used++] = *p;
            p++;
            continue;
        }
        const char *end = strchr(p, '}');
        if (!end) {                       // незакрытая скобка — просто текст
            if (used + 1 < cap) out[used++] = *p;
            p++;
            continue;
        }
        size_t len = (size_t)(end - p - 1);
        const char *val = NULL;
        if (known(p + 1, len, names, values, n, &val)) {
            int w = snprintf(out + used, cap - used, "%s", val ? val : "");
            if (w > 0) used += (size_t)w < cap - used ? (size_t)w : cap - used - 1;
        } else {
            // Неизвестное имя оставляем в тексте и записываем в вопросы:
            // форма выводится из шаблона, отдельно её не описывают.
            int w = snprintf(out + used, cap - used, "%.*s", (int)(end - p + 1), p);
            if (w > 0) used += (size_t)w < cap - used ? (size_t)w : cap - used - 1;
            if (ask && len < ACTION_LABEL_MAX) {
                bool dup = false;
                for (int i = 0; i < asked; i++)
                    if (!strncmp(ask[i], p + 1, len) && strlen(ask[i]) == len) dup = true;
                if (!dup && asked < ask_cap) {
                    snprintf(ask[asked], ACTION_LABEL_MAX, "%.*s", (int)len, p + 1);
                    asked++;
                }
            }
        }
        p = end + 1;
    }
    out[used < cap ? used : cap - 1] = '\0';
    return asked;
}
