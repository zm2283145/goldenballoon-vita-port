"""ROM/browser-free regression fixture for timeout diagnostic collection."""
from __future__ import annotations

import ast
import os
from pathlib import Path
import json
from types import SimpleNamespace
import unittest
from unittest.mock import Mock


class CheckFailure(RuntimeError):
    pass


def harness_functions():
    source = Path(__file__).with_name("check_browser_runtime.py").read_text()
    tree = ast.parse(source)
    names = {"add_config_script", "wait_value"}
    functions = [node for node in tree.body if isinstance(node, ast.FunctionDef) and node.name in names]
    assert len(functions) == len(names)
    module = ast.Module(body=[ast.ImportFrom(module="__future__", names=[ast.alias(name="annotations")], level=0), *functions], type_ignores=[])
    ast.fix_missing_locations(module)
    clock = SimpleNamespace(monotonic=Mock(side_effect=[0, 1]), sleep=Mock())
    def require(ok, message):
        if not ok:
            raise CheckFailure(message)
    # Isolate the fixture from the invoking session's diagnostic environment.
    fixture_os = SimpleNamespace(urandom=os.urandom, environ={})
    namespace = dict(time=clock, CheckFailure=CheckFailure, os=fixture_os, json=json, require=require)
    exec(compile(module, "check_browser_runtime.py [isolated functions]", "exec"), namespace)
    return namespace


class StartupDiagnosticsHarnessTests(unittest.TestCase):
    def test_timeout_collects_only_bounded_diagnostics(self):
        namespace = harness_functions()
        cdp = SimpleNamespace(evaluate=Mock(return_value={"lastPhase": "gfx-start-before"}))
        with self.assertRaisesRegex(CheckFailure, "startup diagnostics=.*gfx-start-before"):
            namespace["wait_value"](cdp, "irrelevant", lambda _: False, "first frames", 0)
        expression = cdp.evaluate.call_args.args[0]
        self.assertIn("__mdkrStartupDiagnosticsSnapshot", expression)
        self.assertNotIn("__mdkrTestSnapshot", expression)
        self.assertEqual(cdp.evaluate.call_args.kwargs["timeout"], 2.0)

    def test_diagnostic_failure_preserves_original_timeout(self):
        for error in [CheckFailure("context gone"), OSError("socket closed")]:
            with self.subTest(error=error):
                namespace = harness_functions()
                cdp = SimpleNamespace(evaluate=Mock(side_effect=error))
                with self.assertRaisesRegex(CheckFailure, "timed out waiting for first frames;.*startup diagnostics=None"):
                    namespace["wait_value"](cdp, "irrelevant", lambda _: False, "first frames", 0)

    def test_success_does_not_collect_diagnostics(self):
        namespace = harness_functions()
        namespace["time"].monotonic = Mock(side_effect=[0, 0])
        cdp = SimpleNamespace(closed=False, evaluate=Mock(return_value=5))
        self.assertEqual(namespace["wait_value"](cdp, "frames", lambda value: value == 5, "frames", 1), 5)
        cdp.evaluate.assert_called_once_with("frames", timeout=1)

    def test_preload_defaults_off_and_preserves_explicit_environment_precedence(self):
        cases = [
            ({}, None, False),
            ({}, "0", False),
            ({}, "true", False),
            ({}, "1", True),
            ({"startupDiagnostics": True}, None, True),
            ({"startupDiagnostics": True}, "0", True),
            ({"startupDiagnostics": False}, None, False),
            ({"startupDiagnostics": False}, "1", False),
        ]
        for original, environment, enabled in cases:
            with self.subTest(config=original, environment=environment):
                namespace = harness_functions()
                if environment is not None:
                    namespace["os"].environ["MDKR_TEST_BROWSER_STARTUP_DIAGNOSTICS"] = environment
                before = dict(original)
                cdp = SimpleNamespace(call=Mock(return_value={"identifier": "preload"}))
                namespace["add_config_script"](cdp, original)
                self.assertEqual(original, before)
                source = cdp.call.call_args.args[1]["source"]
                config = json.loads(source.removeprefix("globalThis.__mdkrTestConfig = ").removesuffix(";"))
                self.assertIs(config["startupDiagnostics"], enabled)
                self.assertEqual(config["documentToken"], cdp.expected_document_token)


class StartupDiagnosticsSourceTests(unittest.TestCase):
    """Bind diagnostic witnesses to real calls without replacing their behavior."""

    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[1]
        cls.hook = (root / "platform/web_startup_diagnostics.h").read_text()
        cls.backend = (root / "platform/fast3d/gfx_pc_dkr.c").read_text()
        cls.webgpu = (root / "platform/fast3d/gfx_webgpu.c").read_text()
        cls.tasks = (root / "platform/stubs_dkr.c").read_text()
        cls.platform = (root / "platform/platform_sdl_min.c").read_text()
        cls.shell = (root / "dist/web/mdkr64-shell.js").read_text()

    def section(self, source, start, end="\n}\n"):
        begin = source.index(start)
        return source[begin:source.index(end, begin)]

    def assert_bracketed(self, source, before, call, after):
        opening = f'mdkr_web_startup_phase("{before}");'
        closing = f'mdkr_web_startup_phase("{after}");'
        self.assertEqual(source.count(opening), 1)
        self.assertEqual(source.count(closing), 1)
        self.assertLess(source.index(opening), source.index(call))
        self.assertLess(source.index(call), source.index(closing))

    def test_c_hook_is_opt_in_cached_and_native_noop(self):
        web = self.section(self.hook, "#ifdef __EMSCRIPTEN__", "#else")
        self.assertIn("static int enabled = -1;", web)
        self.assertIn("if (enabled < 0)", web)
        self.assertIn("typeof globalThis.__mdkrStartupTrace === 'function' ? 1 : 0", web)
        self.assertLess(web.index("if (!enabled) return;"), web.index("EM_ASM({"))
        self.assertIn("try {", web)
        self.assertIn("catch (error) {}", web)
        self.assertIn("globalThis.__mdkrStartupTrace(UTF8ToString($0));", web)
        native = self.section(self.hook, "#else", "#endif")
        self.assertRegex(native, r"#define mdkr_web_startup_phase\(phase\)\s+\(\(void\)0\)")
        self.assertNotIn("static inline", native)
        self.assertNotIn("EM_ASM", native)
        self.assertNotIn("printf", native)
        for source in (self.backend, self.webgpu, self.tasks):
            self.assertIn('#include "web_startup_diagnostics.h"', source)

    def test_backend_and_interpreter_witnesses_surround_real_calls(self):
        frame = self.section(self.backend, "bool gfx_start_frame(uint64_t authored_tick)")
        for before, call, after in (
            ("gfx-backend-before", "!gfx_rapi->start_frame()", "gfx-backend-after"),
            ("gfx-shadow-before", "gfx_shadow_capture_begin();", "gfx-shadow-after"),
            ("gfx-interpreter-before", "gfx_dkr_reset_interpreter_state();", "gfx-interpreter-after"),
            ("gfx-retained-before", "gfx_retained_task_capture_begin(", "gfx-retained-after"),
            ("gfx-retained-before", "gfx_presentation_packet_capture_begin(authored_tick);", "gfx-retained-after"),
        ):
            with self.subTest(call=call):
                self.assert_bracketed(frame, before, call, after)
        refusal = self.section(frame, 'mdkr_web_startup_phase("gfx-backend-refused");',
                               'mdkr_web_startup_phase("gfx-backend-after");')
        self.assertIn("return false;", refusal)
        self.assertIn("if (present_sched_replay_armed())", frame)

    def test_webgpu_witnesses_surround_real_setup_work(self):
        frame = self.section(self.webgpu, "static bool wgpu_start_frame(void)")
        self.assertIn('mdkr_web_startup_phase("webgpu-start-enter");', frame)
        for before, call, after in (
            ("callback", "wgpu_consume_callback_failure()", "callback"),
            ("admission", "wgpu_backpressure_check_below(", "admission"),
            ("targets", "WGPUTexture new_scene_tex", "targets"),
            ("targets", "wgpuDeviceCreateTexture(s_device, &td)", "targets"),
            ("targets", "wgpuTextureCreateView(new_scene_tex, NULL)", "targets"),
            ("targets", "s_resolve_view = new_resolve_view;", "targets"),
            ("surface", "!wgpu_configure_surface(out_w, out_h)", "surface"),
            ("uniforms", "wgpu_update_noise_ubo();", "uniforms"),
            ("uniforms", "wgpu_update_light_ubo();", "uniforms"),
            ("encoder", "wgpuDeviceCreateCommandEncoder(s_device, NULL)", "encoder"),
            ("shadow-pass", "wgpu_render_shadow_maps();", "shadow-pass"),
            ("timing", "wgpu_character_gpu_timing_frame_begin(&timestamp_writes)", "timing"),
            ("pass", "wgpuCommandEncoderBeginRenderPass(s_encoder, &rp)", "pass"),
        ):
            with self.subTest(call=call):
                self.assert_bracketed(frame, f"webgpu-{before}-before", call,
                                      f"webgpu-{after}-after")
        self.assertIn("if (!admitted)", frame)
        self.assertIn("if (s_encoder == NULL)", frame)
        self.assertIn("if (s_pass == NULL)", frame)

    def test_task_walk_and_end_markers_do_not_replace_renderer_guards(self):
        task = self.section(self.tasks, "static void sched_dispatch_task(OSMesg msg)")
        self.assertLess(task.index('mdkr_web_startup_phase("gfx-task-enter");'),
                        task.index("gfx_start_frame(t->presentationAuthoredTick)"))
        self.assert_bracketed(task, "gfx-walk-before", "gfx_run((void *)t->task.data_ptr);",
                              "gfx-walk-after")
        self.assert_bracketed(task, "gfx-walk-after", "gfx_end_frame();", "gfx-frame-ended")
        self.assertLess(task.index("if (gfx_renderer_failed())"),
                        task.index('mdkr_web_startup_phase("gfx-walk-before");'))
        self.assertIn("platform_request_exit(EXIT_FAILURE);", task)

    def test_ordinary_raf_keeps_await_and_distinguishes_synthetic_path(self):
        ordinary = self.section(self.platform,
                                "EM_ASYNC_JS(void, platformWaitAnimationFrame,", "\n});")
        self.assertIn("typeof globalThis.__mdkrStartupTrace === 'function'", ordinary)
        self.assertIn("catch (error) {}", ordinary)
        self.assertIn("while (document.visibilityState === 'hidden')", ordinary)
        self.assertEqual(ordinary.count("trace('raf-requested')"), 1)
        self.assertEqual(ordinary.count("trace('raf-resolved')"), 1)
        self.assertRegex(ordinary, r"trace\('raf-requested'\);\s*const timestamp = await new Promise\(\s*\(resolve\) => requestAnimationFrame\(resolve\)\);\s*if \(trace\) trace\('raf-resolved'\);")
        self.assertNotIn("__mdkrWaitAnimationFrame", ordinary)
        synthetic = self.section(self.platform,
                                 "EM_ASYNC_JS(double, platformWaitSyntheticAnimationFrame,", "\n});")
        self.assertIn("return await globalThis.__mdkrWaitAnimationFrame();", synthetic)
        self.assertNotIn("raf-requested", synthetic)
        self.assertNotIn("raf-resolved", synthetic)
        shell_wait = self.section(self.shell,
                                  "globalThis.__mdkrWaitAnimationFrame = async function ()", "\n};")
        self.assertEqual(shell_wait.count('trace("raf-requested")'), 1)
        self.assertEqual(shell_wait.count('trace("raf-resolved")'), 1)
        self.assertRegex(shell_wait, r'trace\("raf-requested"\);\s*const actual = await new Promise\(\(resolve\) => requestAnimationFrame\(resolve\)\);\s*if \(startupDiagnostics\) startupDiagnostics.trace\("raf-resolved"\);')
        dispatch = self.section(self.platform, "static uint64_t browser_wait_animation_frame(void)")
        self.assertIn("if (platformAnimationFrameClockSynthetic())", dispatch)
        self.assertRegex(dispatch, r"platformWaitSyntheticAnimationFrame\(\)\);\s*\} else \{\s*platformWaitAnimationFrame\(\);")


if __name__ == "__main__":
    unittest.main()
