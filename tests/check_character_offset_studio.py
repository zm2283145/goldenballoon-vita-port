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
    draft_snapshot = source("platform/app/character_draft_snapshot.cpp")
    draft_header = source("platform/app/character_draft_snapshot.h")
    runtime_h = source("platform/modern_character_runtime.h")
    runtime = source("platform/modern_character_runtime.c")
    entry = source("platform/app/engine_entry.h")
    game = source("game/src/thread3_main.c")
    objects = source("game/src/objects.c")
    webgpu = source("platform/fast3d/gfx_webgpu.c")
    report_h = source("platform/app/character_visual_report.h")
    report = source("platform/app/character_visual_report.cpp")
    preview_cache = source("platform/app/character_preview_cache.cpp")

    require("interactiveStudio" in settings and
            "g_characterPreviewRequest.interactiveStudio" in settings,
            "Offset Studio no longer marks its preview request as interactive")
    require("character_preview_studio" in launcher and
            "characterPreviewInteractiveStudio" in launcher,
            "launcher state no longer carries studio intent into the boot")
    require("Overlay_installCharacterStudio" in main_app,
            "engine handoff no longer installs the focused studio overlay")
    require("cfg->character_preview_studio || cfg->character_motion_review" in boot and
            "MDKR_CHARACTER_PREVIEW_POSE_LIVE" in boot,
            "boot no longer rejects studio/review modes without a preview or capture/inspection modes masquerading as a live studio")

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

    require("Review all motion and scenes" in settings and
            "representativeMotionReviewRoute" in settings and
            "CharacterSceneReviewRun" in settings and
            "Retry current course" in settings and
            "Stop review" in settings and
            "refreshAll" in settings and
            "Opening the next qualified course automatically" in settings and
            "MDKR_CHARACTER_PREVIEW_SCENE_COUNT" in entry and
            "MDKR_CHARACTER_MOTION_REVIEW_SAMPLE_COUNT" in entry,
            "fitting lost its one-action complete semantic review workflow")
    require("ASSET_LEVEL_ANCIENTLAKE" in game and
            "ASSET_LEVEL_GREENWOODVILLAGE" in game and
            "ASSET_LEVEL_SNOWBALLVALLEY" in game and
            "ASSET_LEVEL_WHALEBAY" in game and
            "ASSET_LEVEL_CRESCENTISLAND" in game and
            "ASSET_LEVEL_HOTTOPVOLCANO" in game and
            "ASSET_LEVEL_WINDMILLPLAINS" in game and
            "ASSET_LEVEL_SPACEPORTALPHA" in game and
            "ASSET_LEVEL_EVERFROSTPEAK" in game,
            "the three vehicle review pressures lost their qualified course mappings")
    require("character_motion_review" in launcher and
            "character_motion_review_result" in launcher and
            "g_mdkrCharacterMotionReviewResult" in game,
            "representative motion intent or value-owned evidence was dropped at a layer boundary")
    require("race.steer\", 0u" in game and
            "race.steer\", 1000u" in game and
            "race.airborne\", 500u" in game and
            "race.land\", 500u" in game and
            "race.finish_win\", 500u" in game and
            "race.reverse\", 500u" in game and
            "race.boost\", 500u" in game and
            "race.damage\", 500u" in game and
            "race.item\", 500u" in game and
            "race.spin\", 500u" in game and
            "race.finish_lose\", 500u" in game and
            "select.idle\", 500u" in game and
            "select.hover\", 500u" in game and
            "select.confirm\", 500u" in game and
            "WORKSHOP_MOTION_REVIEW_SETTLE_DRAWS 60u" in game,
            "semantic review no longer settles every select and race state")
    require("mdkr_modern_character_inspection_pose_settled(0)" in game and
            "mdkr_modern_character_inspection_pose_settled" in runtime_h and
            "slot->inspection_generation == s_inspection_generation" in runtime,
            "representative review lacks an engine-owned exact-pose settling witness")
    require("representativeMotionReady" in settings and
            "mdkr-character-fit-review-v6-contact-stability" in settings and
            "all 33 exact race-and-scene samples" in settings and
            "idle, hover, and confirm" in settings and
            "currentCharacterMotionReview" in settings and
            "value.fitSha256 == fit" in settings and
            "value.presentationSha256 == presentation" in settings,
            "vehicle approval can bypass current representative renderer evidence or retain a pre-stability active approval")
    require("mdkr-fit-history-v8" in settings and
            "kFitContactStabilityVersion = 17u" in draft_snapshot and
            "fitContactStabilityContractPresent" in draft_header and
            "parsed.reviewedContexts = 0u" in draft_snapshot,
            "older approvals can inherit the settled contact-stability meaning")
    require("characterMotionReviewContactStabilityValid" in settings and
            "Residual stability:" in settings and
            "contactStabilityMeasured" in settings and
            "contact-quality exception" in settings,
            "settled contact instability can disappear between exact evidence, guidance, and approval")
    require("characterPreviewCameraReviewFlags" in settings and
            "camera_landmark_clip_flags" in settings and
            "Gameplay-camera framing · Exact clipping measured" in settings and
            "wheel, skid, propeller, held object, or piece of scenery" in settings,
            "unusual anatomy can lose its head-clipping warning or attachment review scope between exact diagnostics and approval")
    require("CharacterWorkshop_suggestContacts" in settings and
            "Measured starting point" in settings and
            "Complete scene review" in settings and
            "Current exact pose" in settings and
            "Apply measured offsets" in settings and
            "smallest constant least-squares correction" in settings and
            "Positive values move the named target" in settings and
            '"X %+.1f mm · Y %+.1f mm · Z %+.1f mm"' in settings and
            "invalidates prior exact evidence until retested" in settings and
            "contactSuggestion.withinLimits" in settings and
            "spokenProposal.c_str()" in settings,
            "exact contact residuals no longer lead to a bounded, explicit, reversible starting point")

    require("Capture registered comparison pair" in settings and
            "CharacterComparisonRun" in settings and
            "run.awaitingDonor = false" in settings and
            "result.view_yaw_degrees" in settings and
            "Comparison capture in progress" in settings and
            "Retry registered comparison pair" in settings and
            "Stop comparison" in settings and
            "preparePreserving" in settings and
            "bindAndStoreInlineCharacterCapture" in settings and
            "retirePreviousInlineCharacterCaptures" in settings and
            "capture->renderProduct == reportProduct" in settings and
            "retainedPath == primary ? alternate : primary" in preview_cache and
            "CustomModelAlpha" in preview_cache,
            "Offset Studio lost its bounded one-action donor/custom comparison workflow")
    require("reference_only" in runtime and
            "reference_draws" in runtime and
            "mdkr_workshop_preview_reference_enabled" in objects and
            "!draw->reference_only" in webgpu,
            "donor comparison no longer prepares the exact custom renderer witness while suppressing only its pixels")
    require("sceneRegistrationSha256" in report_h and
            "registeredComparison" in report_h and
            "custom.sceneRegistrationSha256 ==" in report and
            "CharacterVisualReport::registeredComparison(custom, donor)" in settings and
            "SliderInt" in settings and "Donor %d%%" in settings,
            "the comparison UI can infer an overlay without an exact shared witness or expose an inaccurate blend control")
    require("result.reference_draws != 0u" in settings and
            "A donor-reference result reached a custom-character route" in settings and
            "Unavailable for retail donor references" in settings,
            "comparison-only donor evidence can leak into custom approval or portrait workflows")

    print("character Offset Studio contract passed: exact live scene, input isolation, "
          "post-edit fit handoff, bounded measured contact starting point, "
          "one-action complete semantic and registered comparison evidence, "
          "truthful approval, and bounded recovery")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
