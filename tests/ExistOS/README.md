# ExistOS Boot Fixture

This directory contains the read-only inputs needed to boot ExistOS in EmuGII:

- `hypervisor-rom.bin`: Hypervisor ROM loaded with QEMU `-bios`.
- `flash.initial.bin`: initial 128 MiB NAND image containing the System/FTL contents.
- `run-existos.ps1`: foreground launcher for the local QEMU build.
- `MANIFEST.txt`: source paths, sizes, and SHA256 hashes for the binary inputs.

The Flash image may be modified by ExistOS at runtime, so the launcher never boots
directly from `flash.initial.bin`. It copies the initial image to:

```text
build\ExistOS\flash.bin
```

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
