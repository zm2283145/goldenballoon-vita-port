// Native Vita trophy bridge. Other targets deliberately receive a no-op.
#ifndef MDKR64_VITA_TROPHY_H
#define MDKR64_VITA_TROPHY_H

struct Settings;

// Reconciles the currently loaded save with the installed Vita trophy pack.
// It is safe to call once per game tick.
void mdkr_vita_trophy_pump(const struct Settings *settings);

#endif
