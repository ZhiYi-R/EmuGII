import hashlib
import os
import re
import shutil
import subprocess
from pathlib import Path

WINDOWS_SYSTEM_DLLS = {
    "cfgmgr32.dll",
    "comctl32.dll",
    "advapi32.dll",
    "dwmapi.dll",
    "bcrypt.dll",
    "comdlg32.dll",
    "crypt32.dll",
    "dwrite.dll",
    "gdiplus.dll",
    "dnsapi.dll",
    "gdi32.dll",
    "glu32.dll",
    "hid.dll",
    "imm32.dll",
    "iphlpapi.dll",
    "kernel32.dll",
    "kernelbase.dll",
    "msvcrt.dll",
    "msimg32.dll",
    "ntdll.dll",
    "ole32.dll",
    "oleaut32.dll",
    "opengl32.dll",
    "rpcrt4.dll",
    "secur32.dll",
    "setupapi.dll",
    "shell32.dll",
    "shlwapi.dll",
    "usp10.dll",
    "user32.dll",
    "version.dll",
    "winmm.dll",
    "win32u.dll",
    "winspool.drv",
    "ws2_32.dll",
}


def to_msys_path(path):
    """Convert a Windows drive path to the MSYS2 /d/... form."""
    path_text = os.fspath(path).replace("\\", "/")
    match = re.match(r"^([A-Za-z]):/?(.*)$", path_text)
    if not match:
        return path_text

    drive = match.group(1).lower()
    rest = match.group(2)
    if rest:
        return f"/{drive}/{rest}"
    return f"/{drive}"


def parse_objdump_dll_names(output):
    """Extract imported DLL names from objdump -p output."""
    names = []
    for line in output.splitlines():
        match = re.search(r"DLL Name:\s*(\S+)", line)
        if match:
            names.append(match.group(1))
    return names


def input_fingerprint(paths):
    """Return a stable content fingerprint for files and directory trees."""
    files = {}

    for entry in paths:
        path = Path(os.fspath(entry))
        if path.is_file():
            candidates = [path]
        elif path.is_dir():
            candidates = (child for child in path.rglob("*") if child.is_file())
        else:
            continue

        for candidate in candidates:
            resolved = candidate.resolve()
            files.setdefault(os.path.normcase(str(resolved)), resolved)

    digest = hashlib.sha256()
    for key in sorted(files):
        path = files[key]
        digest.update(path.as_posix().encode("utf-8"))
        digest.update(b"\0")
        digest.update(path.read_bytes())
        digest.update(b"\0")

    return digest.hexdigest()


def _find_in_path(name, path_env):
    for directory in (path_env or "").split(os.pathsep):
        if not directory:
            continue
        candidate = Path(directory) / name
        if candidate.is_file():
            return candidate
    return None


def resolve_windows_dll_dependencies(dependency_names, path_env,
                                     import_reader=None):
    """Resolve non-system DLL imports from PATH, including transitive imports."""
    resolved = []
    queued = list(dependency_names or [])
    seen_names = set()
    resolved_paths = set()

    while queued:
        name = queued.pop(0)
        key = name.lower()
        if (key in seen_names or key in WINDOWS_SYSTEM_DLLS or
                key.startswith("api-ms-win-")):
            continue
        seen_names.add(key)

        path = _find_in_path(name, path_env)
        if not path:
            continue

        path_key = os.path.normcase(os.path.abspath(path))
        if path_key not in resolved_paths:
            resolved.append(path)
            resolved_paths.add(path_key)

        if import_reader:
            queued.extend(import_reader(path))

    return resolved


def runtime_path_entries_for_bash(bash):
    """Infer MSYS2/Git runtime directories from a compatible bash path."""
    bash_path = Path(bash).resolve()
    parent = bash_path.parent
    roots = []
    seen_roots = set()

    if parent.name.lower() == "bin" and parent.parent.name.lower() == "usr":
        roots.append(parent.parent.parent)
    elif parent.name.lower() == "bin":
        roots.append(parent.parent)

    entries = []
    seen_entries = set()
    for root in roots:
        root_key = os.path.normcase(os.path.abspath(root))
        if root_key in seen_roots:
            continue
        seen_roots.add(root_key)

        for candidate in (
            root / "mingw64" / "bin",
            root / "ucrt64" / "bin",
            root / "clang64" / "bin",
            root / "clangarm64" / "bin",
            root / "mingw32" / "bin",
            root / "usr" / "bin",
            root / "bin",
        ):
            if not candidate.is_dir():
                continue

            key = os.path.normcase(os.path.abspath(candidate))
            if key in seen_entries:
                continue

            seen_entries.add(key)
            entries.append(str(candidate))

    return entries


def prepend_path_entries(path_env, entries):
    """Prepend unique path entries while preserving their original order."""
    combined = []
    seen = set()

    for entry in list(entries) + (path_env or "").split(os.pathsep):
        if not entry:
            continue

        key = os.path.normcase(os.path.abspath(entry))
        if key in seen:
            continue

        seen.add(key)
        combined.append(entry)

    return os.pathsep.join(combined)


def find_executable(name, path_env=None):
    """Resolve an executable from an explicit PATH-like string."""
    path = os.environ.get("PATH", "") if path_env is None else path_env
    resolved = shutil.which(name, path=path)
    if resolved:
        return resolved
    raise FileNotFoundError(f"Executable not found in PATH: {name}")


def collect_release_files(build_dir, runtime_files=None, dependency_names=None,
                          path_env=None, os_name=None, import_reader=None,
                          binary_dest_name=None, dependency_dest_dir=None,
                          runtime_dest_dir=None):
    """Return (source, relative_dest) files needed by the runnable release dir."""
    build_path = Path(build_dir)
    runtime_files = runtime_files or []
    dependency_names = dependency_names or []
    path_env = os.environ.get("PATH", "") if path_env is None else path_env
    os_name = os.name if os_name is None else os_name
    binary_name = "qemu-system-arm.exe" if os_name == "nt" else "qemu-system-arm"
    files = []
    seen = set()

    def add_file(path, dest_name=None, dest_dir=None):
        dest = Path(dest_name or path.name)
        if dest_dir and len(dest.parts) == 1:
            dest = Path(dest_dir) / dest
        key = os.path.normcase(str(dest))
        if key in seen:
            return
        files.append((path, dest))
        seen.add(key)

    binary = build_path / binary_name
    if binary.exists():
        add_file(binary, binary_dest_name)

    if os_name == "nt":
        for dll in sorted(build_path.glob("*.dll")):
            add_file(dll, dest_dir=dependency_dest_dir)
        for dependency in resolve_windows_dll_dependencies(
            dependency_names,
            path_env,
            import_reader=import_reader,
        ):
            add_file(dependency, dependency.name, dependency_dest_dir)

    for runtime_file in runtime_files:
        path = Path(runtime_file)
        if path.exists():
            add_file(path, dest_dir=runtime_dest_dir)

    return files


def _path_bash_candidates(path):
    names = ("bash.exe", "bash") if os.name == "nt" else ("bash",)
    seen = set()

    def emit(candidate):
        key = os.path.normcase(os.path.abspath(candidate))
        if key in seen:
            return None
        seen.add(key)
        return str(candidate)

    for directory in (path or "").split(os.pathsep):
        if not directory:
            continue

        base = Path(directory)
        for name in names:
            candidate = base / name
            if not candidate.is_file():
                continue

            emitted = emit(candidate)
            if emitted:
                yield emitted

        if os.name != "nt":
            continue

        derived = []
        if base.name.lower() == "cmd":
            derived.extend(base.parent / "bin" / name for name in names)

        ancestors = [base] + list(base.parents[:3])
        for ancestor in ancestors:
            derived.extend([
                ancestor / "msys64" / "usr" / "bin" / name
                for name in names
            ])
            derived.extend([
                ancestor / "Git" / "bin" / name
                for name in names
            ])

        for candidate in derived:
            if not candidate.is_file():
                continue

            emitted = emit(candidate)
            if emitted:
                yield emitted


def _is_msys_compatible_bash(bash):
    probe = (
        'case "$(uname -s 2>/dev/null)" in '
        'MINGW*|MSYS*) ;; *) exit 1 ;; esac; '
        'command -v cygpath >/dev/null'
    )
    try:
        result = subprocess.run(
            [bash, "-lc", probe],
            capture_output=True,
            text=True,
            timeout=10,
        )
    except (OSError, subprocess.TimeoutExpired):
        return False

    return result.returncode == 0


def _check_compatible_bash(bash, is_compatible):
    if is_compatible(bash):
        return bash
    raise FileNotFoundError(f"bash is not MSYS2/MINGW compatible: {bash}")


def _has_mingw_compiler(bash):
    names = ("gcc.exe", "gcc") if os.name == "nt" else ("gcc",)

    return any(
        (Path(directory) / name).is_file()
        for directory in runtime_path_entries_for_bash(bash)
        for name in names
    )


def resolve_bash(env=None, is_compatible=None, has_mingw_compiler=None):
    """Resolve the shell used for the QEMU POSIX build steps."""
    if env is None:
        env = os.environ
    if is_compatible is None:
        is_compatible = _is_msys_compatible_bash
    if has_mingw_compiler is None:
        has_mingw_compiler = _has_mingw_compiler

    for name in ("EMUGII_BASH", "MSYS2_BASH"):
        value = env.get(name)
        if value:
            bash = Path(value)
            if not bash.exists():
                raise FileNotFoundError(f"{name} points to a missing bash: {value}")
            return _check_compatible_bash(str(bash), is_compatible)

    fallback = None
    for bash in _path_bash_candidates(env.get("PATH")):
        if is_compatible(bash):
            if has_mingw_compiler(bash):
                return bash
            if fallback is None:
                fallback = bash

    if fallback:
        return fallback

    raise FileNotFoundError(
        "MSYS2/MINGW bash was not found. Add it to PATH or set EMUGII_BASH."
    )
