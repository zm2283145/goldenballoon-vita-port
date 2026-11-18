/* mod_texture_preload.h — per-level HD texture preload list.
 *
 * Remembers which Rice identities a level actually drew with and queues them
 * on the decode helper the next time that level loads, so replacements are
 * resident before the first frame instead of arriving during play. The list
 * lives beside the shader cache and is regenerated whenever it is wrong.
 */
#ifndef MDKR64_MOD_TEXTURE_PRELOAD_H
#define MDKR64_MOD_TEXTURE_PRELOAD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Level load: queue everything the last visit recorded. No-op off Vita, with
 * no pack installed, or with no list for this level yet. */
void mdkr_mod_texture_preload_level_begin(int level_id);

/* Level load, after level_begin: block (bounded) until the recorded
 * replacements that fit the cache are decoded, so the first frames draw them
 * instead of popping in. Returns nonzero when it actually waited, in which
 * case the caller should retire the wait from game time. */
int mdkr_mod_texture_preload_hold(void);

/* Startup, after the pack registry is scanned: queue the menu/title list. */
void mdkr_mod_texture_preload_boot(void);

/* Renderer: one Rice identity resolved for the current level. Cheap enough
 * for a texture-cache miss (a bounded linear scan of identities seen so far).
 */
void mdkr_mod_texture_preload_note(uint32_t crc, int fmt, int siz);

/* Level unload: write the list back when this visit saw something new. */
void mdkr_mod_texture_preload_level_end(void);

#ifdef __cplusplus
}
#endif
#endif /* MDKR64_MOD_TEXTURE_PRELOAD_H */
