#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>

#include <sys/stat.h>
#include <unistd.h>

#include "claude.h"
#include "skills_builtin.h"

void claude_clear_session_env(void)
{
    static const char *vars[] = {
        "CLAUDECODE",
        "CLAUDE_CODE_ENTRYPOINT",
        "CLAUDE_CODE_SESSION_ID",
        "CLAUDE_CODE_CHILD_SESSION",
        "CLAUDE_CODE_BRIDGE_SESSION_ID",
        "CLAUDE_CODE_MESSAGING_SOCKET",
        "CLAUDE_CODE_MESSAGING_TOKEN",
        "CLAUDE_CODE_EXECPATH",
        "CLAUDE_CODE_EXPERIMENTAL_AGENT_TEAMS",
        "CLAUDE_PID",
        "CLAUDE_EFFORT",
    };
    for (size_t i = 0; i < sizeof(vars) / sizeof(vars[0]); i++)
        unsetenv(vars[i]);
}

bool claude_session_dir(const char *cwd, char *out, size_t cap)
{
    const char *home = getenv("HOME");
    if (!home || !cwd || !*cwd) return false;

    char slug[512];
    size_t n = 0;
    for (const char *p = cwd; *p && n + 1 < sizeof(slug); p++)
        slug[n++] = (*p == '/' || *p == '.' || *p == ' ') ? '-' : *p;
    slug[n] = '\0';

    int written = snprintf(out, cap, "%s/.claude/projects/%s", home, slug);
    return written > 0 && (size_t)written < cap;
}

bool claude_has_history(const char *cwd)
{
    char dir[768];
    if (!claude_session_dir(cwd, dir, sizeof(dir))) return false;

    DIR *d = opendir(dir);
    if (!d) return false;

    // Сессия — это файл .jsonl; пустой каталог историей не считается.
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(d)) != NULL) {
        const char *dot = strrchr(e->d_name, '.');
        if (dot && strcmp(dot, ".jsonl") == 0) found = true;
    }
    closedir(d);
    return found;
}

const char *claude_bundle_dir(void)
{
    static char dir[1024];
    if (dir[0]) return dir;
    const char *home = getenv("HOME");
    if (!home) return NULL;
    char p[1024];
    snprintf(p, sizeof(p), "%s/.config", home);              mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/.config/berth", home);        mkdir(p, 0755);
    snprintf(dir, sizeof(dir), "%s/.config/berth/bundle", home); mkdir(dir, 0755);
    snprintf(p, sizeof(p), "%s/.claude", dir);               mkdir(p, 0755);
    snprintf(p, sizeof(p), "%s/.claude/skills", dir);        mkdir(p, 0755);
    return dir;
}

static bool listed(const char *list, const char *name)
{
    if (!list) return false;
    size_t n = strlen(name);
    const char *p = list;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char *e = p;
        while (*e && *e != ',' && *e != ' ') e++;
        if ((size_t)(e - p) == n && !strncmp(p, name, n)) return true;
        p = e;
    }
    return false;
}

// Наказ берта каждому запуску claude. Описание скилла — подсказка, и агент
// в Internet Revolution написал письмо, не описав его: скилл berth-files
// он видел в списке и не позвал. Паспорт из папки --add-dir Claude Code не
// читает (проверено), поэтому правило едет системным промптом. Без
// апострофов и кавычек: строка уходит файлом rules.md в пакете и
// подставляется через $(cat …) — набранная в оболочку целиком, она
// красовалась бы на экране вкладки. Claude Code
// запоминает его при первом запросе разговора; продолженный разговор
// получит новый текст только после сжатия.
static const char BERTH_RULES[] =
    "Ты запущен из берта (Berth). Его правила обязательны:\n- Создал или существенно переписал документ для человека (письмо, записку, отчёт, таблицу, презентацию, картинку, запись) — в том же ходе допиши строку в .berth/files.tsv проекта: путь, табуляция, что это одной фразой (скилл berth-files). Код, конфиги, паспорт и служебные файлы не описываются.\n- Задачи и планы — в .berth/tasks.md (скилл berth-tasks), а не списком в ответе.\n- Однотипные записи с атрибутами, к которым вернутся, — таблицей .berth/data/<имя>.tsv (скилл berth-data).\n- Новый проект — новая папка с паспортом CLAUDE.md рядом с проектами той же группы; в панель берт добавит её сам, списки проектов править не нужно.\nСкиллы berth-* лежат в подключённой папке пакета.";

const char *claude_rules_text(void) { return BERTH_RULES; }

void claude_install_bundle(const char *off)
{
    const char *dir = claude_bundle_dir();
    if (!dir) return;
    for (int i = 0; i < SKILLS_BUILTIN_COUNT; i++) {
        char sd[1200], file[1300];
        snprintf(sd, sizeof(sd), "%s/.claude/skills/%s", dir, SKILLS_BUILTIN[i].name);
        snprintf(file, sizeof(file), "%s/SKILL.md", sd);
        if (listed(off, SKILLS_BUILTIN[i].name)) {
            unlink(file);
            rmdir(sd);
            continue;
        }
        mkdir(sd, 0755);
        // Перезаписываем всегда: это файлы берта, их версия — версия берта.
        FILE *f = fopen(file, "w");
        if (!f) continue;
        fputs(SKILLS_BUILTIN[i].skill_md, f);
        fclose(f);
    }
    char rules[1200];
    snprintf(rules, sizeof(rules), "%s/rules.md", dir);
    FILE *rf = fopen(rules, "w");
    if (rf) { fputs(BERTH_RULES, rf); fputc('\n', rf); fclose(rf); }
}

void claude_with_bundle(const char *cmd, char *out, size_t cap)
{
    const char *dir = claude_bundle_dir();
    if (!cmd) { if (cap) out[0] = '\0'; return; }
    bool is_claude = !strncmp(cmd, "claude", 6) && (cmd[6] == ' ' || cmd[6] == '\0');
    if (!dir || !is_claude) {
        snprintf(out, cap, "%s", cmd);
        return;
    }
    snprintf(out, cap, "claude --add-dir '%s' --append-system-prompt \"$(cat '%s/rules.md')\"%s",
             dir, dir, cmd + 6);
}
