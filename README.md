# EmuGII - STMP3770 QEMU Emulator

基于 QEMU 的 SigmaTel STMP3770 SoC 模拟器。

STMP3770 是一款基于 ARM926EJ-S 的嵌入式 SoC,广泛用于便携式媒体播放器(如 HP 39gII 计算器)。

**注**: HP 39gII 仅有 512KB 片上 SRAM,无外部 DRAM。ExistOS 使用 NAND Flash 作为虚拟内存交换空间。

## 项目特点

- **完整的 SoC 模拟** - 18 个外设模块,涵盖中断、时钟、DMA、NAND Flash、显示、音频等
- **非侵入式构建** - 不修改 QEMU 子模块,所有变更通过补丁应用
- **真实固件验证** - 已通过 ExistOS-For-HP39GII 固件验证
- **易于扩展** - 模块化设计,便于添加新外设

## 快速开始

### 依赖安装

**Windows (MSYS2):**
```bash
pacman -S base-devel mingw-w64-x86_64-toolchain \
          mingw-w64-x86_64-glib2 mingw-w64-x86_64-pixman \
          mingw-w64-x86_64-ninja mingw-w64-x86_64-meson \
          python python-pip git
pip install scons
```

**Linux (Ubuntu/Debian):**
```bash
sudo apt-get install build-essential git python3 python3-pip \
                     libglib2.0-dev libpixman-1-dev ninja-build meson
pip3 install scons
```

### 构建

```bash
# 克隆仓库(含子模块)
git clone --recursive <repository-url>
cd EmuGII

# 构建 QEMU
scons

# 清理
scons -c
```

构建产物: `build/qemu/build/qemu-system-arm`

### 运行

```bash
# 启动模拟器
build/qemu/build/qemu-system-arm -M stmp3770 -bios <firmware.bin> -nographic

# 查看帮助
build/qemu/build/qemu-system-arm -M help | grep stmp3770
```

## 项目结构

```
EmuGII/
├── src/                    # STMP3770 实现代码
│   ├── hw/arm/            # SoC 和 Board 定义
│   ├── hw/intc/           # 中断控制器
│   ├── hw/misc/           # 时钟、电源、数字控制等
│   ├── hw/dma/            # DMA 控制器
│   ├── hw/block/          # NAND Flash 控制器
│   └── include/hw/        # 头文件
├── patches/                # QEMU 构建系统补丁
├── tests/                  # 裸机测试程序
├── ThirdParty/qemu/       # QEMU 子模块 (只读)
├── build/                  # 构建输出 (自动生成)
└── SConstruct              # 构建脚本
```

## 构建系统设计

采用"非侵入式"集成方案:

1. **复制** `ThirdParty/qemu` → `build/qemu` (保持子模块干净)
2. **复制** `src/` 代码到 `build/qemu` 相应位置
3. **应用** `patches/` 到 `build/qemu` (Kconfig + Meson 集成)
4. **配置** 并编译 QEMU

优点:
- ThirdParty/qemu 始终干净,易于更新上游
- build/ 可随时删除重建
- 补丁独立管理,便于提交到 QEMU 上游

## 已实现组件

### 核心
- **STMP3770 SoC** - ARM926EJ-S + 512KB SRAM (HP 39gII 无外部 DRAM)
- **ICOLL** - 中断控制器 (64 源, 4 级优先级, IRQ/FIQ 路由)
- **CLKCTRL** - 时钟控制器 (480MHz PLL, 多时钟域)
- **DIGCTL** - 芯片 ID、版本、复位控制
- **Board** - STMP3770 开发板定义

### 外设
- **UART** - PL011 串口 (Debug + App UART)
- **TIMROT** - 4 通道 16-bit 定时器
- **RTC** - 实时时钟 + 看门狗
- **GPIO/PINCTRL** - 3 组 GPIO 中断
- **OCOTP** - OTP 控制器
- **POWER** - 电源管理
- **LRADC** - 低分辨率 ADC

### 存储与 DMA
- **APBH/APBX DMA** - 8 通道链表 DMA
- **GPMI** - NAND Flash 控制器 (含启动支持)
- **BCH/ECC** - ECC 控制器

### 多媒体
- **LCDIF** - LCD 控制器 (真实 framebuffer)
- **Audio DAC/ADC** - 音频输入输出
- **PWM** - 5 通道 PWM (寄存器级 stub)

### 通信
- **I2C** - I2C 总线 (寄存器级 stub)
- **SSP1/SSP2** - 同步串行接口 (寄存器级 stub)
- **USB PHY + OTG** - USB 控制器 (寄存器级 stub)

## 内存映射

| 地址 | 模块 | 状态 |
|------|------|------|
| 0x00000000 | SRAM (512KB) | ✅ |
| 0x80000000 | ICOLL | ✅ |
| 0x80004000 | APBH DMA | ✅ |
| 0x80008000 | BCH/ECC | ✅ |
| 0x8000C000 | GPMI | ✅ |
| 0x80018000 | PINCTRL | ✅ |
| 0x8001C000 | DIGCTL | ✅ |
| 0x80024000 | APBX DMA | ✅ |
| 0x8002C000 | OCOTP | ✅ |
| 0x80030000 | LCDIF | ✅ |
| 0x80040000 | CLKCTRL | ✅ |
| 0x80048000 | Audio DAC | ✅ |
| 0x8004C000 | Audio ADC | ✅ |
| 0x80058000 | I2C | ⚠️ Stub |
| 0x8005C000 | RTC | ✅ |
| 0x80064000 | PWM | ⚠️ Stub |
| 0x80068000 | TIMROT | ✅ |
| 0x8006C000 | App UART | ✅ |
| 0x80070000 | Debug UART | ✅ |
| 0x8007C000 | USB PHY | ⚠️ Stub |
| 0x80080000 | USB OTG | ⚠️ Stub |

完整定义见 `src/include/hw/arm/stmp3770.h`

## 测试

`tests/` 目录包含裸机测试程序,用于验证各外设:

- `hello.c` - Debug UART 输出测试
- `gpmi_test.c` - GPMI 寄存器访问
- `gpmi_dma_test.c` - APBH DMA + GPMI
- `nand_boot_test.c` - NAND 启动路径
- `lcdif_test.c` - LCD 初始化
- `audio_test.c` - 音频 DAC/ADC
- `usb_test.c` - USB 寄存器

构建示例:
```bash
arm-none-eabi-gcc -mcpu=arm926ej-s -nostdlib -ffreestanding -O2 \
    -c tests/hello.c -o build/tests/hello.o
arm-none-eabi-ld -T tests/stmp3770.ld -o build/tests/hello.elf build/tests/hello.o
arm-none-eabi-objcopy -O binary build/tests/hello.elf build/tests/hello.bin
```

运行:
```bash
build/qemu/build/qemu-system-arm -M stmp3770 -kernel build/tests/hello.bin -nographic
```

## 开发

### 添加新外设

1. 创建源文件: `src/hw/<category>/<device>.c` 和 `src/include/hw/<category>/<device>.h`
2. 在 `src/hw/arm/stmp3770.c` 中集成到 SoC
3. 创建 Kconfig 和 Meson 补丁: `patches/00XX-add-<device>-*.patch`
4. 重新构建: `scons -c && scons`

### 生成补丁

```bash
cd build/qemu
# 修改 Kconfig 或 meson.build
git diff hw/xxx/Kconfig > ../../patches/00XX-description.patch
```

### 调试

```bash
# 启用 QEMU 日志
build/qemu/build/qemu-system-arm -M stmp3770 -kernel <fw.bin> \
    -d guest_errors,unimp,int -nographic

# GDB 调试
build/qemu/build/qemu-system-arm -M stmp3770 -kernel <fw.bin> -s -S
# 另一终端: gdb-multiarch -ex "target remote :1234"
```

## 验证状态

✅ **ExistOS-For-HP39GII 固件启动成功**
- CPU、内存、中断、时钟系统完全正常
- NAND Flash 检测和 FTL 初始化成功
- 串口输出稳定
- 电源管理、定时器、GPIO 等工作正常

## 许可证

GNU GPL v2+, 与 QEMU 保持一致。

## 参考

- [QEMU 官方文档](https://www.qemu.org/docs/master/)
- [ExistOS-For-HP39GII](https://github.com/ExistOS-Team/ExistOS-For-HP39GII) - 真实固件参考
