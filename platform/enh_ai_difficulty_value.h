/* Pure opponent-skill interpretation shared by gameplay and settings.
 * Does not read/latch configuration or mutate the caller's saved/staged text. */
#ifndef MDKR64_ENH_AI_DIFFICULTY_VALUE_H
#define MDKR64_ENH_AI_DIFFICULTY_VALUE_H

#define MDKR_AI_DIFFICULTY_AUTHORED "authored"
#define MDKR_AI_DIFFICULTY_HARD     "hard"
#define MDKR_AI_DIFFICULTY_BRUTAL   "brutal"

static inline int mdkr_ai_difficulty_value_ci_equal(const char *a, const char *b) {
    if (!a || !b) {
        return 0;
    }
    while (*a != '\0' && *b != '\0') {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char) (ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char) (cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
        ++a;
        ++b;
    }
    return *a == '\0' && *b == '\0';
}

/* Case-insensitive ASCII, exactly as gameplay has always accepted it.
 * Empty, null and unknown values mean authored; no whitespace trimming or
 * aliases that would silently change existing gameplay. Returned literals
 * have static lifetime. Resolve current and desired snapshots independently. */
static inline const char *mdkr_ai_difficulty_effective_value(const char *value) {
    if (mdkr_ai_difficulty_value_ci_equal(value, MDKR_AI_DIFFICULTY_HARD)) {
        return MDKR_AI_DIFFICULTY_HARD;
    }
    if (mdkr_ai_difficulty_value_ci_equal(value, MDKR_AI_DIFFICULTY_BRUTAL)) {
        return MDKR_AI_DIFFICULTY_BRUTAL;
    }
    return MDKR_AI_DIFFICULTY_AUTHORED;
}

#endif /* MDKR64_ENH_AI_DIFFICULTY_VALUE_H */
