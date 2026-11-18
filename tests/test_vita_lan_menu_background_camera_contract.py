#!/usr/bin/env python3
"""Guard LAN menu background levels against endpoint view remapping.

Results (checkered flag) and track-select previews animate camera 0 only.
Endpoint-to-canonical view mapping must be limited to the race, otherwise a
non-host endpoint renders an unanimated camera (flat flag, empty preview).
"""

import re
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def main() -> None:
    tracks = (ROOT / "game/src/tracks.c").read_text(encoding="utf-8")
    start = tracks.index("void render_scene(")
    body = tracks[start:tracks.index("\n}\n", start)]

    assignments = re.findall(r"endpointMappedViews\s*=\s*([^;]+);", body)
    assert assignments, "render_scene no longer assigns endpointMappedViews"
    for expr in assignments:
        if "mdkr_net_roster_runtime_active()" in expr:
            assert "get_game_mode() == GAMEMODE_INGAME" in expr, (
                "endpoint view mapping must be gated to GAMEMODE_INGAME: "
                + expr.strip()
            )

    mapping = body.index("mdkr_net_roster_runtime_viewport_to_canonical(")
    guard = body.rfind("endpointMappedViews", 0, mapping)
    assert guard != -1, "viewport_to_canonical must stay behind endpointMappedViews"
    print("test_vita_lan_menu_background_camera_contract: PASS")


if __name__ == "__main__":
    main()
