import assert from 'node:assert/strict';
import { spawn } from 'node:child_process';
import net from 'node:net';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const __filename = fileURLToPath(import.meta.url);
const __dirname = path.dirname(__filename);
const repoRoot = path.resolve(__dirname, '..');
const qemuCwd = process.env.EMUGII_QEMU_CWD ?? path.join(repoRoot, 'build', 'qemu', 'build');
const qemuBinary = process.env.EMUGII_QEMU_BINARY ?? path.join(
  qemuCwd,
  process.platform === 'win32' ? 'qemu-system-arm.exe' : 'qemu-system-arm',
);

const ICOLL_BASE = 0x80000000;
const CLKCTRL_BASE = 0x80040000;
const DIGCTL_BASE = 0x8001c000;
const PINCTRL_BASE = 0x80018000;
const DFLPT_BASE = 0x800c0000;
const LCDIF_BASE = 0x80030000;
const OCOTP_BASE = 0x8002c000;
const POWER_BASE = 0x80044000;
const RTC_BASE = 0x8005c000;

class QTestMachine {
  constructor(port) {
    this.buffer = '';
    this.queue = [];
    this.waiters = [];
    this.stderr = '';
    this.proc = spawn(
      qemuBinary,
      [
        '-M', 'stmp3770',
        '-display', 'none',
        '-monitor', 'none',
        '-serial', 'none',
        '-chardev', `socket,id=qtest,host=127.0.0.1,port=${port},server=on,wait=off`,
        '-accel', 'qtest',
        '-qtest', 'chardev:qtest',
      ],
      {
        cwd: qemuCwd,
        stdio: ['ignore', 'ignore', 'pipe'],
        env: process.env,
      },
    );

    this.proc.stderr.setEncoding('utf8');
    this.proc.stderr.on('data', (chunk) => {
      this.stderr += chunk;
    });

    this.socket = new net.Socket();
    this.socket.setEncoding('utf8');
    this.socket.on('data', (chunk) => this.#onStdout(chunk));
  }

  #onStdout(chunk) {
    this.buffer += chunk;
    while (true) {
      const idx = this.buffer.indexOf('\n');
      if (idx === -1) {
        return;
      }
      const line = this.buffer.slice(0, idx).replace(/\r$/, '');
      this.buffer = this.buffer.slice(idx + 1);
      if (!line) {
        continue;
      }
      if (this.waiters.length > 0) {
        const resolve = this.waiters.shift();
        resolve(line);
      } else {
        this.queue.push(line);
      }
    }
  }

  async #readLine(timeoutMs = 5000) {
    if (this.queue.length > 0) {
      return this.queue.shift();
    }

    return await new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        const idx = this.waiters.indexOf(done);
        if (idx >= 0) {
          this.waiters.splice(idx, 1);
        }
        reject(new Error(`Timed out waiting for qtest response. stderr=${this.stderr}`));
      }, timeoutMs);

      const done = (line) => {
        clearTimeout(timer);
        resolve(line);
      };

      this.waiters.push(done);
    });
  }

  async connect(timeoutMs = 5000) {
    const deadline = Date.now() + timeoutMs;

    while (true) {
      try {
        await new Promise((resolve, reject) => {
          const onError = (err) => {
            this.socket.off('connect', onConnect);
            reject(err);
          };
          const onConnect = () => {
            this.socket.off('error', onError);
            resolve();
          };

          this.socket.once('error', onError);
          this.socket.once('connect', onConnect);
          this.socket.connect({ host: '127.0.0.1', port: this.port });
        });
        return;
      } catch (err) {
        if (Date.now() >= deadline) {
          throw new Error(`Timed out connecting qtest socket. stderr=${this.stderr}`, { cause: err });
        }
        await new Promise((resolve) => setTimeout(resolve, 50));
      }
    }
  }

  async cmd(command) {
    this.socket.write(`${command}\n`);
    while (true) {
      const line = await this.#readLine();
      if (line.startsWith('IRQ ')) {
        continue;
      }
      return line;
    }
  }

  async readl(addr) {
    const resp = await this.cmd(`readl 0x${addr.toString(16)}`);
    assert.match(resp, /^OK 0x[0-9a-fA-F]+$/, `Unexpected readl response: ${resp}`);
    return Number(BigInt(resp.split(' ')[1]));
  }

  async writel(addr, value) {
    const resp = await this.cmd(`writel 0x${addr.toString(16)} 0x${value.toString(16)}`);
    assert.equal(resp, 'OK', `Unexpected writel response: ${resp}`);
  }

  async clockStep(ns) {
    const resp = ns === undefined
      ? await this.cmd('clock_step')
      : await this.cmd(`clock_step ${ns}`);
    assert.match(resp, /^OK /, `Unexpected clock_step response: ${resp}`);
  }

  async close() {
    this.socket.destroy();
    if (this.proc.exitCode !== null) {
      return;
    }
    this.proc.kill('SIGKILL');
    await new Promise((resolve) => {
      this.proc.once('exit', () => resolve());
    });
  }
}

async function reservePort() {
  return await new Promise((resolve, reject) => {
    const server = net.createServer();
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const address = server.address();
      if (!address || typeof address === 'string') {
        server.close();
        reject(new Error('Failed to reserve TCP port for qtest'));
        return;
      }
      const { port } = address;
      server.close((err) => {
        if (err) {
          reject(err);
        } else {
          resolve(port);
        }
      });
    });
  });
}

async function withMachine(fn) {
  const port = await reservePort();
  const machine = new QTestMachine(port);
  machine.port = port;
  try {
    await machine.connect();
    await fn(machine);
  } finally {
    await machine.close();
  }
}

async function testRtc1MsecIrq() {
  await withMachine(async (machine) => {
    await machine.writel(RTC_BASE + 0x008, 0xc000002d);
    await machine.writel(RTC_BASE + 0x004, 0x00000002);
    await machine.clockStep(1_000_000);

    const ctrl = await machine.readl(RTC_BASE + 0x000);
    const raw0 = await machine.readl(ICOLL_BASE + 0x040);
    const raw1 = await machine.readl(ICOLL_BASE + 0x050);

    assert.notEqual(ctrl & (1 << 3), 0, `RTC 1ms status bit missing: ctrl=0x${ctrl.toString(16)}`);
    assert.equal(raw0 & (1 << 22), 0, `RTC alarm line asserted unexpectedly: raw0=0x${raw0.toString(16)}`);
    assert.notEqual(raw1 & (1 << 16), 0, `RTC 1ms line not asserted on ICOLL source 48: raw1=0x${raw1.toString(16)}`);
  });
}

async function testLcdifCtrl1Layout() {
  await withMachine(async (machine) => {
    await machine.writel(LCDIF_BASE + 0x008, 0xc0000000);
    await machine.writel(LCDIF_BASE + 0x010, 0x00001000);
    await machine.writel(LCDIF_BASE + 0x004, 0x00010000);
    await machine.clockStep(20_000_000);

    const ctrl1 = await machine.readl(LCDIF_BASE + 0x010);
    const raw1 = await machine.readl(ICOLL_BASE + 0x050);

    assert.notEqual(ctrl1 & (1 << 12), 0, `LCDIF VSYNC enable bit lost: ctrl1=0x${ctrl1.toString(16)}`);
    assert.notEqual(ctrl1 & (1 << 8), 0, `LCDIF VSYNC status bit missing from CTRL1: ctrl1=0x${ctrl1.toString(16)}`);
    assert.notEqual(raw1 & (1 << 14), 0, `LCDIF IRQ not asserted on ICOLL source 46: raw1=0x${raw1.toString(16)}`);
  });
}

async function testPinctrlBank3Absent() {
  await withMachine(async (machine) => {
    const ctrl = await machine.readl(PINCTRL_BASE + 0x000);
    const before = await machine.readl(PINCTRL_BASE + 0x430);
    await machine.writel(PINCTRL_BASE + 0x430, 0xffffffff);
    const after = await machine.readl(PINCTRL_BASE + 0x430);

    assert.equal(ctrl & (1 << 29), 0, `PINCTRL PRESENT3 should be 0 on STMP3770: ctrl=0x${ctrl.toString(16)}`);
    assert.equal(before, 0, `Bank 3 DOUT should reset to 0: before=0x${before.toString(16)}`);
    assert.equal(after, 0, `Bank 3 DOUT should remain inactive when GPIO is absent: after=0x${after.toString(16)}`);
  });
}

async function testPowerVersionAndResetContract() {
  await withMachine(async (machine) => {
    const version = await machine.readl(POWER_BASE + 0x110);
    const resetBefore = await machine.readl(POWER_BASE + 0x0e0);

    assert.equal(version, 0x02000000, `POWER VERSION should report v2.0 at 0x110: got 0x${version.toString(16)}`);
    assert.equal(resetBefore, 0, `POWER RESET should reset to 0: got 0x${resetBefore.toString(16)}`);

    await machine.writel(POWER_BASE + 0x0e0, 0x00000001);
    const resetWithoutUnlock = await machine.readl(POWER_BASE + 0x0e0);
    assert.equal(
      resetWithoutUnlock,
      0,
      `POWER RESET low bits must ignore writes without unlock key: got 0x${resetWithoutUnlock.toString(16)}`,
    );

    await machine.writel(POWER_BASE + 0x0e0, 0x3e770001);
    const resetWithUnlock = await machine.readl(POWER_BASE + 0x0e0);
    assert.equal(
      resetWithUnlock,
      0x00000001,
      `POWER RESET should accept unlocked write to PWD bit: got 0x${resetWithUnlock.toString(16)}`,
    );
  });
}

async function testPowerResetValues() {
  await withMachine(async (machine) => {
    const ctrl = await machine.readl(POWER_BASE + 0x000);
    const v5ctrl = await machine.readl(POWER_BASE + 0x010);
    const charge = await machine.readl(POWER_BASE + 0x030);
    const vddd = await machine.readl(POWER_BASE + 0x040);
    const vdda = await machine.readl(POWER_BASE + 0x050);
    const vddio = await machine.readl(POWER_BASE + 0x060);
    const dclimits = await machine.readl(POWER_BASE + 0x090);
    const loopctrl = await machine.readl(POWER_BASE + 0x0a0);
    const speed = await machine.readl(POWER_BASE + 0x0c0);
    const batt = await machine.readl(POWER_BASE + 0x0d0);
    const sts = await machine.readl(POWER_BASE + 0x0b0);

    assert.equal(ctrl, 0x40040024, `POWER CTRL reset mismatch: got 0x${ctrl.toString(16)}`);
    assert.equal(v5ctrl, 0x00000100, `POWER 5VCTRL reset mismatch: got 0x${v5ctrl.toString(16)}`);
    assert.equal(charge, 0x00010000, `POWER CHARGE reset mismatch: got 0x${charge.toString(16)}`);
    assert.equal(vddd, 0x00310710, `POWER VDDDCTRL reset mismatch: got 0x${vddd.toString(16)}`);
    assert.equal(vdda, 0x0000170a, `POWER VDDACTRL reset mismatch: got 0x${vdda.toString(16)}`);
    assert.equal(vddio, 0x0000170c, `POWER VDDIOCTRL reset mismatch: got 0x${vddio.toString(16)}`);
    assert.equal(dclimits, 0x00040c5f, `POWER DCLIMITS reset mismatch: got 0x${dclimits.toString(16)}`);
    assert.equal(loopctrl, 0x00000021, `POWER LOOPCTRL reset mismatch: got 0x${loopctrl.toString(16)}`);
    assert.equal(speed, 0x00000000, `POWER SPEED reset mismatch: got 0x${speed.toString(16)}`);
    assert.equal(batt, 0x00000020, `POWER BATTMONITOR reset mismatch: got 0x${batt.toString(16)}`);
    assert.equal(sts, 0x80000000, `POWER STS reset mismatch: got 0x${sts.toString(16)}`);
  });
}

async function testDflptPte2048Contract() {
  await withMachine(async (machine) => {
    const pte2048 = await machine.readl(DFLPT_BASE + 0x2000);

    assert.equal(
      pte2048,
      0x80000c12,
      `DFLPT PTE_2048 reset mismatch: got 0x${pte2048.toString(16)}`,
    );

    await machine.writel(DFLPT_BASE + 0x2000, 0xffffffff);
    const updated = await machine.readl(DFLPT_BASE + 0x2000);

    assert.equal(
      updated,
      0x80000df6,
      `DFLPT PTE_2048 should only expose AP/DOMAIN/BUFFERABLE fields: got 0x${updated.toString(16)}`,
    );
  });
}

async function testDflptMpteTracksLocator() {
  await withMachine(async (machine) => {
    const mpte0Reset = await machine.readl(DFLPT_BASE + 0x0000);
    const pte5Reset = await machine.readl(DFLPT_BASE + (5 << 2));

    assert.equal(mpte0Reset, 0, `DFLPT MPTE0 reset contents should be 0: got 0x${mpte0Reset.toString(16)}`);
    assert.equal(pte5Reset, 0, `DFLPT unbound PTE5 should reset to 0: got 0x${pte5Reset.toString(16)}`);

    await machine.writel(DFLPT_BASE + 0x0000, 0x11223344);
    const mpte0Written = await machine.readl(DFLPT_BASE + 0x0000);
    assert.equal(mpte0Written, 0x11223344, `DFLPT MPTE0 write should stick at reset locator: got 0x${mpte0Written.toString(16)}`);

    await machine.writel(DIGCTL_BASE + 0x0400, 0x00000020);
    const oldLocation = await machine.readl(DFLPT_BASE + 0x0000);
    const newLocation = await machine.readl(DFLPT_BASE + (0x20 << 2));

    assert.equal(oldLocation, 0, `DFLPT old MPTE0 location should become unbound after locator move: got 0x${oldLocation.toString(16)}`);
    assert.equal(newLocation, 0x11223344, `DFLPT MPTE0 contents should move with DIGCTL_MPTE0_LOC: got 0x${newLocation.toString(16)}`);
  });
}

async function testDigctlWritableFieldMasks() {
  await withMachine(async (machine) => {
    await machine.writel(DIGCTL_BASE + 0x030, 0xffffffff);
    await machine.writel(DIGCTL_BASE + 0x040, 0xffffffff);
    await machine.writel(DIGCTL_BASE + 0x050, 0xffffffff);
    await machine.writel(DIGCTL_BASE + 0x0f0, 0xffffffff);
    await machine.writel(DIGCTL_BASE + 0x2b0, 0xffffffff);
    await machine.writel(DIGCTL_BASE + 0x330, 0xffffffff);

    const ramctrl = await machine.readl(DIGCTL_BASE + 0x030);
    const ramrepair = await machine.readl(DIGCTL_BASE + 0x040);
    const romctrl = await machine.readl(DIGCTL_BASE + 0x050);
    const ocramBist = await machine.readl(DIGCTL_BASE + 0x0f0);
    const armcache = await machine.readl(DIGCTL_BASE + 0x2b0);
    const ahbStatsSelect = await machine.readl(DIGCTL_BASE + 0x330);

    assert.equal(ramctrl, 0x00000f01, `DIGCTL RAMCTRL should only expose SPEED_SELECT/RAM_REPAIR_EN: got 0x${ramctrl.toString(16)}`);
    assert.equal(ramrepair, 0x0000ffff, `DIGCTL RAMREPAIR should only expose ADDR[15:0]: got 0x${ramrepair.toString(16)}`);
    assert.equal(romctrl, 0x0000000f, `DIGCTL ROMCTRL should only expose RD_MARGIN[3:0]: got 0x${romctrl.toString(16)}`);
    assert.equal(ocramBist, 0x00000306, `DIGCTL OCRAM_BIST_CSR should keep writable bits and self-clear START: got 0x${ocramBist.toString(16)}`);
    assert.equal(armcache, 0x00000333, `DIGCTL ARMCACHE should only expose CACHE_SS/DTAG_SS/ITAG_SS: got 0x${armcache.toString(16)}`);
    assert.equal(ahbStatsSelect, 0x0f0f0f0f, `DIGCTL AHB_STATS_SELECT should only expose layer-select nibbles: got 0x${ahbStatsSelect.toString(16)}`);
  });
}

async function testDigctlScratchAndMicrosecondsContract() {
  await withMachine(async (machine) => {
    const scratch0Reset = await machine.readl(DIGCTL_BASE + 0x290);
    const scratch1Reset = await machine.readl(DIGCTL_BASE + 0x2a0);

    assert.equal(scratch0Reset, 0, `DIGCTL SCRATCH0 should reset to 0: got 0x${scratch0Reset.toString(16)}`);
    assert.equal(scratch1Reset, 0, `DIGCTL SCRATCH1 should reset to 0: got 0x${scratch1Reset.toString(16)}`);

    await machine.writel(DIGCTL_BASE + 0x290, 0x89abcdef);
    await machine.writel(DIGCTL_BASE + 0x2a0, 0x01234567);

    const scratch0 = await machine.readl(DIGCTL_BASE + 0x290);
    const scratch1 = await machine.readl(DIGCTL_BASE + 0x2a0);

    assert.equal(scratch0, 0x89abcdef, `DIGCTL SCRATCH0 should store arbitrary scratch data: got 0x${scratch0.toString(16)}`);
    assert.equal(scratch1, 0x01234567, `DIGCTL SCRATCH1 should store arbitrary scratch data: got 0x${scratch1.toString(16)}`);

    await machine.writel(DIGCTL_BASE + 0x0c0, 0x00000100);
    let microseconds = await machine.readl(DIGCTL_BASE + 0x0c0);
    assert.equal(microseconds, 0x00000100, `DIGCTL MICROSECONDS base write should seed the counter value: got 0x${microseconds.toString(16)}`);

    await machine.writel(DIGCTL_BASE + 0x0c4, 0x00000020);
    microseconds = await machine.readl(DIGCTL_BASE + 0x0c0);
    assert.equal(microseconds, 0x00000120, `DIGCTL MICROSECONDS_SET should OR bits into the current value: got 0x${microseconds.toString(16)}`);

    await machine.writel(DIGCTL_BASE + 0x0c8, 0x00000010);
    microseconds = await machine.readl(DIGCTL_BASE + 0x0c0);
    assert.equal(microseconds, 0x00000120, `DIGCTL MICROSECONDS_CLR should only clear selected bits: got 0x${microseconds.toString(16)}`);

    await machine.writel(DIGCTL_BASE + 0x0cc, 0x00000001);
    microseconds = await machine.readl(DIGCTL_BASE + 0x0c0);
    assert.equal(microseconds, 0x00000121, `DIGCTL MICROSECONDS_TOG should XOR selected bits: got 0x${microseconds.toString(16)}`);
  });
}

async function testDigctlUndocumentedAliasDecode() {
  await withMachine(async (machine) => {
    await machine.writel(DIGCTL_BASE + 0x0400, 0x00000055);
    await machine.writel(DIGCTL_BASE + 0x0290, 0x89abcdef);
    await machine.writel(DIGCTL_BASE + 0x02b0, 0x00000321);
    await machine.writel(DIGCTL_BASE + 0x0330, 0x01020304);

    const scratch0AliasRead = await machine.readl(DIGCTL_BASE + 0x0294);
    const armcacheAliasRead = await machine.readl(DIGCTL_BASE + 0x02b4);
    const ahbStatsAliasRead = await machine.readl(DIGCTL_BASE + 0x0334);
    const mpte0AliasRead = await machine.readl(DIGCTL_BASE + 0x0404);

    assert.equal(scratch0AliasRead, 0, `DIGCTL SCRATCH0 undocumented +0x4 alias should decode as hole: got 0x${scratch0AliasRead.toString(16)}`);
    assert.equal(armcacheAliasRead, 0, `DIGCTL ARMCACHE undocumented +0x4 alias should decode as hole: got 0x${armcacheAliasRead.toString(16)}`);
    assert.equal(ahbStatsAliasRead, 0, `DIGCTL AHB_STATS_SELECT undocumented +0x4 alias should decode as hole: got 0x${ahbStatsAliasRead.toString(16)}`);
    assert.equal(mpte0AliasRead, 0, `DIGCTL MPTE0_LOC undocumented +0x4 alias should decode as hole: got 0x${mpte0AliasRead.toString(16)}`);

    await machine.writel(DIGCTL_BASE + 0x0294, 0x13572468);
    await machine.writel(DIGCTL_BASE + 0x02b4, 0xffffffff);
    await machine.writel(DIGCTL_BASE + 0x0334, 0xffffffff);
    await machine.writel(DIGCTL_BASE + 0x0404, 0x00000a00);

    const scratch0 = await machine.readl(DIGCTL_BASE + 0x0290);
    const armcache = await machine.readl(DIGCTL_BASE + 0x02b0);
    const ahbStatsSelect = await machine.readl(DIGCTL_BASE + 0x0330);
    const mpte0Loc = await machine.readl(DIGCTL_BASE + 0x0400);

    assert.equal(scratch0, 0x89abcdef, `DIGCTL SCRATCH0 base register should ignore undocumented alias writes: got 0x${scratch0.toString(16)}`);
    assert.equal(armcache, 0x00000321, `DIGCTL ARMCACHE base register should ignore undocumented alias writes: got 0x${armcache.toString(16)}`);
    assert.equal(ahbStatsSelect, 0x01020304, `DIGCTL AHB_STATS_SELECT base register should ignore undocumented alias writes: got 0x${ahbStatsSelect.toString(16)}`);
    assert.equal(mpte0Loc, 0x00000055, `DIGCTL MPTE0_LOC base register should ignore undocumented alias writes: got 0x${mpte0Loc.toString(16)}`);
  });
}

async function testDigctlCtrlBehaviorContract() {
  await withMachine(async (machine) => {
    const ctrlReset = await machine.readl(DIGCTL_BASE + 0x000);
    assert.equal(ctrlReset, 0x00000004, `DIGCTL CTRL should reset with only USB_CLKGATE set: got 0x${ctrlReset.toString(16)}`);

    await machine.writel(DIGCTL_BASE + 0x004, 0x00000008);
    const ctrlAfterDebugDisableSet = await machine.readl(DIGCTL_BASE + 0x000);
    assert.equal(
      ctrlAfterDebugDisableSet,
      0x0000000c,
      `DIGCTL CTRL.DEBUG_DISABLE should latch high when set: got 0x${ctrlAfterDebugDisableSet.toString(16)}`,
    );

    await machine.writel(DIGCTL_BASE + 0x008, 0x00000008);
    const ctrlAfterDebugDisableClear = await machine.readl(DIGCTL_BASE + 0x000);
    assert.equal(
      ctrlAfterDebugDisableClear,
      0x0000000c,
      `DIGCTL CTRL.DEBUG_DISABLE should stay set until reset: got 0x${ctrlAfterDebugDisableClear.toString(16)}`,
    );

    const entropyLatchedReset = await machine.readl(DIGCTL_BASE + 0x0a0);
    assert.equal(
      entropyLatchedReset,
      0,
      `DIGCTL ENTROPY_LATCHED should reset to 0: got 0x${entropyLatchedReset.toString(16)}`,
    );

    await machine.clockStep(1_000_000);
    await machine.writel(DIGCTL_BASE + 0x004, 0x00000001);
    const entropyLatched1 = await machine.readl(DIGCTL_BASE + 0x0a0);
    assert.notEqual(
      entropyLatched1,
      0,
      `DIGCTL CTRL.LATCH_ENTROPY should latch the live entropy value on first set: got 0x${entropyLatched1.toString(16)}`,
    );

    await machine.clockStep(1_000_000);
    await machine.writel(DIGCTL_BASE + 0x004, 0x00000001);
    const entropyLatched2 = await machine.readl(DIGCTL_BASE + 0x0a0);
    assert.notEqual(
      entropyLatched2,
      entropyLatched1,
      `DIGCTL CTRL.LATCH_ENTROPY should re-latch on repeated set writes: first=0x${entropyLatched1.toString(16)} second=0x${entropyLatched2.toString(16)}`,
    );
  });
}

async function testClkctrlResetContract() {
  await withMachine(async (machine) => {
    const pllctrl0 = await machine.readl(CLKCTRL_BASE + 0x000);
    const pllctrl1 = await machine.readl(CLKCTRL_BASE + 0x010);
    const cpu = await machine.readl(CLKCTRL_BASE + 0x020);
    const hbus = await machine.readl(CLKCTRL_BASE + 0x030);
    const xbus = await machine.readl(CLKCTRL_BASE + 0x040);
    const xtal = await machine.readl(CLKCTRL_BASE + 0x050);
    const pix = await machine.readl(CLKCTRL_BASE + 0x060);
    const ssp = await machine.readl(CLKCTRL_BASE + 0x070);
    const gpmi = await machine.readl(CLKCTRL_BASE + 0x080);
    const spdif = await machine.readl(CLKCTRL_BASE + 0x090);
    const frac = await machine.readl(CLKCTRL_BASE + 0x0d0);
    const clkseq = await machine.readl(CLKCTRL_BASE + 0x0e0);
    const reset = await machine.readl(CLKCTRL_BASE + 0x0f0);
    const version = await machine.readl(CLKCTRL_BASE + 0x100);

    assert.equal(pllctrl0, 0x00000000, `CLKCTRL PLLCTRL0 reset mismatch: got 0x${pllctrl0.toString(16)}`);
    assert.equal(pllctrl1, 0x00000000, `CLKCTRL PLLCTRL1 reset mismatch: got 0x${pllctrl1.toString(16)}`);
    assert.equal(cpu, 0x00010001, `CLKCTRL CPU reset mismatch: got 0x${cpu.toString(16)}`);
    assert.equal(hbus, 0x00000001, `CLKCTRL HBUS reset mismatch: got 0x${hbus.toString(16)}`);
    assert.equal(xbus, 0x00000001, `CLKCTRL XBUS reset mismatch: got 0x${xbus.toString(16)}`);
    assert.equal(xtal, 0x70000001, `CLKCTRL XTAL reset mismatch: got 0x${xtal.toString(16)}`);
    assert.equal(pix, 0x80000001, `CLKCTRL PIX reset mismatch: got 0x${pix.toString(16)}`);
    assert.equal(ssp, 0x80000001, `CLKCTRL SSP reset mismatch: got 0x${ssp.toString(16)}`);
    assert.equal(gpmi, 0x80000001, `CLKCTRL GPMI reset mismatch: got 0x${gpmi.toString(16)}`);
    assert.equal(spdif, 0x80000000, `CLKCTRL SPDIF reset mismatch: got 0x${spdif.toString(16)}`);
    assert.equal(frac, 0x92920092, `CLKCTRL FRAC reset mismatch: got 0x${frac.toString(16)}`);
    assert.equal(clkseq, 0x000000bb, `CLKCTRL CLKSEQ reset mismatch: got 0x${clkseq.toString(16)}`);
    assert.equal(reset, 0x00000000, `CLKCTRL RESET reset mismatch: got 0x${reset.toString(16)}`);
    assert.equal(version, 0x02010000, `CLKCTRL VERSION mismatch: got 0x${version.toString(16)}`);
  });
}

async function testOcotpBankOpenContract() {
  await withMachine(async (machine) => {
    await machine.writel(OCOTP_BASE + 0x000, 0x3e770000);
    await machine.writel(OCOTP_BASE + 0x010, 0x11223344);

    const custcap = await machine.readl(OCOTP_BASE + 0x110);
    const cust0Closed = await machine.readl(OCOTP_BASE + 0x020);
    const ctrlAfterClosedRead = await machine.readl(OCOTP_BASE + 0x000);

    assert.equal(custcap, 0, `OCOTP CUSTCAP shadow should be readable without bank open: got 0x${custcap.toString(16)}`);
    assert.equal(cust0Closed, 0xbadabada, `OCOTP CUST0 should return BADABADA when bank is closed: got 0x${cust0Closed.toString(16)}`);
    assert.notEqual(
      ctrlAfterClosedRead & (1 << 9),
      0,
      `OCOTP CTRL.ERROR should latch after closed-bank read: ctrl=0x${ctrlAfterClosedRead.toString(16)}`,
    );

    await machine.writel(OCOTP_BASE + 0x008, 0x00000200);
    const ctrlAfterClr = await machine.readl(OCOTP_BASE + 0x000);
    assert.equal(
      ctrlAfterClr & (1 << 9),
      0,
      `OCOTP CTRL_CLR should clear ERROR via SCT clear space: ctrl=0x${ctrlAfterClr.toString(16)}`,
    );

    await machine.writel(OCOTP_BASE + 0x004, 0x00001000);
    const cust0Open = await machine.readl(OCOTP_BASE + 0x020);

    assert.equal(cust0Open, 0x11223344, `OCOTP CUST0 should expose programmed OTP bits once bank is open: got 0x${cust0Open.toString(16)}`);
  });
}

async function testOcotpLockAndShadowContract() {
  await withMachine(async (machine) => {
    await machine.writel(OCOTP_BASE + 0x110, 0x12345678);
    const custcapBeforeLock = await machine.readl(OCOTP_BASE + 0x110);
    assert.equal(custcapBeforeLock, 0x12345678, `OCOTP CUSTCAP shadow should be writable before lock: got 0x${custcapBeforeLock.toString(16)}`);

    await machine.writel(OCOTP_BASE + 0x000, 0x3e77000f);
    await machine.writel(OCOTP_BASE + 0x010, 0x12345678);

    await machine.writel(OCOTP_BASE + 0x000, 0x3e770010);
    await machine.writel(OCOTP_BASE + 0x010, 0x00000090);

    const ctrlAfterLockProgram = await machine.readl(OCOTP_BASE + 0x000);
    assert.equal(
      ctrlAfterLockProgram & 0xffff0000,
      0,
      `OCOTP successful DATA write should clear WR_UNLOCK: ctrl=0x${ctrlAfterLockProgram.toString(16)}`,
    );

    await machine.writel(OCOTP_BASE + 0x004, 0x00002000);
    const lockShadow = await machine.readl(OCOTP_BASE + 0x120);
    const custcapReloaded = await machine.readl(OCOTP_BASE + 0x110);
    assert.equal(lockShadow, 0x00000090, `OCOTP LOCK shadow should reload programmed lock bits: got 0x${lockShadow.toString(16)}`);
    assert.equal(
      custcapReloaded,
      0x12345678,
      `OCOTP reload should repopulate CUSTCAP shadow from OTP bank 1 word 7: got 0x${custcapReloaded.toString(16)}`,
    );

    await machine.writel(OCOTP_BASE + 0x110, 0xdeadbeef);
    const custcapAfterLock = await machine.readl(OCOTP_BASE + 0x110);
    const ctrlAfterLockedShadowWrite = await machine.readl(OCOTP_BASE + 0x000);

    assert.equal(
      custcapAfterLock,
      0x12345678,
      `OCOTP CUSTCAP shadow should ignore writes after CUSTCAP_SHADOW lock: got 0x${custcapAfterLock.toString(16)}`,
    );
    assert.notEqual(
      ctrlAfterLockedShadowWrite & (1 << 9),
      0,
      `OCOTP locked shadow write should raise CTRL.ERROR: ctrl=0x${ctrlAfterLockedShadowWrite.toString(16)}`,
    );

    await machine.writel(OCOTP_BASE + 0x008, 0x00000200);
    await machine.writel(OCOTP_BASE + 0x004, 0x00001000);
    const crypto0 = await machine.readl(OCOTP_BASE + 0x060);
    const ctrlAfterCryptoRead = await machine.readl(OCOTP_BASE + 0x000);

    assert.equal(crypto0, 0xbadabada, `OCOTP locked CRYPTO0 should return BADABADA: got 0x${crypto0.toString(16)}`);
    assert.notEqual(
      ctrlAfterCryptoRead & (1 << 9),
      0,
      `OCOTP locked crypto read should raise CTRL.ERROR: ctrl=0x${ctrlAfterCryptoRead.toString(16)}`,
    );
  });
}

const tests = [
  ['RTC 1ms IRQ routing', testRtc1MsecIrq],
  ['LCDIF CTRL1 interrupt layout', testLcdifCtrl1Layout],
  ['PINCTRL Bank 3 absence', testPinctrlBank3Absent],
  ['POWER version and reset contract', testPowerVersionAndResetContract],
  ['POWER reset values', testPowerResetValues],
  ['DFLPT PTE_2048 contract', testDflptPte2048Contract],
  ['DFLPT MPTE locator remap', testDflptMpteTracksLocator],
  ['DIGCTL writable field masks', testDigctlWritableFieldMasks],
  ['DIGCTL scratch and microseconds contract', testDigctlScratchAndMicrosecondsContract],
  ['DIGCTL undocumented alias decode', testDigctlUndocumentedAliasDecode],
  ['DIGCTL ctrl behavior contract', testDigctlCtrlBehaviorContract],
  ['CLKCTRL reset contract', testClkctrlResetContract],
  ['OCOTP bank-open contract', testOcotpBankOpenContract],
  ['OCOTP lock and shadow contract', testOcotpLockAndShadowContract],
];

let failures = 0;

for (const [name, fn] of tests) {
  try {
    await fn();
    console.log(`PASS ${name}`);
  } catch (err) {
    failures++;
    console.error(`FAIL ${name}`);
    console.error(err instanceof Error ? err.stack : String(err));
  }
}

if (failures > 0) {
  process.exitCode = 1;
}
