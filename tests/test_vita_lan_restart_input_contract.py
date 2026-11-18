#!/usr/bin/env python3
"""Guard relay input continuity across a scheduled race restart."""

from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent


def main() -> None:
    lan = (ROOT / "platform/vita_lan.c").read_text(encoding="utf-8")

    assert (
        "#define MDKR_LAN_DEFERRED_INPUT_BUNDLES \\\n"
        "    MDKR_ROLLBACK_MAX_INPUT_AGE_TICKS"
    ) in lan
    assert "bundle.match_epoch == s_controlMatchEpoch" in lan
    assert "defer_input_bundle(&bundle, sequence)" in lan
    assert "is_previous_match_epoch(" in lan
    assert "apply_deferred_input_bundles(matchEpoch)" in lan

    install = lan.index("if (!s_haveLaunch || !install_match_runtime())")
    apply_deferred = lan.index(
        "if (!apply_deferred_input_bundles(matchEpoch))", install
    )
    publish_launch = lan.index("*launch = s_launch;", apply_deferred)
    assert install < apply_deferred < publish_launch

    assert "DEFERRED INPUT OVERFLOW" in lan
    assert "DEFERRED INPUT reason=%u" in lan
    print("test_vita_lan_restart_input_contract: PASS")


if __name__ == "__main__":
    main()
