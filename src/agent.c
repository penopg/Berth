#include <string.h>

#include "agent.h"
#include "claude.h"
#include "projinfo.h"

static const AgentProfile PROFILES[] = {
    {
        .id = "shell",
        .label = "оболочка",
        .launch = NULL,
        .reports_progress = false,
    },
    {
        .id = "claude",
        .label = "Claude Code",
        .launch = "claude",
        // Запасной путь, если resume_command почему-то ничего не вернул.
        // Не `--continue`: «самая свежая» у каждой вкладки разрешается
        // одинаково, и несколько вкладок садятся в один диалог.
        .launch_resume = "claude",
        .resume_command = projinfo_resume_command,
        .has_history = claude_has_history,
        .clear_context = "/clear",
        .reports_progress = true,
    },
};

const AgentProfile *agent_default(void)
{
    return &PROFILES[0];
}

const AgentProfile *agent_by_id(const char *id)
{
    if (!id) return agent_default();
    for (size_t i = 0; i < sizeof(PROFILES) / sizeof(PROFILES[0]); i++)
        if (strcmp(PROFILES[i].id, id) == 0)
            return &PROFILES[i];
    return agent_default();
}
