import os
import re
import subprocess
import tempfile
import unittest
from pathlib import Path

from build_helpers import (
    collect_release_files,
    find_executable,
    input_fingerprint,
    parse_objdump_dll_names,
    prepend_path_entries,
    resolve_bash,
    resolve_windows_dll_dependencies,
    runtime_path_entries_for_bash,
    to_msys_path,
)


class BuildHelperTests(unittest.TestCase):
    def test_qtest_alias_is_always_built(self):
        sconstruct = Path(__file__).resolve().parents[1] / "SConstruct"

        self.assertIn(
            "AlwaysBuild(stmp3770_qtest)",
            sconstruct.read_text(encoding="utf-8"),
            "scons qtest must execute the contract suite on every explicit invocation",
        )

    def test_input_fingerprint_changes_when_a_tracked_file_changes(self):
        with tempfile.TemporaryDirectory() as tmp:
            source_dir = Path(tmp) / "src"
            source_dir.mkdir()
            source = source_dir / "peripheral.c"
            source.write_text("first\n", encoding="utf-8")

            before = input_fingerprint([source_dir])

            source.write_text("second\n", encoding="utf-8")
            after = input_fingerprint([source_dir])

            self.assertNotEqual(before, after)

    def test_input_fingerprint_is_independent_of_input_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            first = root / "first.c"
            second = root / "second.c"
            first.write_text("first\n", encoding="utf-8")
            second.write_text("second\n", encoding="utf-8")

            self.assertEqual(
                input_fingerprint([first, second]),
                input_fingerprint([second, first]),
            )

    def test_env_override_selects_bash_without_hardcoded_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            bash = Path(tmp) / ("bash.exe" if os.name == "nt" else "bash")
            bash.write_text("", encoding="utf-8")

            resolved = resolve_bash(
                {"EMUGII_BASH": str(bash), "PATH": ""},
                is_compatible=lambda path: path == str(bash),
            )

            self.assertEqual(resolved, str(bash))

    def test_path_lookup_finds_bash_when_no_override_is_set(self):
        with tempfile.TemporaryDirectory() as tmp:
            bash_name = "bash.exe" if os.name == "nt" else "bash"
            bash = Path(tmp) / bash_name
            bash.write_text("", encoding="utf-8")

            resolved = resolve_bash(
                {"PATH": tmp},
                is_compatible=lambda path: Path(path).resolve() == bash.resolve(),
            )

            self.assertEqual(Path(resolved).resolve(), bash.resolve())

    def test_path_lookup_skips_incompatible_bash_candidates(self):
        with tempfile.TemporaryDirectory() as bad_tmp:
            with tempfile.TemporaryDirectory() as good_tmp:
                bash_name = "bash.exe" if os.name == "nt" else "bash"
                bad_bash = Path(bad_tmp) / bash_name
                good_bash = Path(good_tmp) / bash_name
                bad_bash.write_text("", encoding="utf-8")
                good_bash.write_text("", encoding="utf-8")

                resolved = resolve_bash(
                    {"PATH": os.pathsep.join([bad_tmp, good_tmp])},
                    is_compatible=lambda path: Path(path).resolve()
                    == good_bash.resolve(),
                )

                self.assertEqual(Path(resolved).resolve(), good_bash.resolve())

    def test_path_lookup_prefers_bash_with_mingw_compiler(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            git_bash = root / "Git" / "bin" / ("bash.exe" if os.name == "nt" else "bash")
            msys_bash = root / "msys64" / "usr" / "bin" / ("bash.exe" if os.name == "nt" else "bash")
            git_bash.parent.mkdir(parents=True)
            msys_bash.parent.mkdir(parents=True)
            git_bash.write_text("", encoding="utf-8")
            msys_bash.write_text("", encoding="utf-8")

            resolved = resolve_bash(
                {"PATH": os.pathsep.join([str(git_bash.parent), str(msys_bash.parent)])},
                is_compatible=lambda path: True,
                has_mingw_compiler=lambda path: Path(path).resolve() == msys_bash.resolve(),
            )

            self.assertEqual(Path(resolved).resolve(), msys_bash.resolve())

    def test_path_lookup_can_derive_git_bash_from_git_cmd_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            git_root = Path(tmp) / "Git"
            cmd_dir = git_root / "cmd"
            bin_dir = git_root / "bin"
            cmd_dir.mkdir(parents=True)
            bin_dir.mkdir(parents=True)
            bash = bin_dir / ("bash.exe" if os.name == "nt" else "bash")
            bash.write_text("", encoding="utf-8")

            resolved = resolve_bash(
                {"PATH": str(cmd_dir)},
                is_compatible=lambda path: Path(path).resolve() == bash.resolve(),
            )

            self.assertEqual(Path(resolved).resolve(), bash.resolve())

    def test_env_override_rejects_incompatible_bash(self):
        with tempfile.TemporaryDirectory() as tmp:
            bash = Path(tmp) / ("bash.exe" if os.name == "nt" else "bash")
            bash.write_text("", encoding="utf-8")

            with self.assertRaisesRegex(FileNotFoundError, "not MSYS2/MINGW"):
                resolve_bash(
                    {"EMUGII_BASH": str(bash), "PATH": ""},
                    is_compatible=lambda path: False,
                )

    def test_windows_drive_path_converts_to_msys_path_without_lowercasing_rest(self):
        windows_path = "D:" + "\\Projects\\EmuGII\\build\\qemu"

        self.assertEqual(
            to_msys_path(windows_path),
            "/d/Projects/EmuGII/build/qemu",
        )

    def test_posix_path_is_left_as_posix_path(self):
        self.assertEqual(to_msys_path("/d/Projects/EmuGII"), "/d/Projects/EmuGII")

    def test_tracked_project_files_do_not_embed_windows_absolute_paths(self):
        repo_root = Path(__file__).resolve().parents[1]
        result = subprocess.run(
            ["git", "ls-files", "--cached", "--others", "--exclude-standard"],
            cwd=repo_root,
            capture_output=True,
            text=True,
            check=True,
        )
        binary_suffixes = {".bin", ".elf", ".exe", ".img", ".png", ".jpg", ".pdf"}
        offenders = []

        for relpath in result.stdout.splitlines():
            path = Path(relpath)
            full_path = repo_root / path
            if not full_path.exists():
                continue
            if path.parts and path.parts[0] == "ThirdParty":
                continue
            if path.suffix.lower() in binary_suffixes:
                continue

            text = full_path.read_text(encoding="utf-8", errors="ignore")
            if re.search(r"(?<![A-Za-z0-9_])[A-Za-z]:[\\/]", text):
                offenders.append(relpath)

        self.assertEqual(offenders, [])

    def test_collect_release_files_includes_binary_dependencies_and_runtime_images(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp) / "qemu" / "build"
            runtime_dir = Path(tmp) / "runtime"
            dep_dir = Path(tmp) / "mingw" / "bin"
            build_dir.mkdir(parents=True)
            runtime_dir.mkdir()
            dep_dir.mkdir(parents=True)

            binary = build_dir / ("qemu-system-arm.exe" if os.name == "nt" else "qemu-system-arm")
            binary.write_text("exe", encoding="utf-8")
            (build_dir / "libglib-2.0-0.dll").write_text("dll", encoding="utf-8")
            (build_dir / "qemu-system-arm.pdb").write_text("debug", encoding="utf-8")
            (dep_dir / "libpixman-1-0.dll").write_text("dll", encoding="utf-8")
            (dep_dir / "kernel32.dll").write_text("system", encoding="utf-8")
            rom = runtime_dir / "rom.bin"
            flash = runtime_dir / "flash.bin"
            rom.write_bytes(b"rom")
            flash.write_bytes(b"flash")

            files = collect_release_files(
                build_dir,
                runtime_files=[rom, flash],
                dependency_names=["libpixman-1-0.dll", "kernel32.dll"],
                path_env=str(dep_dir),
                os_name="nt",
            )

            relpaths = {dest for _, dest in files}

            self.assertIn(Path(binary.name), relpaths)
            self.assertIn(Path("libglib-2.0-0.dll"), relpaths)
            self.assertIn(Path("libpixman-1-0.dll"), relpaths)
            self.assertIn(Path("rom.bin"), relpaths)
            self.assertIn(Path("flash.bin"), relpaths)
            self.assertNotIn(Path("qemu-system-arm.pdb"), relpaths)
            self.assertNotIn(Path("kernel32.dll"), relpaths)

    def test_collect_release_files_can_rename_binary_for_release(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp)
            binary = build_dir / "qemu-system-arm.exe"
            binary.write_text("exe", encoding="utf-8")

            files = collect_release_files(
                build_dir,
                binary_dest_name="EmuGii.exe",
                os_name="nt",
            )

            self.assertEqual(files, [(binary, Path("EmuGii.exe"))])

    def test_collect_release_files_can_place_runtime_and_firmware_files_under_subdirectories(self):
        with tempfile.TemporaryDirectory() as tmp:
            build_dir = Path(tmp) / "build"
            dep_dir = Path(tmp) / "deps"
            runtime_dir = Path(tmp) / "runtime"
            build_dir.mkdir()
            dep_dir.mkdir()
            runtime_dir.mkdir()
            binary = build_dir / "qemu-system-arm.exe"
            local_dll = build_dir / "libgtk-3-0.dll"
            path_dll = dep_dir / "libglib-2.0-0.dll"
            rom = runtime_dir / "rom.bin"
            flash = runtime_dir / "flash.bin"
            binary.write_text("exe", encoding="utf-8")
            local_dll.write_text("dll", encoding="utf-8")
            path_dll.write_text("dll", encoding="utf-8")
            rom.write_bytes(b"rom")
            flash.write_bytes(b"flash")

            files = collect_release_files(
                build_dir,
                runtime_files=[rom, flash],
                dependency_names=[path_dll.name],
                path_env=str(dep_dir),
                os_name="nt",
                binary_dest_name=Path("bin") / "EmuGii-runtime.exe",
                dependency_dest_dir="bin",
                runtime_dest_dir="firmware",
            )

            relpaths = {dest for _, dest in files}

            self.assertIn(Path("bin") / "EmuGii-runtime.exe", relpaths)
            self.assertIn(Path("bin") / local_dll.name, relpaths)
            self.assertIn(Path("bin") / path_dll.name, relpaths)
            self.assertIn(Path("firmware") / "rom.bin", relpaths)
            self.assertIn(Path("firmware") / "flash.bin", relpaths)

    def test_parse_objdump_dll_names_extracts_pe_imports(self):
        output = """
The Import Tables (interpreted .idata section contents)
 DLL Name: libglib-2.0-0.dll
 vma:            Hint    Time      Forward  DLL       First
 DLL Name: KERNEL32.dll
"""

        self.assertEqual(
            parse_objdump_dll_names(output),
            ["libglib-2.0-0.dll", "KERNEL32.dll"],
        )

    def test_find_executable_resolves_from_explicit_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / ("tool.exe" if os.name == "nt" else "tool")
            exe.write_text("", encoding="utf-8")

            resolved = find_executable(exe.name, path_env=str(Path(tmp)))

            self.assertEqual(Path(resolved).resolve(), exe.resolve())

    def test_runtime_path_entries_for_msys_usr_bash_prefers_runtime_dirs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bash_dir = root / "usr" / "bin"
            bash_dir.mkdir(parents=True)
            (root / "mingw64" / "bin").mkdir(parents=True)
            bash = bash_dir / ("bash.exe" if os.name == "nt" else "bash")
            bash.write_text("", encoding="utf-8")

            entries = runtime_path_entries_for_bash(bash)

            self.assertEqual(
                entries,
                [
                    str(root / "mingw64" / "bin"),
                    str(root / "usr" / "bin"),
                ],
            )

    def test_prepend_path_entries_deduplicates_and_preserves_existing_order(self):
        original = os.pathsep.join([
            "C:" + "\\existing",
            "D:" + "\\shared",
            "C:" + "\\existing",
        ])
        combined = prepend_path_entries(
            original,
            [
                "D:" + "\\shared",
                "E:" + "\\new",
                "C:" + "\\existing",
            ],
        )

        self.assertEqual(
            combined.split(os.pathsep),
            [
                "D:" + "\\shared",
                "E:" + "\\new",
                "C:" + "\\existing",
            ],
        )

    def test_resolve_windows_dll_dependencies_walks_transitive_imports(self):
        with tempfile.TemporaryDirectory() as tmp:
            dep_dir = Path(tmp)
            first = dep_dir / "libfirst.dll"
            second = dep_dir / "libsecond.dll"
            first.write_text("", encoding="utf-8")
            second.write_text("", encoding="utf-8")

            def imports(path):
                if Path(path).name == "libfirst.dll":
                    return ["libsecond.dll", "kernel32.dll"]
                return []

            dependencies = resolve_windows_dll_dependencies(
                ["libfirst.dll"],
                str(dep_dir),
                import_reader=imports,
            )

            self.assertEqual(
                [path.name for path in dependencies],
                ["libfirst.dll", "libsecond.dll"],
            )


if __name__ == "__main__":
    unittest.main()
