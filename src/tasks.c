#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "tasks.h"

static void tasks_path(const char *cwd, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s", cwd, TASKS_FILE);
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

static void append(char *dst, size_t cap, const char *s)
{
    size_t used = strlen(dst), n = strlen(s);
    if (used + n >= cap) n = cap - used - 1;
    memcpy(dst + used, s, n);
    dst[used + n] = '\0';
}

void tasks_load(TaskList *t, const char *cwd)
{
    memset(t, 0, sizeof(*t));
    if (!cwd || !*cwd) return;

    char path[700];
    tasks_path(cwd, path, sizeof(path));
    FILE *f = fopen(path, "r");
    if (!f) return;
    t->exists = true;
    t->mtime = file_mtime(path);

    Task *cur = NULL;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "## ", 3)) {
            if (t->count >= TASK_MAX) { t->partial = true; break; }
            cur = &t->items[t->count++];
            memset(cur, 0, sizeof(*cur));

            const char *p = line + 3;
            // Отметка о готовности — как в списках Markdown, чтобы файл
            // читался и без берта.
            if (!strncmp(p, "[x] ", 4) || !strncmp(p, "[X] ", 4)) {
                cur->state = TASK_DONE;
                p += 4;
            } else if (!strncmp(p, "[?] ", 4)) {
                cur->state = TASK_REVIEW;
                p += 4;
            } else if (!strncmp(p, "[ ] ", 4)) {
                p += 4;
            }
            snprintf(cur->title, sizeof(cur->title), "%s", p);
            rtrim(cur->title);
            continue;
        }

        if (!cur) {
            append(t->preamble, sizeof(t->preamble), line);
            continue;
        }
        // Пустые строки перед описанием не нужны; внутри — это абзацы.
        if (!cur->body[0] && (line[0] == '\n' || line[0] == '\r')) continue;
        append(cur->body, sizeof(cur->body), line);
    }
    fclose(f);

    rtrim(t->preamble);
    for (int i = 0; i < t->count; i++) rtrim(t->items[i].body);
}

bool tasks_save(const TaskList *t, const char *cwd)
{
    if (!cwd || !*cwd) return false;

    char dir[700], path[700];
    snprintf(dir, sizeof(dir), "%s/.berth", cwd);
    mkdir(dir, 0755);
    tasks_path(cwd, path, sizeof(path));

    // Пишем в соседний файл и переименовываем: обрыв посреди записи не должен
    // оставить список задач наполовину пустым.
    char tmp[720];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return false;

    if (t->preamble[0])
        fprintf(f, "%s\n\n", t->preamble);
    else
        fprintf(f, "# Задачи\n\n"
                   "Порядок записей — порядок работы; берт переставляет их "
                   "перетаскиванием на странице проекта.\n\n");

    for (int i = 0; i < t->count; i++) {
        const Task *task = &t->items[i];
        const char *mark = task->state == TASK_DONE   ? "[x] "
                         : task->state == TASK_REVIEW ? "[?] " : "";
        fprintf(f, "## %s%s\n", mark, task->title);
        if (task->body[0]) fprintf(f, "%s\n", task->body);
        fputc('\n', f);
    }
    fclose(f);
    if (rename(tmp, path) != 0) return false;
    // Своя запись — не повод перечитывать.
    ((TaskList *)t)->mtime = file_mtime(path);
    return true;
}

bool tasks_changed(const TaskList *t, const char *cwd)
{
    if (!cwd || !*cwd) return false;
    char path[700];
    tasks_path(cwd, path, sizeof(path));
    return file_mtime(path) != t->mtime;
}

void tasks_move(TaskList *t, int from, int to)
{
    if (from < 0 || from >= t->count || to < 0 || to >= t->count || from == to)
        return;
    Task moved = t->items[from];
    if (from < to)
        memmove(&t->items[from], &t->items[from + 1], sizeof(Task) * (size_t)(to - from));
    else
        memmove(&t->items[to + 1], &t->items[to], sizeof(Task) * (size_t)(from - to));
    t->items[to] = moved;
}

int tasks_count_in(const TaskList *t, TaskState state)
{
    int n = 0;
    for (int i = 0; i < t->count; i++)
        if (t->items[i].state == state) n++;
    return n;
}

size_t tasks_prompt(const Task *task, TaskFinish finish, char *out, size_t cap)
{
    if (task->body[0])
        snprintf(out, cap, "%s\n\n%s", task->title, task->body);
    else
        snprintf(out, cap, "%s", task->title);

    // Наказ повторяет заголовок дословно: агенту нужно найти строку в файле,
    // а не угадывать её по смыслу.
    if (finish != TASK_FINISH_NONE) {
        const char *mark = finish == TASK_FINISH_DONE ? "[x]" : "[?]";
        const char *what = finish == TASK_FINISH_DONE
            ? "сделана"
            : "сделана, нужно проверить";
        size_t used = strlen(out);
        snprintf(out + used, cap - used,
                 "\n\n---\nЭто задача из списка %s. Когда закончишь её, "
                 "отметь в этом файле: замени строку «## %s» на «## %s %s» "
                 "(%s). Остальное в файле не трогай.",
                 TASKS_FILE, task->title, mark, task->title, what);
    }
    return strlen(out);
}

const char *tasks_finish_name(TaskFinish f)
{
    switch (f) {
    case TASK_FINISH_DONE: return "done";
    case TASK_FINISH_NONE: return "none";
    default:               return "review";
    }
}

bool tasks_finish_parse(const char *name, TaskFinish *out)
{
    if (!strcmp(name, "review")) { *out = TASK_FINISH_REVIEW; return true; }
    if (!strcmp(name, "done"))   { *out = TASK_FINISH_DONE;   return true; }
    if (!strcmp(name, "none"))   { *out = TASK_FINISH_NONE;   return true; }
    return false;
}
