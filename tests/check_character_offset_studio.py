#!/usr/bin/env python3
"""Pin the exact Offset Studio's cross-layer safety contract.

This is intentionally a source contract: the failure modes are missing handoff
links (a bool dropped between UI and boot), accidentally pausing the scene,
returning on the pre-edit frame, or publishing an editor-contaminated run as a
clean performance approval. Those properties span independently tested C and
C++ owners and are more legible here than in a renderer screenshot fixture.
"""

from pathlib import Path
import sys


ROOT = Path(__file__).resolve().parent.parent


def source(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> int:
    settings = source("platform/app/ui_settings.cpp")
    launcher = source("platform/app/ui_launcher.cpp")
    overlay = source("platform/app/ui_overlay.cpp")
    main_app = source("platform/app/main_app.cpp")
    boot = source("platform/app/engine_boot.cpp")
    bridge = source("platform/modern_character_studio_bridge.h")

    require("interactiveStudio" in settings and
            "g_characterPreviewRequest.interactiveStudio" in settings,
            "Offset Studio no longer marks its preview request as interactive")
    require("character_preview_studio" in launcher and
            "characterPreviewInteractiveStudio" in launcher,
            "launcher state no longer carries studio intent into the boot")
    require("Overlay_installCharacterStudio" in main_app,
            "engine handoff no longer installs the focused studio overlay")
    require("cfg->character_preview_studio &&" in boot and
            "MDKR_CHARACTER_PREVIEW_POSE_LIVE" in boot,
            "boot no longer rejects capture/inspection modes masquerading as a live studio")

    require("if (g_overlay.mode == OverlayMode::CharacterStudio) return 0;" in overlay,
            "the studio must not pause the scene it claims to preview live")
    require("Hide controls" in overlay and "Show controls" in overlay and
            "studioControlsCollapsed" in overlay,
            "compact and high-scale users need an unobstructed exact-scene inspection state")
    require("static int onWantsInput(void) { return g_overlay.open ? 1 : 0; }" in overlay,
            "the always-open studio must continue swallowing gameplay input")
    require("g_overlay.renderFrame > g_overlay.studioReturnRequestedFrame" in overlay and
            "g_mdkrCharacterPreviewResult->warmup_complete" in overlay and
            "mdkr_modern_character_player_fit_diagnostics" in overlay,
            "return must wait for a post-edit exact frame with current fit diagnostics")
    require("Return without measurement" in overlay and
            "studioReturnRequestedMillis + 10000u" in overlay and
            "ReturnToLauncherWithoutMeasurement" in overlay and
            "result.warmup_complete = 0" in main_app,
            "a renderer failure must not trap the user in finalization")
    require("MDKR_APP_SMOKE_CHARACTER_STUDIO_TOKEN" in overlay and
            "mdkr64-character-studio-v1" in overlay and
            "exact-character-studio overlay-rendered" in overlay and
            "exact-character-studio smoke requested" in settings and
            "exact-character-studio smoke completed" in main_app and
            "published launcher return" in main_app,
            "the exact composed studio lost its explicit renderer smoke route")

    require("Settings_characterPreviewCurrentFitSignature" in launcher,
            "studio results must be rebound to the fit that actually left the renderer")
    require("!state_.romPlayValidationPassed" in launcher,
            "a successful final ROM check must be consumed before another check can start")
    require("if (interactiveStudio)" in settings and
            "!result.warmup_complete" in settings and
            "performance approval remains a separate clean test" in settings,
            "interactive editor time must not be published as clean performance evidence")
    require("persistedCharacterTuning" in settings and
            "persistedActiveCharacterDraftTuning" in settings and
            "persistCharacterStudioTuning" in settings and
            "rollbackCharacterStudioPersistence" in settings and
            "Settings_commitCharacterOffsetStudio" in settings and
            "Settings_commitCharacterOffsetStudio" in overlay and
            "studioEditorInitialized" in overlay and
            "initializeRuntime" in settings and
            "history.actionApplied" in settings and
            "persistCharacterTuning(entry->id, edit)" in settings,
            "live edits and undo/redo must persist atomically or restore the durable fit")
    require("PR/gbi.h" not in bridge,
            "the app-safe studio bridge regressed into renderer/display-list coupling")

    print("character Offset Studio contract passed: exact live scene, input isolation, "
          "post-edit fit handoff, truthful evidence, and bounded recovery")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
