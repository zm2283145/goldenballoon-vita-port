// launcher_panels.h — the launcher's numeric panel contract, and nothing else.
//
// These indices are public: the nav smoke gates (MDKR_APP_SMOKE_NAV_TARGET,
// tests/check_launcher_tabs.py), Launcher_requestTab callers, and the Online
// Room's return-home path all address panels by number. They are therefore
// APPEND-ONLY -- a renumber silently retargets every one of those callers.
//
// They live in their own header, free of any include, so the pure policy layer
// (app_ui_policy.h) can route panels to destinations without pulling in
// engine_entry.h, rom_validate.h and the LAN transport that ui_launcher.h
// needs. app_ui_policy.cpp compiles standalone into mdkr_app_ui_policy_test;
// keeping that target two translation units is what makes the routing testable
// without a window.
#ifndef MDKR64_LAUNCHER_PANELS_H
#define MDKR64_LAUNCHER_PANELS_H

constexpr int kLauncherPanelPlay = 0;
constexpr int kLauncherPanelOnlineRoom = 1;
constexpr int kLauncherPanelSettings = 2;
constexpr int kLauncherPanelDiagnostics = 3;
constexpr int kLauncherPanelAbout = 4;
// Appended so every existing numeric panel contract remains stable.
constexpr int kLauncherPanelCharacterWorkshop = 5;
// Appended, not inserted: the Content hub is where packs and characters are
// reached from, and the Workshop keeps index 5 so every existing caller and
// smoke target still lands where it always did.
constexpr int kLauncherPanelContent = 6;
constexpr int kLauncherPanelCount = 7;

#endif  // MDKR64_LAUNCHER_PANELS_H
