#ifndef BERTH_SKILLS_BUILTIN_H
#define BERTH_SKILLS_BUILTIN_H

typedef struct {
    const char *name;
    const char *skill_md;
} BuiltinSkill;

extern const BuiltinSkill SKILLS_BUILTIN[];
extern const int SKILLS_BUILTIN_COUNT;

#endif
