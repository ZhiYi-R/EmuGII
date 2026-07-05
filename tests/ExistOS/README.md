# ExistOS Boot Fixture

This directory contains the read-only inputs needed to boot ExistOS in EmuGII:

- `hypervisor-rom.bin`: Hypervisor ROM loaded with QEMU `-bios`.
- `flash.initial.bin`: initial 128 MiB data-only NAND image containing the System/FTL contents.
- `run-existos.ps1`: foreground launcher for the local QEMU build.
- `MANIFEST.txt`: source paths, sizes, and SHA256 hashes for the binary inputs.

The Flash image may be modified by ExistOS at runtime, so the launcher never boots
directly from `flash.initial.bin`. It copies the initial image to:

```text
build\ExistOS\flash.bin
```

On first writable boot, the emulator may expand the runtime copy with an
appended OOB area and marker so NAND aux metadata can persist across QEMU
restarts. Keep `tests\ExistOS\flash.initial.bin` as the read-only seed; do not
replace it with the expanded runtime image.

Use this command from the repository root:

```powershell
.\tests\ExistOS\run-existos.ps1
```

Reset the writable runtime Flash copy from the initial image:

```powershell
.\tests\ExistOS\run-existos.ps1 -ResetFlash
```

Run without persisting QEMU Flash writes:

```powershell
.\tests\ExistOS\run-existos.ps1 -Snapshot
```

Expected boot markers:

```text
Booting...
System Booting...
=============SYSTEM STATUS=================
```
