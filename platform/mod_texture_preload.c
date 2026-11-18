/* mod_texture_preload.c — see mod_texture_preload.h.
 *
 * Single-threaded by construction: every entry point runs on the game thread,
 * and the only cross-thread work is the store's own decode queue.
 */
#include "mod_texture_preload.h"

#include "mod_texture_store.h"
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__vita__)

extern void mdkr_vita_boot_log(const char *msg);

/* A level's identity list. Bounded so a corrupt or hostile file cannot make
 * the game allocate without limit; a track that genuinely needs more simply
 * preloads the first entries and resolves the rest the old way. */
#define PRELOAD_MAX_ENTRIES 2048u
#define PRELOAD_MAGIC "MDKRTEXLIST1"

typedef struct PreloadEntry {
    uint32_t crc;
    int16_t  fmt;
    int16_t  siz;
} PreloadEntry;

static PreloadEntry s_entries[PRELOAD_MAX_ENTRIES];
static unsigned     s_count;
static unsigned     s_queue_next;
static int          s_level_id = -1;
static int          s_dirty;
static int          s_active;

/* No size is known before the decode; charge a typical converted texture
 * (256x256 RGBA8 plus its chain) for each one still in flight. */
#define PRELOAD_ENTRY_ESTIMATE (350u * 1024u)
/* The hold never outlasts this, however slow the card or the pack. */
#define PRELOAD_HOLD_MAX_US (6u * 1000u * 1000u)

/* Beside the shader cache, and created the same way: a fixed ux0 path rather
 * than the portable user-paths policy, because on Vita the game's data
 * directory IS this one. */
#define PRELOAD_DIR "ux0:data/goldenballoon/texcache"

static int preload_path(char *out, size_t cap, int level_id) {
    return snprintf(out, cap, PRELOAD_DIR "/lvl_%d.list", level_id) < (int)cap;
}

static void preload_ensure_dir(void) {
    sceIoMkdir(PRELOAD_DIR, 0777);   /* already-exists is not an error here */
}

static int entry_known(uint32_t crc, int fmt, int siz) {
    unsigned index;
    for (index = 0; index < s_count; index++) {
        if (s_entries[index].crc == crc && s_entries[index].fmt == fmt &&
            s_entries[index].siz == siz) {
            return 1;
        }
    }
    return 0;
}

/* Menus, the title screen and every other non-level scene share one list.
 * They draw pack textures too, and they are exactly where a first impression
 * of the port is formed. */
#define PRELOAD_MENU_ID (-1)

static void preload_write_current(void);

/* Queue recorded entries in order while the decoded-but-not-yet-drawn set
 * still fits the cache. Queuing a whole scene regardless made every new decode
 * evict the previous one, so the helpers ran flat out and almost nothing
 * stayed resident. Pixels leave the cache once uploaded, so this is
 * re-entered as decodes land. Returns nonzero once the budget stops it. */
static int preload_queue_more(void) {
    const size_t cap = mdkr_mod_texture_cache_bytes_max();
    const size_t budget = cap - (cap / 4u);      /* leave a quarter free */
    size_t projected = mdkr_mod_texture_resident_bytes() +
        (size_t)mdkr_mod_texture_pending_count() * PRELOAD_ENTRY_ESTIMATE;
    while (s_queue_next < s_count) {
        const PreloadEntry *entry = &s_entries[s_queue_next];
        if (projected >= budget) return 1;
        s_queue_next++;
        /* Lists also record identities the pack does not replace; those
         * answer 0 and are simply skipped rather than ending the preload. */
        if (mdkr_mod_texture_prefetch_rice(entry->crc, entry->fmt,
                                           entry->siz) &&
            mdkr_mod_texture_rice_pending(entry->crc, entry->fmt,
                                          entry->siz)) {
            projected += PRELOAD_ENTRY_ESTIMATE;
        }
    }
    return 0;
}

static void preload_activate(int level_id) {
    char  path[640];
    FILE *file;
    char  magic[sizeof(PRELOAD_MAGIC) - 1];

    preload_write_current();
    s_count = 0;
    s_queue_next = 0;
    s_dirty = 0;
    s_level_id = level_id;
    s_active = mdkr_mod_texture_rice_active();
    if (!s_active) return;
    if (!preload_path(path, sizeof path, level_id)) return;

    file = fopen(path, "rb");
    if (file == NULL) return;   /* first visit: this run writes the list */
    if (fread(magic, 1, sizeof magic, file) != sizeof magic ||
        memcmp(magic, PRELOAD_MAGIC, sizeof magic) != 0) {
        fclose(file);
        return;
    }
    while (s_count < PRELOAD_MAX_ENTRIES) {
        PreloadEntry entry;
        if (fread(&entry, sizeof entry, 1, file) != 1) break;
        s_entries[s_count++] = entry;
    }
    fclose(file);

    (void)preload_queue_more();
    fprintf(stderr, "[MODS] scene %d: queued %u of %u preload textures\n",
            level_id, s_queue_next, s_count);
}

void mdkr_mod_texture_preload_level_begin(int level_id) {
    preload_activate(level_id);
}

int mdkr_mod_texture_preload_hold(void) {
    SceUInt64 started;
    SceUInt64 elapsed = 0;
    int budget_full = 0;
    int pending = 0;
    char line[128];

    if (!s_active || s_count == 0) return 0;
    started = sceKernelGetProcessTimeWide();
    for (;;) {
        budget_full = preload_queue_more();
        pending = mdkr_mod_texture_pending_count();
        elapsed = sceKernelGetProcessTimeWide() - started;
        if (pending == 0 && (budget_full || s_queue_next >= s_count)) break;
        if (elapsed >= PRELOAD_HOLD_MAX_US) break;
        sceKernelDelayThread(2000);
    }
    snprintf(line, sizeof line,
             "[MODS] scene %d: held %u ms for %u of %u textures%s",
             s_level_id, (unsigned)(elapsed / 1000u), s_queue_next, s_count,
             pending != 0 ? " (timed out)"
                          : budget_full ? " (cache full)" : "");
    fprintf(stderr, "%s\n", line);
    mdkr_vita_boot_log(line);
    return elapsed >= 1000u;
}

/* Called once the pack registry is up, so the title screen and menus are
 * already being filled in while the player is reading them. */
void mdkr_mod_texture_preload_boot(void) {
    preload_activate(PRELOAD_MENU_ID);
}

void mdkr_mod_texture_preload_note(uint32_t crc, int fmt, int siz) {
    if (!s_active) return;
    if (s_count >= PRELOAD_MAX_ENTRIES) return;
    if (entry_known(crc, fmt, siz)) return;
    s_entries[s_count].crc = crc;
    s_entries[s_count].fmt = (int16_t)fmt;
    s_entries[s_count].siz = (int16_t)siz;
    s_count++;
    s_dirty = 1;
}

static void preload_write_current(void) {
    char  path[640];
    FILE *file;

    if (!s_active || !s_dirty || s_count == 0) return;
    if (!preload_path(path, sizeof path, s_level_id)) return;
    preload_ensure_dir();
    file = fopen(path, "wb");
    if (file == NULL) return;
    fwrite(PRELOAD_MAGIC, 1, sizeof(PRELOAD_MAGIC) - 1, file);
    fwrite(s_entries, sizeof s_entries[0], s_count, file);
    fclose(file);
    s_dirty = 0;
}

void mdkr_mod_texture_preload_level_end(void) {
    /* Back to the menus, which is both where the list gets written and where
     * the next scene's textures start arriving. */
    preload_activate(PRELOAD_MENU_ID);
}

#else /* !__vita__ */

void mdkr_mod_texture_preload_level_begin(int level_id) { (void)level_id; }
int mdkr_mod_texture_preload_hold(void) { return 0; }
void mdkr_mod_texture_preload_boot(void) {}
void mdkr_mod_texture_preload_note(uint32_t crc, int fmt, int siz) {
    (void)crc; (void)fmt; (void)siz;
}
void mdkr_mod_texture_preload_level_end(void) {}

#endif
