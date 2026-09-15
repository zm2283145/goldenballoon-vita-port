/* ROM-free effective-value contract, shared unchanged by C gameplay and C++ UI. */
#include "enh_ai_difficulty_value.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void expect(int condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        abort();
    }
}

static void check_case_variants(const char *canonical) {
    const size_t length = strlen(canonical);
    const unsigned count = 1u << length;
    unsigned mask;
    for (mask = 0; mask < count; ++mask) {
        char input[16];
        char original[16];
        size_t i;
        strcpy(input, canonical);
        for (i = 0; i < length; ++i) {
            if ((mask & (1u << i)) != 0) {
                input[i] = (char) (input[i] - 'a' + 'A');
            }
        }
        strcpy(original, input);
        expect(strcmp(mdkr_ai_difficulty_effective_value(input), canonical) == 0,
               "every ASCII case combination resolves to its canonical arm");
        expect(strcmp(input, original) == 0, "resolution preserves raw config text");
    }
}

int main(void) {
    static const char *const unknown[] = {
        "", "typo", "harder", "bruta", " hard", "hard ", "brutal\n",
        "1", "0", "on", "off", "\xc3\xa9", "HAR\xc4\xb0"
    };
    size_t i;
    const char *desired;
    const char *current;
    char staged[] = "HARD";
    check_case_variants(MDKR_AI_DIFFICULTY_AUTHORED);
    check_case_variants(MDKR_AI_DIFFICULTY_HARD);
    check_case_variants(MDKR_AI_DIFFICULTY_BRUTAL);
    expect(strcmp(mdkr_ai_difficulty_effective_value(NULL),
                  MDKR_AI_DIFFICULTY_AUTHORED) == 0, "null resolves to authored");
    for (i = 0; i < sizeof(unknown) / sizeof(unknown[0]); ++i) {
        expect(strcmp(mdkr_ai_difficulty_effective_value(unknown[i]),
                      MDKR_AI_DIFFICULTY_AUTHORED) == 0,
               "unknown, whitespace and non-ASCII input retain authored fallback");
    }

    desired = mdkr_ai_difficulty_effective_value(staged);
    current = mdkr_ai_difficulty_effective_value("unrecognized");
    expect(strcmp(desired, MDKR_AI_DIFFICULTY_HARD) == 0,
           "staged desired value does not read the current arm");
    expect(strcmp(current, MDKR_AI_DIFFICULTY_AUTHORED) == 0,
           "current value is resolved independently of desired");
    staged[0] = '\0';
    expect(strcmp(mdkr_ai_difficulty_effective_value(staged),
                  MDKR_AI_DIFFICULTY_AUTHORED) == 0,
           "subsequent resolution is not latched");
    expect(strcmp(desired, MDKR_AI_DIFFICULTY_HARD) == 0,
           "previous resolved value has independent stable lifetime");
    puts("PASS: shared opponent-skill effective value");
    return 0;
}
