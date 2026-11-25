"""Shared wire-format and safe authoring regressions; Python standard library."""
from __future__ import annotations

import concurrent.futures
import hashlib
import importlib.util
import json
import subprocess
import sys
import tempfile
import threading
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


sdk = load_module("fcskin_sdk_tests", ROOT / "tools/fcskin.py")
fixtures = load_module("fcskin_fixture_tests", ROOT / "tests/generate_vectors.py")


class SharedVectorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.index = json.loads((ROOT / "test-vectors/index.json").read_text(encoding="utf-8"))

    def test_all_shared_vectors_match_the_wire_contract(self):
        self.assertEqual(self.index["format"], "fangcun.skin-test-vectors")
        self.assertEqual(self.index["version"], 1)
        self.assertEqual(len({v["name"] for v in self.index["vectors"]}), len(self.index["vectors"]))
        for case in self.index["vectors"]:
            with self.subTest(case=case["name"]):
                data = (ROOT / "test-vectors" / case["file"]).read_bytes()
                self.assertEqual(len(data), case["bytes"])
                self.assertEqual(hashlib.sha256(data).hexdigest(), case["sha256"])
                if case["valid"]:
                    manifest, digest = sdk.validate_archive(data)
                    self.assertEqual(manifest["format"], "fangcun.skin")
                    self.assertEqual(digest, case["sha256"])
                else:
                    # Reject only the public validation exception, never a parser crash.
                    with self.assertRaises(sdk.Invalid):
                        sdk.validate_archive(data)

    def test_fixtures_regenerate_byte_for_byte(self):
        by_name = {case["name"]: case for case in self.index["vectors"]}
        generated = fixtures.make_vectors()
        self.assertEqual(set(by_name), {case[0] for case in generated})
        for name, data, valid, reason in generated:
            with self.subTest(case=name):
                self.assertEqual(by_name[name]["sha256"], hashlib.sha256(data).hexdigest())
                self.assertEqual(by_name[name]["bytes"], len(data))
                self.assertEqual(by_name[name]["valid"], valid)
                self.assertEqual(by_name[name]["reason"], reason)

    def test_structural_schema_matches_the_locked_contract(self):
        schema = json.loads((ROOT / "schema-v1.json").read_text(encoding="utf-8"))
        self.assertEqual(schema, fixtures.schema_document())
        self.assertFalse(schema["additionalProperties"])
        self.assertEqual(set(schema["$defs"]["colors"]["required"]), set(sdk.CONTRACT["colors"]))
        self.assertEqual(len(schema["$defs"]["colors"]["properties"]), 18)
        image_roles = schema["$defs"]["theme"]["properties"]["images"]
        self.assertFalse(image_roles["additionalProperties"])
        self.assertEqual(set(image_roles["properties"]), set(sdk.CONTRACT["imageRoles"]))
        self.assertEqual(len(image_roles["properties"]), 8)
        themes = schema["properties"]["themes"]
        self.assertEqual(themes["required"], ["light", "dark"])
        self.assertFalse(themes["additionalProperties"])
        self.assertEqual(schema["properties"]["version"]["type"], "integer")

    def test_archive_size_is_checked_before_parsing(self):
        with self.assertRaisesRegex(sdk.Invalid, "24 MiB"):
            sdk.validate_archive(b"x" * (sdk.LIMITS["archiveBytes"] + 1))


class AuthoringTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="fcskin-sdk-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def create_skin(self, name="source", identifier="fangcun.authoring"):
        folder = self.root / name
        sdk.create_template(folder)
        manifest_path = folder / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["id"] = identifier
        manifest_path.write_text(json.dumps(manifest, ensure_ascii=False), encoding="utf-8")
        return folder

    @staticmethod
    def snapshot(folder):
        return {str(path.relative_to(folder)): path.read_bytes()
                for path in folder.rglob("*") if path.is_file() and not path.is_symlink()}

    def test_template_refuses_to_overwrite_existing_artwork(self):
        source = self.root / "my-artwork"
        source.mkdir()
        original = source / "original.png"
        original.write_bytes(b"original artwork, do not replace")
        before = self.snapshot(source)
        with self.assertRaisesRegex(sdk.Invalid, "目录必须为空"):
            sdk.create_template(source)
        self.assertEqual(self.snapshot(source), before)

    def test_build_is_deterministic_and_preserves_sources(self):
        source = self.create_skin()
        (source / "notes.txt").write_text("Unreferenced author notes stay outside the pack.", encoding="utf-8")
        before = self.snapshot(source)
        outputs = [self.root / "a.fcskin", self.root / "b.FCSKIN"]
        result_a = sdk.build(source, outputs[0])
        result_b = sdk.build(source, outputs[1])
        self.assertEqual(result_a, result_b)
        self.assertEqual(outputs[0].read_bytes(), outputs[1].read_bytes())
        self.assertEqual(self.snapshot(source), before)
        self.assertEqual(set(sdk.referenced_files(result_a[0])), {"manifest.json", "preview.png"})
        self.assertEqual(sdk.validate_archive(outputs[0].read_bytes()), result_a)

    def test_explicit_output_can_be_updated_atomically(self):
        first = self.create_skin("first", "fangcun.first")
        second = self.create_skin("second", "fangcun.second")
        output = self.root / "result.fcskin"
        sdk.build(first, output)
        old = output.read_bytes()
        sdk.build(second, output)
        self.assertNotEqual(output.read_bytes(), old)
        self.assertEqual(sdk.validate_archive(output.read_bytes())[0]["id"], "fangcun.second")
        self.assertEqual(list(self.root.glob("result.fcskin.tmp.*")), [])

    def test_invalid_build_keeps_existing_output_and_originals(self):
        source = self.create_skin()
        output = self.root / "result.fcskin"
        sdk.build(source, output)
        previous = output.read_bytes()
        manifest_path = source / "manifest.json"
        manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
        manifest["themes"].pop("dark")
        manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
        before = self.snapshot(source)
        with self.assertRaises(sdk.Invalid):
            sdk.build(source, output)
        self.assertEqual(output.read_bytes(), previous)
        self.assertEqual(self.snapshot(source), before)
        self.assertEqual(list(self.root.glob("result.fcskin.tmp.*")), [])

    def test_failed_atomic_replace_cleans_only_its_own_temporary(self):
        source = self.create_skin()
        output = self.root / "result.fcskin"
        output.write_bytes(b"previous package")
        other_temporary = self.root / "result.fcskin.tmp.someone-else"
        other_temporary.write_bytes(b"another writer owns this")
        before = self.snapshot(source)
        with mock.patch.object(Path, "replace", side_effect=PermissionError("test write failure")):
            with self.assertRaises(PermissionError):
                sdk.build(source, output)
        self.assertEqual(output.read_bytes(), b"previous package")
        self.assertEqual(other_temporary.read_bytes(), b"another writer owns this")
        self.assertEqual(self.snapshot(source), before)
        self.assertEqual(list(self.root.glob("result.fcskin.tmp.*")), [other_temporary])

    def test_concurrent_builds_use_separate_temporaries(self):
        sources = [self.create_skin("first", "fangcun.first"), self.create_skin("second", "fangcun.second")]
        output = self.root / "shared.fcskin"
        barrier = threading.Barrier(2)
        original_replace = Path.replace
        temporary_names = []
        def simultaneous_replace(path, target):
            temporary_names.append(path.name)
            barrier.wait(timeout=10)
            return original_replace(path, target)
        with mock.patch.object(Path, "replace", simultaneous_replace):
            with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
                results = [pool.submit(sdk.build, source, output) for source in sources]
                for future in results:
                    future.result(timeout=15)
        self.assertEqual(len(set(temporary_names)), 2)
        self.assertIn(sdk.validate_archive(output.read_bytes())[0]["id"], {"fangcun.first", "fangcun.second"})
        self.assertEqual(list(self.root.glob("shared.fcskin.tmp.*")), [])

    def test_symlinked_manifest_or_image_does_not_enter_a_pack(self):
        for relative in ("manifest.json", "preview.png"):
            with self.subTest(path=relative):
                source = self.create_skin(relative.replace(".", "-"))
                target = source / relative
                outside = self.root / (relative + ".outside")
                outside.write_bytes(target.read_bytes())
                target.unlink()
                target.symlink_to(outside)
                output = self.root / "blocked.fcskin"
                with self.assertRaises(sdk.Invalid):
                    sdk.build(source, output)
                self.assertFalse(output.exists())
                self.assertEqual(outside.read_bytes(), target.read_bytes())

    def test_linked_parent_directory_does_not_escape_source(self):
        source = self.create_skin()
        manifest = json.loads((source / "manifest.json").read_text(encoding="utf-8"))
        outside = self.root / "outside"
        outside.mkdir()
        image = outside / "preview.png"
        image.write_bytes(sdk.placeholder_png())
        (source / "assets").symlink_to(outside, target_is_directory=True)
        manifest["preview"] = "assets/preview.png"
        (source / "manifest.json").write_text(json.dumps(manifest), encoding="utf-8")
        with self.assertRaises(sdk.Invalid):
            sdk.build(source, self.root / "blocked.fcskin")
        self.assertEqual(image.read_bytes(), sdk.placeholder_png())
        self.assertFalse((self.root / "blocked.fcskin").exists())

    def test_cli_reports_invalid_files_without_a_traceback(self):
        invalid = ROOT / "test-vectors/manifest_utf8_bom.fcskin"
        result = subprocess.run([sys.executable, str(ROOT / "tools/fcskin.py"), "validate", str(invalid)],
                                capture_output=True, text=True, check=False)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, "")
        self.assertIn("皮肤包检查失败：", result.stderr)
        self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
