#!/usr/bin/env python3
"""Source bindings for opponent-skill interpretation; no app or ROM execution."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parent.parent


def body(source, signature):
    start = source.index("{", source.index(signature))
    depth = 1
    end = start + 1
    while depth:
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
        end += 1
    return source[start + 1:end - 1]


class AiDifficultyUiBindingTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ui = (ROOT / "platform/app/ui_settings.cpp").read_text()
        cls.game = (ROOT / "platform/enh_ai_difficulty.c").read_text()

    def test_game_resolves_current_snapshot_with_shared_interpreter(self):
        resolve = body(self.game, "static void ai_difficulty_resolve(")
        self.assertIn("if (s_resolved)", resolve)
        self.assertIn("config = mdkr_video_config_current();", resolve)
        self.assertIn("s_arm = mdkr_ai_difficulty_effective_value(value);", resolve)
        self.assertIn("s_scale = MDKR_AI_DIFFICULTY_HARD_SCALE;", resolve)
        self.assertIn("s_scale = MDKR_AI_DIFFICULTY_BRUTAL_SCALE;", resolve)
        self.assertIn("s_scale = 1.0f;", resolve)

    def test_ui_resolves_supplied_value_without_latching_or_writing(self):
        resolver = body(self.ui, "const char *effectiveOptionValue(")
        self.assertIn("key == MDKR_ENH_AI_DIFFICULTY", resolver)
        self.assertIn("mdkr_ai_difficulty_effective_value(value) : value", resolver)
        self.assertNotIn("mdkr_enh_ai_difficulty_arm(", self.ui)
        self.assertNotIn("runtime_set", resolver)
        self.assertNotIn("commitEdit", resolver)
        self.assertNotIn("mdkr_video_config_current", resolver)
        self.assertNotIn("mdkr_video_config_desired", resolver)

    def test_formatted_labels_speech_and_selection_share_interpretation(self):
        formatted = body(self.ui, "void formatValue(")
        label = body(self.ui, "const char *optionLabel(")
        display = body(self.ui, "void displayValue(")
        row = body(self.ui, "bool drawKey(")
        self.assertIn("effectiveOptionValue(key, v->text)", formatted)
        self.assertIn("value = effectiveOptionValue(key, value);", label)
        self.assertIn("formatValue(key, s, v, raw, sizeof(raw));", display)
        self.assertIn("optionLabel(key, raw)", display)
        self.assertIn("effectiveOptionValue(k, editState.text)", row)
        self.assertIn("std::strcmp(opts.items[i].value, effectiveEdit)", row)
        self.assertIn("formatValue(k, s, d, valueBuf, sizeof(valueBuf));", row)
        self.assertIn("displayValue(k, s, d, spoken, sizeof(spoken));", row)
        self.assertIn("ui::SpeakFocusedItem(rowLabel, spoken, spokenHelp);", row)
        self.assertIn("formatValue(k, s, live(k), liveBuf, sizeof(liveBuf));", row)
        self.assertIn("optionLabel(k, valueBuf), optionLabel(k, liveBuf)", row)

    def test_raw_staging_and_locks_are_preserved(self):
        comparison = body(self.ui, "bool differsFromLive(")
        row = body(self.ui, "bool drawKey(")
        self.assertIn("std::strcmp(d->text, l->text) != 0", comparison)
        self.assertIn("mdkr_video_config_runtime_locked(k)", row)
        self.assertIn("if (locked || rumbleProfileUnavailable) ImGui::BeginDisabled();", row)
        self.assertIn('std::snprintf(editState.text, sizeof(editState.text), "%s", d->text);', row)
        self.assertIn("s->scope == MDKR_VIDEO_SCOPE_RESTART && differsFromLive(k, s)", row)
        self.assertIn("MDKR_VIDEO_SOURCE_ENV", row)
        self.assertIn("MDKR_VIDEO_SOURCE_CLI", row)


if __name__ == "__main__":
    unittest.main()
