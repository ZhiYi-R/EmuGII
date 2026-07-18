# EmuGII SCons 构建系统
# 用于构建 STMP3770 QEMU 模拟器

import os
import shlex
import shutil
import subprocess
import sys

from build_helpers import (
    collect_release_files,
    find_executable,
    input_fingerprint,
    parse_objdump_dll_names,
    prepend_path_entries,
    resolve_bash,
    runtime_path_entries_for_bash,
    to_msys_path,
)

# 项目路径
PROJECT_ROOT = Dir('#').abspath
QEMU_SOURCE = os.path.join(PROJECT_ROOT, 'ThirdParty', 'qemu')
BUILD_DIR = os.path.join(PROJECT_ROOT, 'build')
QEMU_BUILD = os.path.join(BUILD_DIR, 'qemu')
QEMU_BUILDDIR = os.path.join(QEMU_BUILD, 'build')

# Windows 构建判定：原生 Windows Python 的 IS_WINDOWS_BUILD；在 MSYS2/MINGW64
# shell 里 os.name 可能为 'posix'，但 MSYSTEM 环境变量会暴露 MINGW*/UCRT*/CLANG*。
_MSYSTEM = os.environ.get('MSYSTEM', '')
IS_WINDOWS_BUILD = (
    os.name == 'nt' or
    _MSYSTEM.upper().startswith(('MINGW', 'UCRT', 'CLANG'))
)

RELEASE_DIR = os.path.join(BUILD_DIR, 'EmuGii')
RELEASE_BINARY = 'EmuGii.exe' if IS_WINDOWS_BUILD else 'EmuGii'
RELEASE_RUNTIME_DIR = 'bin' if IS_WINDOWS_BUILD else ''
RELEASE_FIRMWARE_DIR = 'firmware'
RELEASE_RUNTIME_BINARY = (
    os.path.join(RELEASE_RUNTIME_DIR, 'EmuGii-runtime.exe')
    if IS_WINDOWS_BUILD else RELEASE_BINARY
)
PATCHES_DIR = os.path.join(PROJECT_ROOT, 'patches')
SRC_DIR = os.path.join(PROJECT_ROOT, 'src')
TESTS_DIR = os.path.join(PROJECT_ROOT, 'tests')
SRC_SCONSOURCES = Glob('src/**/*', strings=True) + Glob('src/**/**/*', strings=True)

# SCons 环境
env = Environment(ENV=os.environ)

# 构建类型：默认 release；通过 scons debug=1 或 EMUGII_DEBUG=1 启用 debug
DEBUG_BUILD = ARGUMENTS.get('debug', os.environ.get('EMUGII_DEBUG', '0')).lower() in (
    '1', 'yes', 'true', 'on')
QEMU_BUILD_TYPE = 'debug' if DEBUG_BUILD else 'release'
QEMU_DEBUG_FLAG = '--enable-debug' if DEBUG_BUILD else ''


def prepare_bash_env(base_env):
    """Return an env dict and a compatible bash with runtime PATH entries."""
    env_vars = dict(base_env)
    bash = resolve_bash(env_vars)
    runtime_entries = runtime_path_entries_for_bash(bash)

    if any(entry.lower().endswith('mingw64\\bin') for entry in runtime_entries):
        env_vars.setdefault('MSYSTEM', 'MINGW64')
    env_vars.setdefault('CHERE_INVOKING', '1')
    env_vars['PATH'] = prepend_path_entries(
        env_vars.get('PATH', os.environ.get('PATH', '')),
        runtime_entries + [os.path.dirname(bash)],
    )
    return env_vars, bash

# 辅助函数：复制目录
def copy_tree(src, dst):
    """复制整个目录树"""
    import platform

    if os.path.exists(dst):
        print(f"清理现有目录: {dst}")
        # 在 Git Bash/MSYS2 下使用 rm -rf 更可靠
        if platform.system() != 'Windows' or os.environ.get('MSYSTEM'):
            result = subprocess.run(['rm', '-rf', dst], capture_output=True)
            if result.returncode != 0:
                print(f"  警告: rm 失败，尝试 Python 方法")
                try:
                    shutil.rmtree(dst)
                except Exception as e:
                    print(f"  错误: 无法清理目录: {e}")
                    return False
        else:
            try:
                shutil.rmtree(dst)
            except Exception as e:
                print(f"  错误: 无法清理目录: {e}")
                return False

    print(f"复制 {src} -> {dst} ...")

    # 优先使用 cp 命令 (在 MSYS2/Git Bash 下更快更可靠)
    if platform.system() != 'Windows' or os.environ.get('MSYSTEM'):
        result = subprocess.run(['cp', '-r', src, dst], capture_output=True)
        if result.returncode == 0:
            print(f"  复制完成")
            return True
        else:
            print(f"  cp 失败，尝试 Python 方法: {result.stderr.decode()}")

    # 回退到 Python 方法
    try:
        shutil.copytree(src, dst, symlinks=True, dirs_exist_ok=True)
        print(f"  复制完成")
        return True
    except Exception as e:
        print(f"错误: 复制失败: {e}")
        return False

# 辅助函数：应用补丁
def apply_patches(target, source, env):
    """重置 QEMU 工作树、应用构建补丁，并同步本仓源码到 QEMU 源树"""
    # source[0] 是 .copied 标记文件，QEMU 目录是其父目录
    qemu_dir = os.path.dirname(str(source[0]))
    tool_env, _ = prepare_bash_env(env['ENV'])
    qemu_dir_unix = to_msys_path(os.path.abspath(qemu_dir))
    patch_exe = find_executable('patch', tool_env['PATH'])

    files_to_copy = [
        ('src/include/hw/stmp3770_profile.h', 'include/hw/stmp3770_profile.h'),
        ('src/include/hw/arm/stmp3770.h', 'include/hw/arm/stmp3770.h'),
        ('src/include/hw/arm/stmp3770_uartdbg.h', 'include/hw/arm/stmp3770_uartdbg.h'),
        ('src/include/hw/arm/stmp3770_uartapp.h', 'include/hw/arm/stmp3770_uartapp.h'),
        ('src/include/hw/audio/stmp3770_audio.h', 'include/hw/audio/stmp3770_audio.h'),
        ('src/include/hw/audio/stmp3770_spdif.h', 'include/hw/audio/stmp3770_spdif.h'),
        ('src/include/hw/display/stmp3770_lcdif.h', 'include/hw/display/stmp3770_lcdif.h'),
        ('src/include/hw/display/hp39gii_frontpanel.h', 'include/hw/display/hp39gii_frontpanel.h'),
        ('src/include/hw/gpio/stmp3770_pinctrl.h', 'include/hw/gpio/stmp3770_pinctrl.h'),
        ('src/include/hw/intc/stmp3770_icoll.h', 'include/hw/intc/stmp3770_icoll.h'),
        ('src/include/hw/misc/stmp3770_clkctrl.h', 'include/hw/misc/stmp3770_clkctrl.h'),
        ('src/include/hw/misc/stmp3770_digctl.h', 'include/hw/misc/stmp3770_digctl.h'),
        ('src/include/hw/misc/stmp3770_dcp.h', 'include/hw/misc/stmp3770_dcp.h'),
        ('src/include/hw/misc/stmp3770_dri.h', 'include/hw/misc/stmp3770_dri.h'),
        ('src/include/hw/misc/stmp3770_lradc.h', 'include/hw/misc/stmp3770_lradc.h'),
        ('src/include/hw/misc/stmp3770_power.h', 'include/hw/misc/stmp3770_power.h'),
        ('src/include/hw/misc/stmp3770_ocotp.h', 'include/hw/misc/stmp3770_ocotp.h'),
        ('src/include/hw/rtc/stmp3770_rtc.h', 'include/hw/rtc/stmp3770_rtc.h'),
        ('src/include/hw/timer/stmp3770_timer.h', 'include/hw/timer/stmp3770_timer.h'),
        ('src/include/hw/timer/stmp3770_pwm.h', 'include/hw/timer/stmp3770_pwm.h'),
        ('src/hw/timer/stmp3770_pwm.c', 'hw/timer/stmp3770_pwm.c'),
        ('src/include/hw/usb/stmp3770_usbphy.h', 'include/hw/usb/stmp3770_usbphy.h'),
        ('src/include/hw/usb/stmp3770_usb.h', 'include/hw/usb/stmp3770_usb.h'),
        ('src/hw/usb/stmp3770_usbphy.c', 'hw/usb/stmp3770_usbphy.c'),
        ('src/hw/usb/stmp3770_usb.c', 'hw/usb/stmp3770_usb.c'),
        ('src/include/hw/dma/stmp3770_dma.h', 'include/hw/dma/stmp3770_dma.h'),
        ('src/include/hw/block/stmp3770_gpmi.h', 'include/hw/block/stmp3770_gpmi.h'),
        ('src/include/hw/i2c/stmp3770_i2c.h', 'include/hw/i2c/stmp3770_i2c.h'),
        ('src/include/hw/ssi/stmp3770_ssp.h', 'include/hw/ssi/stmp3770_ssp.h'),
        ('src/hw/arm/stmp3770.c', 'hw/arm/stmp3770.c'),
        ('src/hw/arm/stmp3770_uartdbg.c', 'hw/arm/stmp3770_uartdbg.c'),
        ('src/hw/arm/stmp3770_uartapp.c', 'hw/arm/stmp3770_uartapp.c'),
        ('src/hw/arm/stmp3770-board.c', 'hw/arm/stmp3770-board.c'),
        ('src/hw/audio/stmp3770_audio.c', 'hw/audio/stmp3770_audio.c'),
        ('src/hw/audio/stmp3770_spdif.c', 'hw/audio/stmp3770_spdif.c'),
        ('src/hw/display/stmp3770_lcdif.c', 'hw/display/stmp3770_lcdif.c'),
        ('src/hw/display/hp39gii_frontpanel.c', 'hw/display/hp39gii_frontpanel.c'),
        ('src/hw/gpio/stmp3770_pinctrl.c', 'hw/gpio/stmp3770_pinctrl.c'),
        ('src/hw/intc/stmp3770_icoll.c', 'hw/intc/stmp3770_icoll.c'),
        ('src/hw/misc/stmp3770_dcp.c', 'hw/misc/stmp3770_dcp.c'),
        ('src/hw/misc/stmp3770_dri.c', 'hw/misc/stmp3770_dri.c'),
        ('src/hw/misc/stmp3770_clkctrl.c', 'hw/misc/stmp3770_clkctrl.c'),
        ('src/hw/misc/stmp3770_digctl.c', 'hw/misc/stmp3770_digctl.c'),
        ('src/hw/misc/stmp3770_lradc.c', 'hw/misc/stmp3770_lradc.c'),
        ('src/hw/misc/stmp3770_power.c', 'hw/misc/stmp3770_power.c'),
        ('src/hw/misc/stmp3770_ocotp.c', 'hw/misc/stmp3770_ocotp.c'),
        ('src/hw/rtc/stmp3770_rtc.c', 'hw/rtc/stmp3770_rtc.c'),
        ('src/hw/timer/stmp3770_timer.c', 'hw/timer/stmp3770_timer.c'),
        ('src/hw/dma/stmp3770_dma.c', 'hw/dma/stmp3770_dma.c'),
        ('src/hw/block/stmp3770_gpmi.c', 'hw/block/stmp3770_gpmi.c'),
        ('src/hw/i2c/stmp3770_i2c.c', 'hw/i2c/stmp3770_i2c.c'),
        ('src/hw/ssi/stmp3770_ssp.c', 'hw/ssi/stmp3770_ssp.c'),
        ('src/contrib/plugins/mmio_profile.c', 'contrib/plugins/mmio_profile.c'),
    ]

    # 将 QEMU 工作树统一归一化为 LF，避免 Windows 下的 CRLF 导致补丁/编译异常
    print(">>> 归一化 QEMU 源码行尾到 LF...")
    subprocess.run(
        ['git', '-C', qemu_dir, 'config', 'core.autocrlf', 'false'],
        capture_output=True,
        env=tool_env,
    )
    result = subprocess.run(
        ['git', '-C', qemu_dir, 'checkout', '--', '.'],
        capture_output=True, text=True, env=tool_env
    )
    if result.returncode != 0:
        print("  警告: QEMU 行尾归一化失败，继续尝试应用补丁:")
        print(result.stderr)

    # 应用尚未应用过的 patches/ 下补丁
    print(">>> 应用构建系统补丁...")
    patch_files = sorted([
        f for f in os.listdir(PATCHES_DIR)
        if f.endswith('.patch')
    ])

    if not patch_files:
        print("  警告: patches/ 目录下没有 .patch 文件")

    for patch_file in patch_files:
        patch_path = os.path.join(PATCHES_DIR, patch_file)
        patch_path_unix = to_msys_path(os.path.abspath(patch_path))
        print(f"  应用补丁: {patch_file}")

        result = subprocess.run(
            [patch_exe, '-p1', '-d', qemu_dir_unix, '--forward', '-i', patch_path_unix],
            capture_output=True,
            text=True,
            env=tool_env
        )
        output = result.stdout + result.stderr
        if result.returncode != 0:
            print(f"  错误: 补丁应用失败 {patch_file}:")
            print(output)
            return 1

        print(f"  成功: {patch_file}")

    # 构建补丁应用完成后，再同步我们维护的 STMP3770 源文件，
    # 避免前面的 git checkout 覆盖本仓最新实现。
    print(">>> 同步 STMP3770 源文件到 QEMU 树...")
    for src_file, dst_file in files_to_copy:
        src_path = os.path.join(PROJECT_ROOT, src_file)
        dst_path = os.path.join(qemu_dir, dst_file)
        os.makedirs(os.path.dirname(dst_path), exist_ok=True)
        shutil.copy2(src_path, dst_path)
        print(f"  已复制: {dst_file}")

    marker_fingerprint = input_fingerprint([str(node) for node in source])
    with open(str(target[0]), 'w') as f:
        f.write(f"Patched {marker_fingerprint}\n")

    return None

# 辅助函数：配置 QEMU
def configure_qemu(target, source, env):
    """配置 QEMU 构建"""
    # source[0] 是 .patched 标记文件，QEMU 目录是其父目录
    qemu_dir = os.path.dirname(str(source[0]))
    # target[0] 是 .configured 标记文件，构建目录是其父目录
    build_dir = os.path.dirname(str(target[0]))

    os.makedirs(build_dir, exist_ok=True)

    print(">>> 配置 QEMU...")

    build_dir_unix = to_msys_path(os.path.abspath(build_dir))

    print(f"  使用路径: {build_dir_unix}")

    qemu_dir_unix = to_msys_path(os.path.abspath(qemu_dir))
    tool_env, bash = prepare_bash_env(env['ENV'])

    configure_cmd = [
        bash, '-lc',
        'cd {} && CC=gcc CXX=g++ {} --target-list=arm-softmmu --enable-plugins -Dbuildtype={} {} --disable-werror --disable-vhost-user --disable-libvduse --disable-guest-agent'.format(
            shlex.quote(build_dir_unix),
            shlex.quote(f'{qemu_dir_unix}/configure'),
            QEMU_BUILD_TYPE,
            QEMU_DEBUG_FLAG
        )
    ]

    result = subprocess.run(
        configure_cmd,
        env=tool_env
    )

    if result.returncode != 0:
        print("错误: QEMU 配置失败")
        return result.returncode

    # 创建标记文件
    with open(str(target[0]), 'w') as f:
        f.write("Configured\n")

    return None

# 辅助函数:编译 QEMU
def build_qemu(target, source, env):
    """编译 QEMU"""
    build_dir = os.path.dirname(str(source[0]))

    print(">>> 编译 QEMU...")

    # 获取 CPU 核心数
    import multiprocessing
    nproc = multiprocessing.cpu_count()

    build_dir_unix = to_msys_path(os.path.abspath(build_dir))
    tool_env, bash = prepare_bash_env(env['ENV'])

    result = subprocess.run(
        [
            bash, '-lc',
            f'cd {shlex.quote(build_dir_unix)} && ninja qemu-system-arm.exe -j{nproc}'
        ],
        env=tool_env
    )

    if result.returncode != 0:
        print("错误: QEMU 编译失败")
        return result.returncode

    # 创建标记文件
    qemu_binary = os.path.join(build_dir, 'qemu-system-arm')
    if os.path.exists(qemu_binary) or os.path.exists(qemu_binary + '.exe'):
        with open(str(target[0]), 'w') as f:
            f.write("Build complete\n")
        return None
    else:
        print("错误: QEMU 二进制文件未生成")
        return 1

def package_release(target, source, env):
    """Copy the runnable QEMU binary and runtime files to build/EmuGii."""
    build_dir = os.path.dirname(str(source[0]))
    release_dir = os.path.dirname(str(target[0]))
    launcher_src = os.path.join(SRC_DIR, 'tools', 'emugii-launcher.c')
    launcher_path = os.path.join(release_dir, RELEASE_BINARY)
    root_rom = os.path.join(PROJECT_ROOT, 'rom.bin')
    root_flash = os.path.join(PROJECT_ROOT, 'flash.bin')
    fixture_rom = os.path.join(PROJECT_ROOT, 'tests', 'ExistOS', 'hypervisor-rom.bin')
    fixture_flash = os.path.join(PROJECT_ROOT, 'tests', 'ExistOS', 'flash.initial.bin')
    runtime_files = [
        root_rom if os.path.exists(root_rom) else fixture_rom,
        root_flash if os.path.exists(root_flash) else fixture_flash,
    ]
    binary_name = 'qemu-system-arm.exe' if IS_WINDOWS_BUILD else 'qemu-system-arm'
    binary_path = os.path.join(build_dir, binary_name)
    dependency_names = []
    tool_env, bash = prepare_bash_env(env['ENV'])
    dependency_path = tool_env.get('PATH', os.environ.get('PATH', ''))
    dependency_reader = None

    if IS_WINDOWS_BUILD and os.path.exists(binary_path):
        if bash:
            def dependency_reader(path):
                path_unix = to_msys_path(os.path.abspath(path))
                result = subprocess.run(
                    [
                        bash, '-lc',
                        'objdump -p {}'.format(shlex.quote(path_unix))
                    ],
                    capture_output=True,
                    text=True,
                    env=tool_env
                )
                if result.returncode != 0:
                    return []
                return parse_objdump_dll_names(result.stdout)

            binary_path_unix = to_msys_path(os.path.abspath(binary_path))
            result = subprocess.run(
                [
                    bash, '-lc',
                    'objdump -p {}'.format(shlex.quote(binary_path_unix))
                ],
                capture_output=True,
                text=True,
                env=tool_env
            )
        else:
            try:
                result = subprocess.run(
                    ['objdump', '-p', binary_path],
                    capture_output=True,
                    text=True
                )
            except FileNotFoundError:
                result = None
        if result and result.returncode == 0:
            dependency_names = parse_objdump_dll_names(result.stdout)
        else:
            print("  警告: objdump 依赖扫描失败，只复制构建目录中的 DLL")

    print(f">>> 复制运行产物到 {release_dir} ...")
    if os.path.exists(release_dir):
        shutil.rmtree(release_dir)
    os.makedirs(release_dir, exist_ok=True)

    copied = 0
    for src, rel_dst in collect_release_files(
        build_dir,
        runtime_files=runtime_files,
        dependency_names=dependency_names,
        path_env=dependency_path,
        import_reader=dependency_reader,
        os_name='nt' if IS_WINDOWS_BUILD else 'posix',
        binary_dest_name=RELEASE_RUNTIME_BINARY,
        dependency_dest_dir=RELEASE_RUNTIME_DIR or None,
        runtime_dest_dir=RELEASE_FIRMWARE_DIR,
    ):
        dst_path = rel_dst
        if os.path.basename(os.fspath(src)) == 'hypervisor-rom.bin':
            dst_path = os.path.join(os.path.dirname(os.fspath(rel_dst)), 'rom.bin')
        elif os.path.basename(os.fspath(src)) == 'flash.initial.bin':
            dst_path = os.path.join(os.path.dirname(os.fspath(rel_dst)), 'flash.bin')

        dst_name = os.fspath(dst_path)
        dst = os.path.join(release_dir, dst_name)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        print(f"  已复制: {dst_name}")
        copied += 1

    if IS_WINDOWS_BUILD:
        try:
            gcc_exe = find_executable('gcc', tool_env['PATH'])
            result = subprocess.run(
                [
                    gcc_exe,
                    '-municode',
                    '-O2',
                    '-Wall',
                    '-Wextra',
                    launcher_src,
                    '-lshell32',
                    '-o',
                    launcher_path,
                ],
                capture_output=True,
                text=True,
                env=tool_env,
            )
        except FileNotFoundError:
            print("错误: 未找到 gcc，无法生成 EmuGii.exe 启动器")
            return 1
        if result.returncode != 0:
            print("错误: EmuGii.exe 启动器编译失败")
            if result.stdout:
                print(result.stdout)
            if result.stderr:
                print(result.stderr)
            return result.returncode
        print(f"  已生成: {RELEASE_BINARY}")
        copied += 1

    if copied == 0:
        print("错误: 没有可复制的运行产物")
        return 1

    with open(str(target[0]), 'w') as f:
        f.write("Packaged\n")

    return None


def run_stmp3770_qtest(target, source, env):
    """Run the host-side STMP3770 qtest contract checks."""
    marker_path = str(target[0])
    qemu_build_dir = os.path.abspath(os.path.dirname(str(source[0])))
    env_vars, _ = prepare_bash_env(env['ENV'])

    env_vars['EMUGII_QEMU_BINARY'] = os.path.join(
        qemu_build_dir,
        'qemu-system-arm.exe' if IS_WINDOWS_BUILD else 'qemu-system-arm',
    )
    env_vars['EMUGII_QEMU_CWD'] = qemu_build_dir

    qtest_filter = os.environ.get('EMUGII_QTEST_FILTER')
    if qtest_filter:
        env_vars['EMUGII_QTEST_FILTER'] = qtest_filter

    # Prefer running pytest through uv; fall back to the active python interpreter.
    uv_exe = shutil.which('uv', path=env_vars.get('PATH'))
    if uv_exe:
        cmd = [uv_exe, 'run', 'pytest', 'tests/stmp3770_contract', '-q']
    else:
        python_exe = sys.executable
        cmd = [python_exe, '-m', 'pytest', 'tests/stmp3770_contract', '-q']

    print(">>> 运行 STMP3770 qtest 契约回归...")
    result = subprocess.run(
        cmd,
        cwd=PROJECT_ROOT,
        env=env_vars,
    )

    if result.returncode != 0:
        print("错误: STMP3770 qtest 失败")
        return result.returncode

    with open(marker_path, 'w') as f:
        f.write("PASS\n")

    return None

# 构建步骤定义

# 1. 复制 QEMU 源码树
def copy_qemu_action(target, source, env):
    """复制 QEMU 源码树并创建标记文件"""
    if not copy_tree(str(source[0]), os.path.dirname(str(target[0]))):
        return 1
    with open(str(target[0]), 'w') as f:
        f.write("Copied\n")
    return None

copy_qemu = env.Command(
    os.path.join(QEMU_BUILD, '.copied'),
    Dir(QEMU_SOURCE),
    copy_qemu_action
)

# 2. 应用补丁
patched_qemu = env.Command(
    os.path.join(QEMU_BUILD, '.patched'),
    [copy_qemu, SRC_SCONSOURCES, Glob('patches/*.patch', strings=True)],
    apply_patches
)

# 3. 配置 QEMU
configured_qemu = env.Command(
    os.path.join(QEMU_BUILDDIR, '.configured'),
    [patched_qemu, Value(f'buildtype={QEMU_BUILD_TYPE}')],
    configure_qemu
)

# 4. 编译 QEMU
built_qemu = env.Command(
    os.path.join(QEMU_BUILDDIR, '.built'),
    [configured_qemu, patched_qemu],
    build_qemu
)

packaged_qemu = env.Command(
    os.path.join(RELEASE_DIR, '.packaged'),
    [built_qemu, os.path.join(SRC_DIR, 'tools', 'emugii-launcher.c')],
    package_release
)

stmp3770_qtest = env.Command(
    os.path.join(BUILD_DIR, 'tests', '.stmp3770_qtest'),
    [built_qemu],
    run_stmp3770_qtest
)
AlwaysBuild(stmp3770_qtest)

# 默认目标
Default(packaged_qemu)

# 清理目标
env.Clean(packaged_qemu, BUILD_DIR)

# 别名
env.Alias('qemu', built_qemu)
env.Alias('package', packaged_qemu)
env.Alias('qtest', stmp3770_qtest)
env.Alias('clean', [], Delete(BUILD_DIR))

# ============================================================
# 测试程序构建
# ============================================================

# ARM 交叉编译环境
arm_env = Environment(
    tools=['cc', 'link', 'ar', 'as'],
    CC='arm-none-eabi-gcc',
    AS='arm-none-eabi-as',
    LD='arm-none-eabi-ld',
    LINK='$LD',
    OBJCOPY='arm-none-eabi-objcopy',
    ENV=os.environ
)

# ARM 编译标志
arm_env.Append(
    CFLAGS=['-mcpu=arm926ej-s', '-mthumb-interwork', '-nostdlib',
            '-ffreestanding', '-O2', '-Wall'],
    CPPPATH=['tests/baremetal'],
)

# 构建测试程序
TEST_BUILD = os.path.join(BUILD_DIR, 'tests', 'baremetal')

# 公共 UART 实现
uart_src = 'tests/baremetal/common/uart.c'
uart_obj = arm_env.Object(
    os.path.join(TEST_BUILD, 'common', 'uart.o'),
    uart_src
)

# 自动构建所有 baremetal 子目录下的测试程序
baremetal_bins = []
for src in Glob('tests/baremetal/*/*.c'):
    src_path = os.path.normpath(str(src))
    if src_path == os.path.normpath(uart_src):
        continue
    name = os.path.splitext(os.path.basename(src_path))[0]
    periph = os.path.basename(os.path.dirname(src_path))
    target_dir = os.path.join(TEST_BUILD, periph)
    target = os.path.join(target_dir, name)
    # nand_payload 使用独立的 linker script 加载到 0x40010000
    if name == 'nand_payload':
        ld_script = 'tests/baremetal/nand/nand_payload.ld'
    else:
        ld_script = 'tests/baremetal/common/stmp3770.ld'
    test_env = arm_env.Clone(LINKFLAGS=['-T', ld_script, '-nostdlib'])
    test_obj = test_env.Object(target + '.o', src)
    test_elf = test_env.Program(target + '.elf', [test_obj, uart_obj])
    test_bin = test_env.Command(
        target + '.bin',
        test_elf,
        '$OBJCOPY -O binary $SOURCE $TARGET'
    )
    baremetal_bins.append(test_bin)

arm_env.Alias('baremetal', baremetal_bins)
arm_env.Clean(baremetal_bins, os.path.join(BUILD_DIR, 'tests'))


def run_unit_tests(target, source, env):
    """运行 host-side Python unit tests."""
    uv = shutil.which('uv', path=env['ENV'].get('PATH'))
    if uv:
        cmd = [uv, 'run', 'pytest', 'tests/unit', '-q']
    else:
        cmd = [sys.executable, '-m', 'pytest', 'tests/unit', '-q']
    test_env = env['ENV'].copy()
    existing = test_env.get('PYTHONPATH', '')
    if existing:
        test_env['PYTHONPATH'] = PROJECT_ROOT + os.pathsep + existing
    else:
        test_env['PYTHONPATH'] = PROJECT_ROOT
    print(">>> 运行 Python unit tests...")
    result = subprocess.run(cmd, cwd=PROJECT_ROOT, env=test_env)
    if result.returncode != 0:
        print("错误: Python unit tests 失败")
    return result.returncode


pyunit_marker = os.path.join(BUILD_DIR, 'tests', '.pyunit')
pyunit = env.Command(pyunit_marker, [], run_unit_tests)
AlwaysBuild(pyunit)

arm_env.Alias('test', baremetal_bins + [pyunit])

# 帮助信息
Help("""
EmuGII 构建系统
================

目标:
  scons              - 构建 QEMU (默认)
  scons qemu         - 构建 QEMU
  scons package      - 复制二进制、依赖库和默认镜像到 build/EmuGii
  scons qtest        - 构建 QEMU 并运行 STMP3770 qtest 契约回归
  scons baremetal    - 编译 tests/baremetal 下所有裸机测试程序
  scons test         - 编译裸机测试并运行 Python unit tests
  scons qtest        - 构建 QEMU 并运行 STMP3770 qtest 契约回归
  scons -c           - 清理构建目录

构建过程:
  1. 复制 ThirdParty/qemu 到 build/qemu
  2. 应用 src/ 下的源文件
  3. 应用 patches/ 下的补丁(如果有)
  4. 配置 QEMU
  5. 编译 QEMU
  6. 复制运行产物到 build/EmuGii

测试程序:
  scons baremetal    - 编译 tests/baremetal/*/*.c 到 build/tests/baremetal/*/*.bin
  scons test         - 编译裸机测试 + 运行 tests/unit (pytest)
  scons qtest        - 运行 host-side qtest 契约回归 (pytest)

环境变量:
  EMUGII_QTEST_FILTER - 只运行名称/描述匹配子串的 qtest 契约

生成的二进制文件:
  build/qemu/build/qemu-system-arm       - QEMU 模拟器
  build/EmuGii/EmuGii                    - 可运行发布目录中的 EmuGII 模拟器
  build/tests/baremetal/*/*.bin          - 裸机测试程序

运行测试:
  uv run pytest tests/unit -q
  uv run pytest tests/stmp3770_contract -q
  $env:EMUGII_QTEST_FILTER='RTC'; uv run pytest tests/stmp3770_contract -q
  build/qemu/build/qemu-system-arm -M stmp3770 -kernel build/tests/baremetal/hello/hello.bin -nographic
  build/EmuGii/EmuGii
""")
