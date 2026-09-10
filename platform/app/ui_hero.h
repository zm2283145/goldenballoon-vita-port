// ui_hero.h — the brand hero image on the launcher's home.
//
// The launcher drew no images at all before this: every pixel was an ImGui
// widget, which is a large part of why it read as a settings dialog rather than
// the front door of a game. This is the one raster asset it draws.
//
// The art is project-authored (brand/hero.png -- the Golden Balloon wordmark,
// not a frame of the game), so it carries no ROM-derived pixels and the
// asset-free release verifier has nothing to object to.
//
// Failure is not fatal anywhere in here. A decode failure, an out-of-memory, or
// a build with the art stripped simply reports "no hero", and the home draws
// its heading instead. The front door must never depend on decoration.
#ifndef MDKR64_UI_HERO_H
#define MDKR64_UI_HERO_H

#include "imgui.h"

namespace ui {

// Draws the hero band across the available content width, scaled to preserve
// the art's aspect and clamped to `maxHeight` logical pixels. Returns false
// when there is no usable texture, in which case nothing was drawn and the
// caller should fall back to its text heading.
bool HeroBanner(float maxHeight);

// True when the art decoded and a texture is available. Panels use this to
// choose a layout before drawing, and the app smoke gate reads it to prove the
// fallback path is reachable rather than assumed.
bool HeroAvailable();

}  // namespace ui

#endif  // MDKR64_UI_HERO_H
