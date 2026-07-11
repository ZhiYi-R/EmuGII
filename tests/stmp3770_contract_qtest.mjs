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
const DCP_BASE = 0x80028000;
const LCDIF_BASE = 0x80030000;
const OCOTP_BASE = 0x8002c000;
const POWER_BASE = 0x80044000;
const RTC_BASE = 0x8005c000;
const PWM_BASE = 0x80064000;
const TIMROT_BASE = 0x80068000;
const USBPHY_BASE = 0x8007c000;
const USB_BASE = 0x80080000;
const LRADC_BASE = 0x80050000;
const BCH_BASE = 0x80008000;
const GPMI_BASE = 0x8000c000;
const I2C_BASE = 0x80058000;
const SSP1_BASE = 0x80010000;
const SSP2_BASE = 0x80034000;
const APBH_BASE = 0x80004000;
const APBX_BASE = 0x80024000;
const APPUART_BASE = 0x8006c000;
const DBGUART_BASE = 0x80070000;
const SRAM_BASE = 0x00000000;
const OCROM_BASE = 0xffff0000;

class QTestMachine {
  constructor(port) {
    this.buffer = '';
    this.queue = [];
    this.waiters = [];
    this.stderr = '';
    this.socketError = null;
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
    this.socket.on('error', (err) => {
      this.socketError = err;
      while (this.waiters.length > 0) {
        this.waiters.shift().reject(err);
      }
    });

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

      done.reject = (err) => {
        clearTimeout(timer);
        reject(new Error(`qtest socket failed: ${err.message}; stderr=${this.stderr}`));
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

  async readw(addr) {
    const resp = await this.cmd(`readw 0x${addr.toString(16)}`);
    assert.match(resp, /^OK 0x[0-9a-fA-F]+$/, `Unexpected readw response: ${resp}`);
    return Number(BigInt(resp.split(' ')[1]));
  }

  async writel(addr, value) {
    const resp = await this.cmd(`writel 0x${addr.toString(16)} 0x${value.toString(16)}`);
    assert.equal(resp, 'OK', `Unexpected writel response: ${resp}`);
  }

  async writew(addr, value) {
    const resp = await this.cmd(`writew 0x${addr.toString(16)} 0x${value.toString(16)}`);
    assert.equal(resp, 'OK', `Unexpected writew response: ${resp}`);
  }

  async readb(addr) {
    const resp = await this.cmd(`readb 0x${addr.toString(16)}`);
    assert.match(resp, /^OK 0x[0-9a-fA-F]+$/, `Unexpected readb response: ${resp}`);
    return Number(BigInt(resp.split(' ')[1]));
  }

  async writeb(addr, value) {
    const resp = await this.cmd(`writeb 0x${addr.toString(16)} 0x${value.toString(16)}`);
    assert.equal(resp, 'OK', `Unexpected writeb response: ${resp}`);
  }

  async clockStep(ns) {
    const resp = ns === undefined
      ? await this.cmd('clock_step')
      : await this.cmd(`clock_step ${ns}`);
    assert.match(resp, /^OK /, `Unexpected clock_step response: ${resp}`);
  }

  async setIrqIn(qomPath, name, num, level) {
    const gpioName = name ?? 'unnamed-gpio-in';
    const resp = await this.cmd(`set_irq_in ${qomPath} ${gpioName} ${num} ${level}`);
    assert.equal(resp, 'OK', `Unexpected set_irq_in response: ${resp}`);
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

async function testRtcResetAndPersistent0Contract() {
  await withMachine(async (machine) => {
    assert.equal(
      await machine.readl(RTC_BASE + 0x010),
      0xe0ff0000,
      'RTC STAT reset must expose presence flags and all eight stale shadow registers',
    );

    await machine.writel(RTC_BASE + 0x064, 0x80030000);
    assert.equal(
      await machine.readl(RTC_BASE + 0x060),
      0x80030100,
      'RTC PERSISTENT0 SET must retain SPARE_ANALOG, AUTO_RESTART, and DISABLE_PSWITCH',
    );
  });
}

async function testRtcCopyControllerContract() {
  await withMachine(async (machine) => {
    assert.equal(
      (await machine.readl(RTC_BASE + 0x010) >>> 16) & 0xff,
      0xff,
      'RTC copy controller must report all shadow registers stale after reset',
    );
    await machine.clockStep(3_000_000);
    assert.equal(
      (await machine.readl(RTC_BASE + 0x010) >>> 16) & 0xff,
      0,
      'RTC copy controller must complete reset shadow refresh in approximately 3 ms',
    );

    await machine.writel(RTC_BASE + 0x070, 0x12345678);
    assert.notEqual(
      (await machine.readl(RTC_BASE + 0x010) >>> 8) & 0x02,
      0,
      'RTC PERSISTENT1 write must mark its shadow value newer than analog storage',
    );
    await machine.clockStep(3_000_000);
    assert.equal(
      (await machine.readl(RTC_BASE + 0x010) >>> 8) & 0x02,
      0,
      'RTC copy controller must clear PERSISTENT1 NEW_REGS after write-back',
    );

    await machine.writel(RTC_BASE + 0x004, 0x00000020);
    const ctrlAfterForceUpdate = await machine.readl(RTC_BASE + 0x000);
    assert.equal(
      ctrlAfterForceUpdate & 0x20,
      0,
      'RTC FORCE_UPDATE must self-clear after the copy request is accepted',
    );
    assert.equal(
      (await machine.readl(RTC_BASE + 0x010) >>> 16) & 0xff,
      0xff,
      'RTC FORCE_UPDATE must mark every shadow register stale',
    );
    await machine.clockStep(3_000_000);
    assert.equal(
      (await machine.readl(RTC_BASE + 0x010) >>> 16) & 0xff,
      0,
      'RTC FORCE_UPDATE refresh must complete through the copy controller',
    );
  });
}

async function testRtcClockGateContract() {
  await withMachine(async (machine) => {
    await machine.writel(RTC_BASE + 0x008, 0x80000000);
    assert.equal(
      (await machine.readl(RTC_BASE + 0x000)) & 0xc0000000,
      0x40000000,
      'RTC CTRL_CLR.SFTRST must not clear the independently controlled CLKGATE bit',
    );

    await machine.writel(RTC_BASE + 0x008, 0x40000000);
    assert.equal(
      (await machine.readl(RTC_BASE + 0x000)) & 0xc0000000,
      0,
      'RTC CTRL_CLR.CLKGATE must independently enable the digital clock',
    );
  });
}

async function testRtcWatchdogDebugContract() {
  await withMachine(async (machine) => {
    await machine.writel(RTC_BASE + 0x008, 0xc0000000);
    await machine.writel(RTC_BASE + 0x0c4, 0x00000003);
    assert.equal(
      await machine.readl(RTC_BASE + 0x0c0),
      0x00000002,
      'RTC DEBUG must allow only WATCHDOG_RESET_MASK to be written',
    );

    await machine.writel(RTC_BASE + 0x050, 1);
    await machine.writel(RTC_BASE + 0x004, 0x00000010);
    await machine.clockStep(1_000_000);
    assert.equal(
      await machine.readl(RTC_BASE + 0x0c0),
      0x00000003,
      'RTC watchdog mask must retain the SoC and expose asserted watchdog reset state',
    );
  });
}

async function testRtcAlarmWakeContract() {
  await withMachine(async (machine) => {
    await machine.writel(RTC_BASE + 0x008, 0xc0000000);
    await machine.clockStep(3_000_000);
    await machine.writel(RTC_BASE + 0x040, 1);
    await machine.writel(RTC_BASE + 0x064, 0x00000004);
    await machine.writel(RTC_BASE + 0x030, 1);

    assert.equal(
      (await machine.readl(RTC_BASE + 0x060)) & 0x80,
      0,
      'RTC ALARM_WAKE must remain clear when an alarm occurs while the chip is powered up',
    );
  });
}

async function testRtcSuppressCopyToAnalogContract() {
  await withMachine(async (machine) => {
    await machine.writel(RTC_BASE + 0x008, 0xc0000000);
    await machine.clockStep(3_000_000);
    await machine.writel(RTC_BASE + 0x004, 0x00000040);
    await machine.writel(RTC_BASE + 0x070, 0x12345678);
    await machine.clockStep(3_000_000);

    assert.notEqual(
      (await machine.readl(RTC_BASE + 0x010) >>> 8) & 0x02,
      0,
      'RTC SUPPRESS_COPY2ANALOG must retain PERSISTENT1 NEW_REGS while automatic copy is disabled',
    );
  });
}

async function testRtcAnalogStateSurvivesChipReset() {
  await withMachine(async (machine) => {
    await machine.clockStep(3_000_000);
    await machine.writel(RTC_BASE + 0x030, 0x12345678);
    await machine.writel(RTC_BASE + 0x040, 0x87654321);
    await machine.writel(RTC_BASE + 0x070, 0x0000000f);
    await machine.writel(RTC_BASE + 0x064, 0x00000008);
    await machine.clockStep(3_000_000);

    await machine.writel(CLKCTRL_BASE + 0x0f0, 0x00000002);
    await machine.writel(RTC_BASE + 0x030, 0xffffffff);
    assert.equal(
      await machine.readl(RTC_BASE + 0x030),
      0,
      'RTC LCK_SECS analog state must reject seconds writes before reset shadow refresh completes',
    );
    await machine.clockStep(3_000_000);

    assert.equal(
      await machine.readl(RTC_BASE + 0x030),
      0x12345678,
      'RTC SECONDS analog state must survive CLKCTRL RESET.CHIP and refresh the shadow register',
    );
    assert.equal(
      await machine.readl(RTC_BASE + 0x040),
      0x87654321,
      'RTC ALARM analog state must survive CLKCTRL RESET.CHIP and refresh the shadow register',
    );
    assert.equal(
      await machine.readl(RTC_BASE + 0x060),
      0x00000108,
      'RTC PERSISTENT0 analog state, including LCK_SECS, must survive CLKCTRL RESET.CHIP',
    );
    assert.equal(
      await machine.readl(RTC_BASE + 0x070),
      0x0000000f,
      'RTC PERSISTENT1 analog state must survive CLKCTRL RESET.CHIP and refresh the shadow register',
    );
  });
}

async function testRtcAnalogSecondsRunWhileDigitalClockGated() {
  await withMachine(async (machine) => {
    await machine.clockStep(3_000_000);
    await machine.writel(RTC_BASE + 0x030, 0);
    await machine.clockStep(3_000_000);

    await machine.writel(RTC_BASE + 0x004, 0x40000000);
    await machine.clockStep(1_000_000_000);
    await machine.writel(RTC_BASE + 0x008, 0x40000000);
    await machine.writel(RTC_BASE + 0x004, 0x00000020);
    await machine.clockStep(3_000_000);

    assert.equal(
      await machine.readl(RTC_BASE + 0x030),
      1,
      'RTC analog seconds must continue while the digital clock is gated and refresh after it is enabled',
    );
  });
}

async function testRtcPersistent1WriteMaskContract() {
  await withMachine(async (machine) => {
    await machine.clockStep(3_000_000);
    await machine.writel(RTC_BASE + 0x070, 0xffffffff);
    assert.equal(
      await machine.readl(RTC_BASE + 0x070),
      0x0000000f,
      'RTC PERSISTENT1 must ignore writes to reserved bits 31:4',
    );

    await machine.writel(RTC_BASE + 0x070, 0xdeadbeef);
    assert.equal(
      await machine.readl(RTC_BASE + 0x070),
      0x0000000f,
      'RTC PERSISTENT1 must only retain writable bits 3:0',
    );
  });
}

async function testRtcMsecResolutionContract() {
  await withMachine(async (machine) => {
    const resolutions = [1, 2, 4, 8, 16];

    await machine.writel(RTC_BASE + 0x008, 0xc0000000);
    await machine.clockStep(3_000_000);

    for (const resolution of resolutions) {
      await machine.writel(RTC_BASE + 0x060, resolution << 8);
      await machine.clockStep(3_000_000);
      const before = await machine.readl(RTC_BASE + 0x020);

      await machine.clockStep(resolution * 8 * 1_000_000);
      assert.equal(
        await machine.readl(RTC_BASE + 0x020),
        before + 8,
        `RTC MSEC_RES=${resolution} must advance the counter once per ${resolution} ms`,
      );
    }
  });
}

async function testTimrotTickAndUpdateContract() {
  await withMachine(async (machine) => {
    await machine.writel(TIMROT_BASE + 0x008, 0xc0000000);

    await machine.writel(TIMROT_BASE + 0x020, 0x000000cf);
    await machine.writel(TIMROT_BASE + 0x030, 10);
    await machine.clockStep(1_000);
    assert.notEqual(
      (await machine.readl(TIMROT_BASE + 0x020)) & 0x8000,
      0,
      'TIMROT SELECT=0xF must use undefined-select always-tick behavior',
    );

    await machine.writel(TIMROT_BASE + 0x028, 0x00008000);
    await machine.writel(TIMROT_BASE + 0x020, 0x0000004c);
    await machine.writel(TIMROT_BASE + 0x030, 1_000);
    await machine.clockStep(10_000);
    const runningBeforeUpdate = (await machine.readl(TIMROT_BASE + 0x030)) >>> 16;

    await machine.writel(TIMROT_BASE + 0x024, 0x00000080);
    await machine.clockStep(1_000);
    const runningAfterUpdate = (await machine.readl(TIMROT_BASE + 0x030)) >>> 16;

    assert.ok(
      runningAfterUpdate < runningBeforeUpdate,
      `TIMROT UPDATE alone must not reload running count: before=${runningBeforeUpdate}, after=${runningAfterUpdate}`,
    );
  });
}

async function testTimrotExternalEdgeContract() {
  await withMachine(async (machine) => {
    const pwm0Active = 0x00010000;
    const pwm0Period = 0x004b0003;

    await machine.writel(TIMROT_BASE + 0x008, 0xc0000000);

    await machine.writel(TIMROT_BASE + 0x020, 0x00000081);
    await machine.writel(TIMROT_BASE + 0x030, 1);
    await machine.writel(TIMROT_BASE + 0x040, 0x00000181);
    await machine.writel(TIMROT_BASE + 0x050, 1);

    await machine.writel(PWM_BASE + 0x008, 0xc0000000);
    await machine.writel(PWM_BASE + 0x010, pwm0Active);
    await machine.writel(PWM_BASE + 0x020, pwm0Period);
    await machine.writel(PWM_BASE + 0x004, 0x00000001);

    assert.notEqual(
      (await machine.readl(TIMROT_BASE + 0x020)) & 0x8000,
      0,
      'TIMROT POLARITY=0 must decrement a PWM-selected timer on PWM rising edges',
    );
    assert.equal(
      (await machine.readl(TIMROT_BASE + 0x040)) & 0x8000,
      0,
      'TIMROT POLARITY=1 must not decrement a PWM-selected timer on PWM rising edges',
    );

    await machine.clockStep(1_400);
    assert.notEqual(
      (await machine.readl(TIMROT_BASE + 0x040)) & 0x8000,
      0,
      'TIMROT POLARITY=1 must decrement a PWM-selected timer on PWM falling edges',
    );
  });
}

async function testTimrotDutyCycleContract() {
  await withMachine(async (machine) => {
    await machine.writel(TIMROT_BASE + 0x008, 0xc0000000);
    await machine.writel(TIMROT_BASE + 0x080, 0x0002020c);
    assert.equal(
      await machine.readl(TIMROT_BASE + 0x080),
      0x0002020c,
      'TIMROT Timer3 control must decode at its documented 0x80 offset',
    );

    await machine.writel(PWM_BASE + 0x008, 0xc0000000);
    await machine.writel(PWM_BASE + 0x030, 0x00010000);
    await machine.writel(PWM_BASE + 0x040, 0x000b0003);
    await machine.writel(PWM_BASE + 0x004, 0x00000002);

    await machine.clockStep(1_000);

    const dutyCtrl = await machine.readl(TIMROT_BASE + 0x080);
    const dutyCount = await machine.readl(TIMROT_BASE + 0x090);

    assert.notEqual(
      dutyCtrl & 0x00000400,
      0,
      `TIMROT Timer3 must set DUTY_VALID after sampling PWM1 high and low intervals: ctrl=0x${dutyCtrl.toString(16)}, count=0x${dutyCount.toString(16)}`,
    );
    assert.notEqual(
      dutyCount >>> 16,
      0,
      'TIMROT Timer3 duty mode must latch a nonzero low interval on PWM1 rising edge',
    );
    assert.notEqual(
      dutyCount & 0xffff,
      0,
      'TIMROT Timer3 duty mode must latch a nonzero high interval on PWM1 falling edge',
    );

    await machine.writel(TIMROT_BASE + 0x080, 0x0002020c);
    assert.equal(
      (await machine.readl(TIMROT_BASE + 0x080)) & 0x00000400,
      0,
      'TIMROT Timer3 control writes must clear DUTY_VALID while duty mode remains enabled',
    );

    await machine.writel(TIMROT_BASE + 0x088, 0x00000200);
    assert.equal(
      (await machine.readl(TIMROT_BASE + 0x080)) & 0x00000400,
      0,
      'TIMROT Timer3 must clear DUTY_VALID when duty-cycle mode is disabled',
    );
  });
}

async function testTimrotRotaryContract() {
  await withMachine(async (machine) => {
    const pwmActive = 0x27100000;
    const pwmPeriod = 0x000b4e20;

    await machine.writel(TIMROT_BASE + 0x000, 0x00000c32);
    await machine.writel(PWM_BASE + 0x008, 0xc0000000);
    await machine.writel(PWM_BASE + 0x030, pwmActive);
    await machine.writel(PWM_BASE + 0x040, pwmPeriod);
    await machine.writel(PWM_BASE + 0x050, pwmActive);
    await machine.writel(PWM_BASE + 0x060, pwmPeriod);

    await machine.writel(PWM_BASE + 0x004, 0x00000002);
    await machine.clockStep(208_000);
    await machine.writel(PWM_BASE + 0x004, 0x00000004);
    await machine.clockStep(5_000_000);

    const absoluteCount = await machine.readl(TIMROT_BASE + 0x010);
    assert.equal(
      absoluteCount >>> 16,
      0,
      'TIMROT ROTCOUNT must keep its reserved upper half clear',
    );
    assert.notEqual(
      absoluteCount & 0xffff,
      0,
      'TIMROT rotary decoder must count legal PWM1/PWM2 quadrature transitions',
    );

    await machine.writel(TIMROT_BASE + 0x004, 0x00001000);
    const relativeCount = await machine.readl(TIMROT_BASE + 0x010);
    assert.notEqual(
      relativeCount & 0xffff,
      0,
      'TIMROT relative ROTCOUNT read must report the accumulated signed count',
    );
    assert.equal(
      await machine.readl(TIMROT_BASE + 0x010),
      0,
      'TIMROT relative ROTCOUNT read must clear the counter as a side effect',
    );
  });
}

async function testTimrotRotaryInvalidTransitionContract() {
  await withMachine(async (machine) => {
    const pwmActive = 0x27100000;
    const pwmPeriod = 0x000b4e20;

    await machine.writel(TIMROT_BASE + 0x000, 0x00000c32);
    await machine.writel(PWM_BASE + 0x008, 0xc0000000);
    await machine.writel(PWM_BASE + 0x030, pwmActive);
    await machine.writel(PWM_BASE + 0x040, pwmPeriod);
    await machine.writel(PWM_BASE + 0x050, pwmActive);
    await machine.writel(PWM_BASE + 0x060, pwmPeriod);

    await machine.writel(PWM_BASE + 0x004, 0x00000006);
    await machine.clockStep(2_000_000);

    assert.equal(
      await machine.readl(TIMROT_BASE + 0x010),
      0,
      'TIMROT rotary decoder must ignore invalid direct BA=00 to BA=11 transitions',
    );
  });
}

async function testPwmRegisterContract() {
  await withMachine(async (machine) => {
    assert.equal(
      await machine.readl(PWM_BASE + 0x000),
      0xfe000000,
      'PWM CTRL reset must retain SFTRST, CLKGATE, and five present bits',
    );
    assert.equal(
      await machine.readl(PWM_BASE + 0x0b0),
      0x01010000,
      'PWM VERSION must be v1.1 at the documented offset',
    );

    await machine.writel(PWM_BASE + 0x000, 0x0000003f);
    assert.equal(
      await machine.readl(PWM_BASE + 0x000),
      0x3e00003f,
      'PWM CTRL must preserve present bits and only store documented writable bits',
    );
    await machine.writel(PWM_BASE + 0x008, 0x0000003f);
    assert.equal(
      await machine.readl(PWM_BASE + 0x000),
      0x3e000000,
      'PWM CTRL_CLR must only clear documented channel enable bits',
    );
    await machine.writel(PWM_BASE + 0x000, 0x80000000);
    assert.equal(
      await machine.readl(PWM_BASE + 0x000),
      0xfe000000,
      'PWM SFTRST must reset the block and automatically gate its clock',
    );

    for (let channel = 0; channel < 5; channel += 1) {
      const activeOffset = 0x010 + channel * 0x020;
      const periodOffset = 0x020 + channel * 0x020;
      const active = (0x10203040 + channel) >>> 0;
      const period = (0xff234567 + channel) >>> 0;

      await machine.writel(PWM_BASE + activeOffset, active);
      await machine.writel(PWM_BASE + periodOffset, period);
      assert.equal(
        await machine.readl(PWM_BASE + activeOffset),
        active,
        `PWM ACTIVE${channel} must use its documented register offset`,
      );
      assert.equal(
        await machine.readl(PWM_BASE + periodOffset),
        period & 0x00ffffff,
        `PWM PERIOD${channel} must ignore reserved bits 31:24 at its documented register offset`,
      );

      await machine.writel(PWM_BASE + activeOffset + 0x004, 0x00000003);
      await machine.writel(PWM_BASE + activeOffset + 0x008, 0x00000001);
      await machine.writel(PWM_BASE + activeOffset + 0x00c, 0x00000006);
      assert.equal(
        await machine.readl(PWM_BASE + activeOffset),
        (active | 0x00000003) & ~0x00000001 ^ 0x00000006,
        `PWM ACTIVE${channel} SET/CLR/TOG aliases must update the documented register`,
      );

      await machine.writel(PWM_BASE + periodOffset + 0x004, 0x00000003);
      await machine.writel(PWM_BASE + periodOffset + 0x008, 0x00000001);
      await machine.writel(PWM_BASE + periodOffset + 0x00c, 0x00000006);
      assert.equal(
        await machine.readl(PWM_BASE + periodOffset),
        (((period & 0x00ffffff) | 0x00000003) & ~0x00000001 ^ 0x00000006) & 0x00ffffff,
        `PWM PERIOD${channel} SET/CLR/TOG aliases must preserve reserved bits`,
      );
    }

    assert.equal(
      await machine.readl(PWM_BASE + 0x100),
      0,
      'PWM must not expose the obsolete synthetic PERIOD register map',
    );
  });
}

async function testPwmWaveformContract() {
  await withMachine(async (machine) => {
    const pwm0Period = 0x004b0003;

    await machine.writel(PINCTRL_BASE + 0x140, 0);
    await machine.writel(PWM_BASE + 0x008, 0xc0000000);
    await machine.writel(PWM_BASE + 0x010, 0x00010000);
    await machine.writel(PWM_BASE + 0x020, pwm0Period);
    await machine.writel(PWM_BASE + 0x004, 0x00000001);

    assert.notEqual(
      (await machine.readl(PINCTRL_BASE + 0x520)) & 0x1,
      0,
      'PWM0 active state must drive Bank 2 Pin 0 when muxed to PWM0 and enabled',
    );
    await machine.clockStep(1_400);
    assert.equal(
      (await machine.readl(PINCTRL_BASE + 0x520)) & 0x1,
      0,
      'PWM0 must enter its programmed inactive state after the ACTIVE.INACTIVE count',
    );

    await machine.writel(PWM_BASE + 0x010, 0x00000000);
    await machine.writel(PWM_BASE + 0x020, pwm0Period);
    assert.equal(
      (await machine.readl(PINCTRL_BASE + 0x520)) & 0x1,
      0,
      'PWM0 ACTIVE/PERIOD rewrite must not take effect in the middle of a period',
    );
    await machine.clockStep(700);
    assert.equal(
      (await machine.readl(PINCTRL_BASE + 0x520)) & 0x1,
      0,
      'PWM0 staged parameters must remain pending until the next period boundary',
    );
    await machine.clockStep(700);
    assert.notEqual(
      (await machine.readl(PINCTRL_BASE + 0x520)) & 0x1,
      0,
      'PWM0 staged ACTIVE/PERIOD parameters must commit at the next period boundary',
    );

    await machine.writel(PWM_BASE + 0x004, 0x40000000);
    await machine.clockStep(1_400);
    assert.notEqual(
      (await machine.readl(PINCTRL_BASE + 0x520)) & 0x1,
      0,
      'PWM CLKGATE must freeze the currently driven output state',
    );
    await machine.writel(PWM_BASE + 0x008, 0x40000000);
    await machine.clockStep(700);
    assert.equal(
      (await machine.readl(PINCTRL_BASE + 0x520)) & 0x1,
      0,
      'PWM must resume its preserved phase after CLKGATE is cleared',
    );

    await machine.writel(PWM_BASE + 0x004, 0x80000000);
    assert.equal(
      await machine.readl(PWM_BASE + 0x000),
      0xfe000000,
      'PWM SFTRST must reset the block and automatically gate its clock',
    );
    assert.equal(
      await machine.readl(PWM_BASE + 0x010),
      0,
      'PWM SFTRST must clear staged ACTIVE state',
    );
  });
}

async function testPwmMattContract() {
  await withMachine(async (machine) => {
    await machine.writel(PINCTRL_BASE + 0x140, 0);
    await machine.writel(PWM_BASE + 0x008, 0xc0000000);
    await machine.writel(PWM_BASE + 0x010, 0xffffffff);
    await machine.writel(PWM_BASE + 0x020, 0x00800000);
    await machine.writel(PWM_BASE + 0x004, 0x00000001);

    const initial = (await machine.readl(PINCTRL_BASE + 0x520)) & 0x1;
    await machine.clockStep(22);
    assert.notEqual(
      (await machine.readl(PINCTRL_BASE + 0x520)) & 0x1,
      initial,
      'PWM MATT must route the 24 MHz crystal independently of ACTIVE and PERIOD fields',
    );
    await machine.clockStep(22);
    assert.equal(
      (await machine.readl(PINCTRL_BASE + 0x520)) & 0x1,
      initial,
      'PWM MATT must return to the prior state after a 24 MHz clock period',
    );
  });
}

async function testPwm2AnalogEnableContract() {
  await withMachine(async (machine) => {
    await machine.writel(PINCTRL_BASE + 0x140, 0);
    await machine.writel(PWM_BASE + 0x008, 0xc0000000);
    await machine.writel(PWM_BASE + 0x050, 0xffffffff);
    await machine.writel(PWM_BASE + 0x060, 0x000f0003);
    await machine.writel(PWM_BASE + 0x004, 0x00000024);

    assert.equal(
      (await machine.readl(PINCTRL_BASE + 0x520)) & 0x4,
      0,
      'PWM2 analog path must disable PWM2 while LRADC BL_ENABLE is clear',
    );
    await machine.writel(LRADC_BASE + 0x024, 0x00400000);
    assert.notEqual(
      (await machine.readl(PINCTRL_BASE + 0x520)) & 0x4,
      0,
      'PWM2 analog path must enable PWM2 when LRADC BL_ENABLE is set',
    );
    await machine.writel(LRADC_BASE + 0x028, 0x00400000);
    assert.equal(
      (await machine.readl(PINCTRL_BASE + 0x520)) & 0x4,
      0,
      'clearing LRADC BL_ENABLE must disable PWM2 through the analog path',
    );
    await machine.writel(PWM_BASE + 0x008, 0x00000020);
    assert.notEqual(
      (await machine.readl(PINCTRL_BASE + 0x520)) & 0x4,
      0,
      'clearing PWM2_ANA_CTRL_ENABLE must restore ordinary PWM2 behavior',
    );
  });
}

async function testLradcRegisterContract() {
  await withMachine(async (machine) => {
    /* CTRL0 resets with SFTRST and CLKGATE asserted */
    const ctrl0 = await machine.readl(LRADC_BASE + 0x000);
    assert.equal(ctrl0, 0xc0000000, 'LRADC CTRL0 must reset with SFTRST/CLKGATE');

    /* CTRL2 resets with TEMPSENSE_PWD asserted (temperature sensor block powered down) */
    assert.equal(
      await machine.readl(LRADC_BASE + 0x020),
      0x00008000,
      'LRADC CTRL2 must reset with TEMPSENSE_PWD asserted',
    );

    /* Reserved bits in CTRL1-3 and CONVERSION read as zero and are not writable */
    await machine.writel(LRADC_BASE + 0x010, 0xffffffff);
    assert.equal(
      await machine.readl(LRADC_BASE + 0x010),
      0x01ff01ff,
      'LRADC CTRL1 must mask reserved bits 31:25 and 15:9',
    );
    await machine.writel(LRADC_BASE + 0x020, 0xffffffff);
    assert.equal(
      await machine.readl(LRADC_BASE + 0x020),
      0xffffb3ff,
      'LRADC CTRL2 must mask reserved bits 14 and 11:10',
    );
    await machine.writel(LRADC_BASE + 0x030, 0xffffffff);
    assert.equal(
      await machine.readl(LRADC_BASE + 0x030),
      0x03c00333,
      'LRADC CTRL3 must mask reserved bits 31:26, 21:14, 13:10, 7:6, 3:2',
    );
    await machine.writel(LRADC_BASE + 0x130, 0xffffffff);
    assert.equal(
      await machine.readl(LRADC_BASE + 0x130),
      0x001303ff,
      'LRADC CONVERSION must mask reserved bits 31:21, 19:18, 15:10',
    );

    /* Channel 7 defaults to the disconnected-battery value used by ExistOS */
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x0c0)) & 0x3ffff,
      2748,
      'LRADC CH7 VALUE must reset to 2748 (disconnected battery)',
    );

    /* CH0-6 bit 30 is reserved; CH7 bit 30 is TESTMODE_TOGGLE (kept RO) */
    await machine.writel(LRADC_BASE + 0x050, 0xffffffff);
    assert.equal(
      await machine.readl(LRADC_BASE + 0x050),
      0xbf03ffff,
      'LRADC CH0 must mask reserved bits 30 and 23:18',
    );
    await machine.writel(LRADC_BASE + 0x0c0, 0xffffffff);
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x0c0)) & 0x3ffff,
      0x3ffff,
      'LRADC CH7 VALUE must be writable (software value semantics)',
    );

    /* SFTRST resets the entire LRADC block, restoring CH7 default */
    await machine.writel(LRADC_BASE + 0x000, 0x80000000);
    assert.equal(
      await machine.readl(LRADC_BASE + 0x000),
      0xc0000000,
      'LRADC SFTRST must gate clock and remain asserted',
    );
    assert.equal(
      await machine.readl(LRADC_BASE + 0x010),
      0,
      'LRADC SFTRST must clear CTRL1',
    );
    assert.equal(
      await machine.readl(LRADC_BASE + 0x020),
      0x00008000,
      'LRADC SFTRST must restore CTRL2 TEMPSENSE_PWD',
    );
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x0c0)) & 0x3ffff,
      2748,
      'LRADC SFTRST must restore CH7 default value',
    );
  });
}

async function testLradcIrqContract() {
  await withMachine(async (machine) => {
    /* Bring LRADC out of reset and enable LRADC0/7 interrupts */
    await machine.writel(LRADC_BASE + 0x000, 0x00000000);
    await machine.writel(LRADC_BASE + 0x010, 0x00810081); /* LRADC0/7 IRQ_EN */

    /* Schedule channels 0 and 7 */
    await machine.writel(LRADC_BASE + 0x000, 0x00000081);

    /* LRADC0/7 IRQ status should be set and the ICOLL lines should assert */
    const ctrl1 = await machine.readl(LRADC_BASE + 0x010);
    assert.notEqual(ctrl1 & 0x0001, 0, 'LRADC0 IRQ status must be set after schedule');
    assert.notEqual(ctrl1 & 0x0080, 0, 'LRADC7 IRQ status must be set after schedule');

    const raw1 = await machine.readl(ICOLL_BASE + 0x050);
    assert.notEqual(raw1 & (1 << 5), 0, 'LRADC0 must assert ICOLL source 37');
    assert.notEqual(raw1 & (1 << 12), 0, 'LRADC7 must assert ICOLL source 44');

    /* Clear LRADC0/7 IRQ status via CTRL1_CLR */
    await machine.writel(LRADC_BASE + 0x018, 0x00810081);
    assert.equal(
      await machine.readl(LRADC_BASE + 0x010),
      0,
      'LRADC0/7 IRQ status must clear after CLR write',
    );
    assert.equal(
      await machine.readl(ICOLL_BASE + 0x050) & ((1 << 5) | (1 << 12)),
      0,
      'LRADC0/7 ICOLL sources must deassert when IRQ cleared',
    );

    /* Touch detect IRQ should not assert if only enabled but not pending */
    await machine.writel(LRADC_BASE + 0x014, 0x01000000); /* SET TOUCH_DETECT_IRQ_EN only */
    assert.equal(
      await machine.readl(ICOLL_BASE + 0x050) & (1 << 4),
      0,
      'TOUCH_DETECT_IRQ must not assert when status is clear',
    );
    /* Software can force the touch status bit; when pending + enabled, it asserts */
    await machine.writel(LRADC_BASE + 0x014, 0x00000100); /* SET TOUCH_DETECT_IRQ */
    assert.notEqual(
      await machine.readl(ICOLL_BASE + 0x050) & (1 << 4),
      0,
      'TOUCH_DETECT_IRQ must assert when status is set and enabled',
    );
    await machine.writel(LRADC_BASE + 0x018, 0x01000100); /* CLR touch */
  });
}

async function testLradcSchedulerContract() {
  await withMachine(async (machine) => {
    /* Bring LRADC out of reset and enable LRADC0/1 IRQs (status bits remain clear) */
    await machine.writel(LRADC_BASE + 0x000, 0x00000000);
    await machine.writel(LRADC_BASE + 0x010, 0x00030000); /* LRADC0/1 IRQ_EN */

    /* Configure CH0 for accumulate mode with NUM_SAMPLES=3 and VALUE=0 */
    await machine.writel(LRADC_BASE + 0x050, 0x23000000);

    /* Schedule CH0; first accumulated sample should not raise IRQ */
    await machine.writel(LRADC_BASE + 0x000, 0x00000001);
    assert.equal(
      await machine.readl(LRADC_BASE + 0x010) & 0x0001,
      0,
      'LRADC0 IRQ must not assert after first accumulated sample',
    );
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0xabc,
      'CH0 value must be 0xabc after first accumulated sample',
    );

    /* Schedule CH0 two more times; IRQ should assert after third sample */
    await machine.writel(LRADC_BASE + 0x000, 0x00000001);
    await machine.writel(LRADC_BASE + 0x000, 0x00000001);
    assert.notEqual(
      await machine.readl(LRADC_BASE + 0x010) & 0x0001,
      0,
      'LRADC0 IRQ must assert after third accumulated sample',
    );
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0x2034,
      'CH0 value must be 0x2034 after three accumulated samples',
    );

    /* Clear LRADC0 IRQ and reset CH0 to non-accumulate mode for delay tests */
    await machine.writel(LRADC_BASE + 0x018, 0x00010001);
    await machine.writel(LRADC_BASE + 0x050, 0x00000000);

    /* Test delay-driven scheduling: DELAY0 triggers CH0 after 5 ticks */
    await machine.writel(LRADC_BASE + 0x0d0, 0x01100005); /* TRIGGER_LRADCS=0x01, KICK, DELAY=5 */
    await machine.clockStep(5 * 500_000);
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0xabc,
      'CH0 value must be 0xabc after DELAY0 triggers CH0',
    );
    assert.notEqual(
      await machine.readl(LRADC_BASE + 0x010) & 0x0001,
      0,
      'LRADC0 IRQ must assert after DELAY0 triggers CH0',
    );

    /* Clear LRADC0 IRQ */
    await machine.writel(LRADC_BASE + 0x018, 0x00010001);

    /* Test delay loop: DELAY0 triggers CH0 twice with LOOP_COUNT=1 */
    await machine.writel(LRADC_BASE + 0x0d0, 0x01100805); /* TRIGGER_LRADCS=0x01, KICK, LOOP_COUNT=1, DELAY=5 */
    await machine.clockStep(5 * 500_000);
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0xabc,
      'CH0 value must be 0xabc after first DELAY0 loop iteration',
    );
    await machine.clockStep(5 * 500_000);
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0xabc,
      'CH0 value must be 0xabc after second DELAY0 loop iteration',
    );
    assert.notEqual(
      await machine.readl(LRADC_BASE + 0x010) & 0x0001,
      0,
      'LRADC0 IRQ must assert after second DELAY0 loop iteration',
    );

    /* Test delay chaining: DELAY0 triggers DELAY1, which triggers CH1 */
    await machine.writel(LRADC_BASE + 0x0d0, 0x01120005); /* TRIGGER_LRADCS=0x01, TRIGGER_DELAYS=0x02, KICK, DELAY=5 */
    await machine.writel(LRADC_BASE + 0x0e0, 0x02000005); /* DELAY1 triggers CH1, DELAY=5 */
    await machine.clockStep(10 * 500_000);
    assert.notEqual(
      await machine.readl(LRADC_BASE + 0x010) & 0x0002,
      0,
      'LRADC1 IRQ must assert after DELAY1 is triggered by DELAY0',
    );
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x060)) & 0x3ffff,
      0xabc,
      'CH1 value must be 0xabc after DELAY1 triggers CH1',
    );
  });
}

async function testLradcTouchTemperatureContract() {
  await withMachine(async (machine) => {
    /* STATUS reports touch panel and temperature sources present, no touch raw */
    assert.equal(
      await machine.readl(LRADC_BASE + 0x040),
      0x07ff0000,
      'LRADC STATUS must report touch panel and all channels present',
    );

    /* Enable touch detection and the touch IRQ */
    await machine.writel(LRADC_BASE + 0x000, 0x00100000); /* TOUCH_DETECT_ENABLE */
    assert.equal(
      await machine.readl(LRADC_BASE + 0x040),
      0x07ff0000,
      'TOUCH_DETECT_RAW must be 0 when no touch is active',
    );

    await machine.writel(LRADC_BASE + 0x010, 0x01000100); /* TOUCH_DETECT_IRQ + EN */
    assert.notEqual(
      await machine.readl(ICOLL_BASE + 0x050) & (1 << 4),
      0,
      'TOUCH_DETECT_IRQ must assert ICOLL source 36',
    );
    await machine.writel(LRADC_BASE + 0x018, 0x01000100); /* clear */
    assert.equal(
      await machine.readl(ICOLL_BASE + 0x050) & (1 << 4),
      0,
      'TOUCH_DETECT_IRQ must deassert when cleared',
    );

    /* Hardware touch-detect input via the named GPIO line */
    await machine.writel(LRADC_BASE + 0x010, 0x01000000); /* TOUCH_DETECT_IRQ_EN only */
    await machine.setIrqIn('/machine/soc/lradc', 'touch-detect', 0, 1);
    assert.equal(
      await machine.readl(LRADC_BASE + 0x040),
      0x07ff0001,
      'TOUCH_DETECT_RAW must be 1 when touch-detect input is active and enabled',
    );
    assert.notEqual(
      await machine.readl(LRADC_BASE + 0x010) & 0x0100,
      0,
      'TOUCH_DETECT_IRQ status must be set by the touch-detect input',
    );
    assert.notEqual(
      await machine.readl(ICOLL_BASE + 0x050) & (1 << 4),
      0,
      'TOUCH_DETECT_IRQ must assert ICOLL source 36 when touch-detect input is active',
    );

    await machine.setIrqIn('/machine/soc/lradc', 'touch-detect', 0, 0);
    assert.equal(
      await machine.readl(LRADC_BASE + 0x040),
      0x07ff0000,
      'TOUCH_DETECT_RAW must return to 0 when touch-detect input is released',
    );
    assert.notEqual(
      await machine.readl(LRADC_BASE + 0x010) & 0x0100,
      0,
      'TOUCH_DETECT_IRQ status must remain sticky when touch-detect input is released',
    );

    await machine.writel(LRADC_BASE + 0x018, 0x00000100); /* clear TOUCH_DETECT_IRQ */
    assert.equal(
      await machine.readl(LRADC_BASE + 0x010) & 0x0100,
      0,
      'TOUCH_DETECT_IRQ status must clear when input is released and software clears it',
    );
    assert.equal(
      await machine.readl(ICOLL_BASE + 0x050) & (1 << 4),
      0,
      'TOUCH_DETECT_IRQ must deassert when status is cleared',
    );

    /* Temperature sensor mapping: CH0 -> physical 8, TEMPSENSE_PWD disabled first */
    await machine.writel(LRADC_BASE + 0x140, 0x76543218);
    await machine.writel(LRADC_BASE + 0x020, 0x00000000); /* clear TEMPSENSE_PWD */
    await machine.writel(LRADC_BASE + 0x010, 0x00010001); /* enable LRADC0 IRQ */
    await machine.writel(LRADC_BASE + 0x000, 0x00000001); /* schedule CH0 */
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0x400,
      'CH0 mapped to physical 8 with TEMPSENSE enabled must be 0x400',
    );

    await machine.writel(LRADC_BASE + 0x018, 0x00010001); /* clear LRADC0 IRQ */
    await machine.writel(LRADC_BASE + 0x140, 0x76543219); /* CH0 -> physical 9 */
    await machine.writel(LRADC_BASE + 0x000, 0x00000001); /* schedule CH0 */
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0x800,
      'CH0 mapped to physical 9 with TEMPSENSE enabled must be 0x800',
    );

    await machine.writel(LRADC_BASE + 0x018, 0x00010001); /* clear LRADC0 IRQ */
    await machine.writel(LRADC_BASE + 0x020, 0x00008000); /* set TEMPSENSE_PWD */
    await machine.writel(LRADC_BASE + 0x000, 0x00000001); /* schedule CH0 */
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0,
      'CH0 mapped to physical 9 with TEMPSENSE powered down must be 0',
    );

    await machine.writel(LRADC_BASE + 0x018, 0x00010001); /* clear LRADC0 IRQ */
    await machine.writel(LRADC_BASE + 0x140, 0x76543210); /* CH0 -> physical 0 */
    await machine.writel(LRADC_BASE + 0x000, 0x00000001); /* schedule CH0 */
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0xabc,
      'CH0 mapped to physical 0 must be 0xabc',
    );
  });
}

async function testLradcDivideByTwoContract() {
  await withMachine(async (machine) => {
    /* Bring LRADC out of reset; CH0 maps to physical 0 by default */
    await machine.writel(LRADC_BASE + 0x000, 0x00000000);

    await machine.writel(LRADC_BASE + 0x000, 0x00000001); /* schedule CH0 */
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0xabc,
      'CH0 physical 0 must be 0xabc without divide-by-two',
    );

    await machine.writel(LRADC_BASE + 0x020, 0x01000000); /* DIVIDE_BY_TWO for CH0 */
    await machine.writel(LRADC_BASE + 0x000, 0x00000001); /* schedule CH0 */
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0x55e,
      'CH0 physical 0 must be halved to 0x55e when DIVIDE_BY_TWO is set',
    );

    await machine.writel(LRADC_BASE + 0x140, 0x76543218); /* CH0 -> physical 8 */
    await machine.writel(LRADC_BASE + 0x000, 0x00000001); /* schedule CH0 */
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0x200,
      'CH0 physical 8 must be 0x200 with DIVIDE_BY_TWO and TEMPSENSE enabled',
    );

    await machine.writel(LRADC_BASE + 0x140, 0x76543219); /* CH0 -> physical 9 */
    await machine.writel(LRADC_BASE + 0x000, 0x00000001); /* schedule CH0 */
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0x400,
      'CH0 physical 9 must be 0x400 with DIVIDE_BY_TWO and TEMPSENSE enabled',
    );

    await machine.writel(LRADC_BASE + 0x020, 0x01008000); /* DIVIDE_BY_TWO + TEMPSENSE_PWD */
    await machine.writel(LRADC_BASE + 0x000, 0x00000001); /* schedule CH0 */
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0,
      'CH0 physical 9 must read 0 when TEMPSENSE_PWD is set even with DIVIDE_BY_TWO',
    );
  });
}

async function testLradcTempCurrentContract() {
  await withMachine(async (machine) => {
    /* Bring LRADC out of reset; CH0 maps to physical 0 by default */
    await machine.writel(LRADC_BASE + 0x000, 0x00000000);

    /* Disable current source: physical 0 behaves as a generic input */
    await machine.writel(LRADC_BASE + 0x000, 0x00000001); /* schedule CH0 */
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0xabc,
      'CH0 physical 0 must be 0xabc when TEMP_SENSOR_IENABLE0 is disabled',
    );

    /* Enable LRADC0 current source at 300 uA (0xF) */
    await machine.writel(LRADC_BASE + 0x020, 0x0000010f);
    await machine.writel(LRADC_BASE + 0x000, 0x00000001);
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0xf00,
      'CH0 physical 0 must be 0xf00 when TEMP_SENSOR_IENABLE0 is enabled with ISRC=0xF',
    );

    /* Change ISRC0 to 160 uA (0x8) */
    await machine.writel(LRADC_BASE + 0x020, 0x00000108);
    await machine.writel(LRADC_BASE + 0x000, 0x00000001);
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0x800,
      'CH0 physical 0 must be 0x800 when TEMP_SENSOR_IENABLE0 is enabled with ISRC=0x8',
    );

    /* ISRC0 = 0 (0 uA) should read as zero */
    await machine.writel(LRADC_BASE + 0x020, 0x00000100);
    await machine.writel(LRADC_BASE + 0x000, 0x00000001);
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0,
      'CH0 physical 0 must read 0 when TEMP_SENSOR_IENABLE0 is enabled with ISRC=0',
    );

    /* Disable current source: generic input returns */
    await machine.writel(LRADC_BASE + 0x020, 0x00000000);
    await machine.writel(LRADC_BASE + 0x000, 0x00000001);
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0xabc,
      'CH0 physical 0 must return to 0xabc when TEMP_SENSOR_IENABLE0 is disabled',
    );

    /* Map CH0 to physical 1 and enable LRADC1 current source at 80 uA (0x4) */
    await machine.writel(LRADC_BASE + 0x140, 0x76543211);
    await machine.writel(LRADC_BASE + 0x020, 0x00000240);
    await machine.writel(LRADC_BASE + 0x000, 0x00000001);
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0x400,
      'CH0 physical 1 must be 0x400 when TEMP_SENSOR_IENABLE1 is enabled with ISRC=0x4',
    );

    /* Combine with DIVIDE_BY_TWO: 0xF00 -> 0x780 */
    await machine.writel(LRADC_BASE + 0x140, 0x76543210);
    await machine.writel(LRADC_BASE + 0x020, 0x0100010f);
    await machine.writel(LRADC_BASE + 0x000, 0x00000001);
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0x780,
      'CH0 physical 0 with ISRC=0xF and DIVIDE_BY_TWO must be halved to 0x780',
    );
  });
}

async function testLradcCtrl3PowerAndDiscardContract() {
  await withMachine(async (machine) => {
    /* Bring LRADC out of reset; CH0 maps to physical 0 by default */
    await machine.writel(LRADC_BASE + 0x000, 0x00000000);

    /* Normal operation: CH0 should read the generic input value 0xabc */
    await machine.writel(LRADC_BASE + 0x000, 0x00000001);
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0xabc,
      'CH0 must read 0xabc when analog is powered normally',
    );

    /* FORCE_ANALOG_PWDN (bit 22) forces analog down: conversion returns 0 */
    await machine.writel(LRADC_BASE + 0x030, 0x00400000);
    await machine.writel(LRADC_BASE + 0x000, 0x00000001);
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0,
      'FORCE_ANALOG_PWDN must force LRADC conversion to 0',
    );

    /* FORCE_ANALOG_PWUP (bit 23) overrides PWDN and powers the analog back up */
    await machine.writel(LRADC_BASE + 0x030, 0x00c00000);
    await machine.writel(LRADC_BASE + 0x000, 0x00000001);
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0xabc,
      'FORCE_ANALOG_PWUP must override PWDN and restore 0xabc',
    );

    /* Clear force bits to return to normal operation */
    await machine.writel(LRADC_BASE + 0x038, 0x00c00000);
    await machine.writel(LRADC_BASE + 0x000, 0x00000001);
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0xabc,
      'clearing force bits must restore normal 0xabc',
    );

    /* DISCARD = 0x1 (discard 1 sample after analog power-up). Gate and ungate clock. */
    await machine.writel(LRADC_BASE + 0x030, 0x01000000);
    await machine.writel(LRADC_BASE + 0x000, 0x40000000); /* CLKGATE */
    await machine.writel(LRADC_BASE + 0x000, 0x00000000); /* ungate */
    await machine.writel(LRADC_BASE + 0x000, 0x00000001); /* first sample discarded */
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0,
      'DISCARD=1 first sample after power-up must be discarded',
    );
    await machine.writel(LRADC_BASE + 0x000, 0x00000001); /* second sample valid */
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0xabc,
      'DISCARD=1 second sample after power-up must be 0xabc',
    );

    /* DISCARD = 0x2 (discard 2 samples). Gate and ungate clock again. */
    await machine.writel(LRADC_BASE + 0x030, 0x02000000);
    await machine.writel(LRADC_BASE + 0x000, 0x40000000);
    await machine.writel(LRADC_BASE + 0x000, 0x00000000);
    await machine.writel(LRADC_BASE + 0x000, 0x00000001);
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0,
      'DISCARD=2 first sample must be discarded',
    );
    await machine.writel(LRADC_BASE + 0x000, 0x00000001);
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0,
      'DISCARD=2 second sample must be discarded',
    );
    await machine.writel(LRADC_BASE + 0x000, 0x00000001);
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0xabc,
      'DISCARD=2 third sample must be 0xabc',
    );

    /* DISCARD = 0x3 discards 3 samples. */
    await machine.writel(LRADC_BASE + 0x030, 0x03000000);
    await machine.writel(LRADC_BASE + 0x000, 0x40000000);
    await machine.writel(LRADC_BASE + 0x000, 0x00000000);
    await machine.writel(LRADC_BASE + 0x000, 0x00000001);
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0,
      'DISCARD=3 first sample must be discarded',
    );
    await machine.writel(LRADC_BASE + 0x000, 0x00000001);
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0,
      'DISCARD=3 second sample must be discarded',
    );
    await machine.writel(LRADC_BASE + 0x000, 0x00000001);
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0,
      'DISCARD=3 third sample must be discarded',
    );
    await machine.writel(LRADC_BASE + 0x000, 0x00000001);
    assert.equal(
      (await machine.readl(LRADC_BASE + 0x050)) & 0x3ffff,
      0xabc,
      'DISCARD=3 fourth sample must be 0xabc',
    );
  });
}

async function testLradcStatusAndCtrl3ClockContract() {
  await withMachine(async (machine) => {
    /* STATUS present bits and reset */
    assert.equal(
      await machine.readl(LRADC_BASE + 0x040),
      0x07ff0000,
      'LRADC STATUS must report all channels, touch and temperature sources present',
    );

    /* TOUCH_DETECT_RAW follows touch-detect input only when TOUCH_DETECT_ENABLE is set */
    await machine.setIrqIn('/machine/soc/lradc', 'touch-detect', 0, 1);
    assert.equal(
      await machine.readl(LRADC_BASE + 0x040),
      0x07ff0000,
      'TOUCH_DETECT_RAW must stay 0 when TOUCH_DETECT_ENABLE is not set',
    );

    await machine.writel(LRADC_BASE + 0x000, 0x00100000); /* TOUCH_DETECT_ENABLE */
    assert.equal(
      await machine.readl(LRADC_BASE + 0x040),
      0x07ff0001,
      'TOUCH_DETECT_RAW must follow touch-detect input when enabled by CTRL0',
    );

    await machine.writel(LRADC_BASE + 0x000, 0x00000000); /* clear TOUCH_DETECT_ENABLE */
    assert.equal(
      await machine.readl(LRADC_BASE + 0x040),
      0x07ff0000,
      'TOUCH_DETECT_RAW must be gated off when TOUCH_DETECT_ENABLE is cleared',
    );

    await machine.setIrqIn('/machine/soc/lradc', 'touch-detect', 0, 0);

    /* CTRL3 clock parameters are writable and readable */
    await machine.writel(LRADC_BASE + 0x030, 0x03000000); /* CYCLE_TIME=0x3, DISCARD=0x3 */
    assert.equal(
      await machine.readl(LRADC_BASE + 0x030),
      0x03000000,
      'CTRL3 CYCLE_TIME=0x3 and DISCARD=0x3 must be readable',
    );

    await machine.writel(LRADC_BASE + 0x030, 0x00000033); /* HIGH_TIME=0x3, DELAY_CLOCK=1, INVERT_CLOCK=1 */
    assert.equal(
      await machine.readl(LRADC_BASE + 0x030),
      0x00000033,
      'CTRL3 HIGH_TIME/DELAY_CLOCK/INVERT_CLOCK must be readable',
    );

    await machine.writel(LRADC_BASE + 0x030, 0x00000000); /* restore */
  });
}

async function testI2cRegisterContract() {
  await withMachine(async (machine) => {
    assert.equal(
      await machine.readl(I2C_BASE + 0x000),
      0xc0000000,
      'I2C CTRL0 must reset with SFTRST and CLKGATE asserted',
    );
    assert.equal(
      await machine.readl(I2C_BASE + 0x010),
      0x00780030,
      'I2C TIMING0 must occupy 0x10 and expose its documented reset value',
    );
    assert.equal(
      await machine.readl(I2C_BASE + 0x020),
      0x00800030,
      'I2C TIMING1 must occupy 0x20 and expose its documented reset value',
    );
    assert.equal(
      await machine.readl(I2C_BASE + 0x030),
      0x00300030,
      'I2C TIMING2 must occupy 0x30 and expose its documented reset value',
    );
    assert.equal(
      await machine.readl(I2C_BASE + 0x040),
      0x00860000,
      'I2C CTRL1 must occupy 0x40 and reset with slave address byte 0x86',
    );
    assert.equal(
      await machine.readl(I2C_BASE + 0x050),
      0xc0000000,
      'I2C STAT must expose fixed master and slave presence bits',
    );
    assert.equal(
      await machine.readl(I2C_BASE + 0x070),
      0x00100000,
      'I2C DEBUG0 must expose the documented reset DMA state',
    );
    assert.equal(
      await machine.readl(I2C_BASE + 0x080),
      0xc0000000,
      'I2C DEBUG1 must expose idle-high pad inputs after reset',
    );
    assert.equal(
      await machine.readl(I2C_BASE + 0x090),
      0x01010000,
      'I2C VERSION must be v1.1 at its documented offset',
    );

    for (const offset of [0x010, 0x020, 0x030]) {
      await machine.writel(I2C_BASE + offset, 0xffffffff);
      assert.equal(
        await machine.readl(I2C_BASE + offset),
        0x03ff03ff,
        `I2C timing register at 0x${offset.toString(16)} must ignore reserved bits`,
      );
    }

    await machine.writel(I2C_BASE + 0x040, 0xffffffff);
    assert.equal(
      await machine.readl(I2C_BASE + 0x040),
      0x01ffffff,
      'I2C CTRL1 must retain only documented status, enable, and slave-address fields',
    );
    assert.equal(
      await machine.readl(I2C_BASE + 0x050),
      0xe00000ff,
      'I2C STAT must summarize all enabled CTRL1 interrupt requests and reject writes',
    );
    const raw0 = await machine.readl(ICOLL_BASE + 0x040);
    assert.notEqual(
      raw0 & (1 << 27),
      0,
      'enabled I2C controller status must assert the I2C error/line-condition source',
    );
    assert.equal(
      raw0 & (1 << 26),
      0,
      'I2C controller status must not assert the APBX-owned I2C DMA source',
    );
    await machine.writel(I2C_BASE + 0x050, 0);
    assert.equal(
      await machine.readl(I2C_BASE + 0x050),
      0xe00000ff,
      'I2C STAT must be read-only',
    );

    await machine.writel(I2C_BASE + 0x060, 0x11223344);
    assert.equal(
      await machine.readl(I2C_BASE + 0x060),
      0x11223344,
      'I2C DATA must remain read/write at its documented base address',
    );
    await machine.writel(I2C_BASE + 0x064, 0x55667788);
    assert.equal(
      await machine.readl(I2C_BASE + 0x060),
      0x11223344,
      'I2C DATA must not decode undocumented SCT aliases',
    );

    await machine.writel(I2C_BASE + 0x070, 0xffffffff);
    assert.equal(
      await machine.readl(I2C_BASE + 0x070),
      0x1c100800,
      'I2C DEBUG0 must retain only TESTMODE and documented test fields',
    );
    await machine.writel(I2C_BASE + 0x080, 0xffffffff);
    assert.equal(
      await machine.readl(I2C_BASE + 0x080),
      0xc000073f,
      'I2C DEBUG1 must preserve input and reserved fields while retaining controls',
    );
    await machine.writel(I2C_BASE + 0x090, 0);
    assert.equal(
      await machine.readl(I2C_BASE + 0x090),
      0x01010000,
      'I2C VERSION must be read-only',
    );

    await machine.writel(I2C_BASE + 0x004, 0x80000000);
    assert.equal(
      await machine.readl(I2C_BASE + 0x000),
      0xc0000000,
      'I2C SFTRST must reset the block and automatically gate its clock',
    );
    assert.equal(
      await machine.readl(I2C_BASE + 0x040),
      0x00860000,
      'I2C SFTRST must restore CTRL1 reset state',
    );
  });
}

async function testI2cDmaIrqOwnershipContract() {
  await withMachine(async (machine) => {
    const descriptor = 0x00000400;
    const channel3Nxtcmdar = APBX_BASE + 0x1a0;
    const channel3Sema = APBX_BASE + 0x1d0;

    await machine.writel(APBX_BASE + 0x008, 0xc0000000);
    await machine.writel(APBX_BASE + 0x014, 1 << 11);
    await machine.writel(descriptor + 0x00, 0);
    await machine.writel(descriptor + 0x04, (1 << 6) | (1 << 3));
    await machine.writel(descriptor + 0x08, 0);
    await machine.writel(descriptor + 0x0c, 0);
    await machine.writel(channel3Nxtcmdar, descriptor);
    await machine.writel(channel3Sema, 1);
    assert.notEqual(
      (await machine.readl(ICOLL_BASE + 0x040)) & (1 << 26),
      0,
      'APBX channel 3 completion must assert the Table 38 I2C DMA source',
    );

    await machine.writel(I2C_BASE + 0x040, 0);
    assert.notEqual(
      (await machine.readl(ICOLL_BASE + 0x040)) & (1 << 26),
      0,
      'I2C device status writes must not clear the APBX-owned I2C DMA source',
    );
  });
}

async function testAppUartRegisterContract() {
  await withMachine(async (machine) => {
    assert.equal(
      await machine.readl(APPUART_BASE + 0x000),
      0xc0030000,
      'UARTAPP CTRL0 must reset with SFTRST, CLKGATE, and RXTIMEOUT=3',
    );
    assert.equal(
      await machine.readl(APPUART_BASE + 0x010),
      0,
      'UARTAPP CTRL1 must reset with no TX DMA command pending',
    );
    assert.equal(
      await machine.readl(APPUART_BASE + 0x020),
      0x00220300,
      'UARTAPP CTRL2 must reset with both FIFOs half-level and RX/TX enabled',
    );
    assert.equal(
      await machine.readl(APPUART_BASE + 0x070),
      0xc9f00000,
      'UARTAPP STAT must reset with present/high-speed, empty FIFOs, and four invalid RX bytes',
    );
    assert.equal(
      await machine.readl(APPUART_BASE + 0x080),
      0,
      'UARTAPP DEBUG must reset with all DMA signal state low',
    );
    assert.equal(
      await machine.readl(APPUART_BASE + 0x090),
      0x02000000,
      'UARTAPP VERSION must report block v2.0',
    );

    await machine.writel(APPUART_BASE + 0x010, 0xffffffff);
    assert.equal(
      await machine.readl(APPUART_BASE + 0x010),
      0x1000ffff,
      'UARTAPP CTRL1 must retain only RUN and XFER_COUNT',
    );
    await machine.writel(APPUART_BASE + 0x020, 0xffffffff);
    assert.equal(
      await machine.readl(APPUART_BASE + 0x020),
      0xff77ffc7,
      'UARTAPP CTRL2 must ignore its documented reserved fields',
    );
    await machine.writel(APPUART_BASE + 0x030, 0xffffffff);
    assert.equal(
      await machine.readl(APPUART_BASE + 0x030),
      0xffff3fff,
      'UARTAPP LINECTRL must retain only its documented baud and framing fields',
    );
    await machine.writel(APPUART_BASE + 0x040, 0xffffffff);
    assert.equal(
      await machine.readl(APPUART_BASE + 0x040),
      0xffff3ffe,
      'UARTAPP LINECTRL2 must retain its documented fields and reject BRK',
    );
    await machine.writel(APPUART_BASE + 0x050, 0xffffffff);
    assert.equal(
      await machine.readl(APPUART_BASE + 0x050),
      0x07ff07ff,
      'UARTAPP INTR must retain only documented enable and status bits',
    );
    const raw0 = await machine.readl(ICOLL_BASE + 0x040);
    assert.notEqual(
      raw0 & (1 << 24),
      0,
      'enabled UARTAPP interrupt status must assert ICOLL source 24',
    );
    assert.equal(
      raw0 & ((1 << 23) | (1 << 25)),
      0,
      'UARTAPP device status must not assert APBX-owned TX/RX DMA sources',
    );

    await machine.writel(APPUART_BASE + 0x004, 0x80000000);
    assert.equal(
      await machine.readl(APPUART_BASE + 0x000),
      0xc0030000,
      'UARTAPP SFTRST must restore the block reset state and gate clocks',
    );
  });
}

async function testDebugUartRegisterContract() {
  await withMachine(async (machine) => {
    assert.equal(
      await machine.readl(DBGUART_BASE + 0x030),
      0x00000300,
      'UARTDBG CR must reset with RXE and TXE asserted',
    );
    assert.equal(
      await machine.readl(DBGUART_BASE + 0x034),
      0x00000012,
      'UARTDBG IFLS must reset both FIFO levels to half',
    );

    await machine.writel(DBGUART_BASE + 0x030, 0xffffffff);
    assert.equal(
      await machine.readl(DBGUART_BASE + 0x030),
      0x0000ffc7,
      'UARTDBG CR must reject unavailable and reserved bits',
    );
    await machine.writel(DBGUART_BASE + 0x034, 0xffffffff);
    assert.equal(
      await machine.readl(DBGUART_BASE + 0x034),
      0x0000003f,
      'UARTDBG IFLS must expose only RX/TX FIFO level fields',
    );
    await machine.writel(DBGUART_BASE + 0x038, 0xffffffff);
    assert.equal(
      await machine.readl(DBGUART_BASE + 0x038),
      0x000007ff,
      'UARTDBG IMSC must expose only documented interrupt masks',
    );
    await machine.writel(DBGUART_BASE + 0x048, 0xffffffff);
    assert.equal(
      await machine.readl(DBGUART_BASE + 0x048),
      0x00000007,
      'UARTDBG DMACR must expose only RXDMAE, TXDMAE, and DMAONERR',
    );

    await machine.writel(DBGUART_BASE + 0x030, 0x00000381);
    await machine.writel(DBGUART_BASE + 0x000, 0x0000005a);
    assert.equal(
      await machine.readl(DBGUART_BASE + 0x000),
      0x0000005a,
      'UARTDBG LBE must feed normal-mode transmitted data back to the receive FIFO',
    );
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

async function testLcdifRegisterMapContract() {
  await withMachine(async (machine) => {
    assert.equal(
      await machine.readl(LCDIF_BASE + 0x010),
      0x000f0000,
      'LCDIF CTRL1 reset must retain BYTE_PACKING_FORMAT=0xf',
    );
    assert.equal(
      await machine.readl(LCDIF_BASE + 0x0c0),
      0x90000000,
      'LCDIF STAT reset must report PRESENT and RXFIFO_EMPTY only',
    );
    assert.equal(
      await machine.readl(LCDIF_BASE + 0x0d0),
      0x02000000,
      'LCDIF VERSION must report Reference Manual v2.0',
    );
    assert.equal(
      await machine.readl(LCDIF_BASE + 0x0e0),
      0x0e810000,
      'LCDIF DEBUG0 reset fields must be read-only Reference defaults',
    );

    const cases = [
      [0x020, 0xffffffff, 0xffffffff, 'TIMING'],
      [0x030, 0xffffffff, 0x3f3803ff, 'VDCTRL0'],
      [0x040, 0xffffffff, 0xffffffff, 'VDCTRL1'],
      [0x050, 0xffffffff, 0xffffffff, 'VDCTRL2'],
      [0x060, 0xffffffff, 0x01fff1ff, 'VDCTRL3'],
      [0x070, 0xffffffff, 0xffffffff, 'DVICTRL0'],
      [0x080, 0xffffffff, 0x3fffffff, 'DVICTRL1'],
      [0x090, 0xffffffff, 0x3fffffff, 'DVICTRL2'],
      [0x0a0, 0xffffffff, 0x03ff03ff, 'DVICTRL3'],
    ];

    for (const [offset, value, expected, name] of cases) {
      await machine.writel(LCDIF_BASE + offset, value);
      assert.equal(
        await machine.readl(LCDIF_BASE + offset),
        expected,
        `LCDIF ${name} must decode at its PDF address and preserve only documented fields`,
      );
    }

    await machine.writel(LCDIF_BASE + 0x034, 0xffffffff);
    assert.equal(
      await machine.readl(LCDIF_BASE + 0x030),
      0x3f3803ff,
      'LCDIF VDCTRL0 SET alias must operate only on documented fields',
    );
  });
}

async function testLcdifClockGateContract() {
  await withMachine(async (machine) => {
    await machine.writel(LCDIF_BASE + 0x008, 0xc0000000);
    await machine.writel(LCDIF_BASE + 0x004, 0x40000000);
    assert.notEqual(
      (await machine.readl(LCDIF_BASE + 0x000)) & 0x40000000,
      0,
      'LCDIF CLKGATE must remain set when SFTRST is clear',
    );

    await machine.writel(LCDIF_BASE + 0x004, 0x80000000);
    await machine.writel(LCDIF_BASE + 0x008, 0x80000000);
    assert.notEqual(
      (await machine.readl(LCDIF_BASE + 0x000)) & 0x40000000,
      0,
      'LCDIF clearing SFTRST must not implicitly clear a separately gated clock',
    );
  });
}

async function testLcdifSoftResetContract() {
  await withMachine(async (machine) => {
    await machine.writel(LCDIF_BASE + 0x008, 0xc0000000);
    await machine.writel(LCDIF_BASE + 0x010, 0x00030001);
    await machine.writel(LCDIF_BASE + 0x020, 0x11223344);
    await machine.writel(LCDIF_BASE + 0x040, 0x55667788);

    await machine.writel(LCDIF_BASE + 0x004, 0x80000000);
    assert.equal(
      await machine.readl(LCDIF_BASE + 0x010),
      0x000f0001,
      'LCDIF SFTRST must restore CTRL1 defaults while preserving the external RESET line',
    );
    assert.equal(
      await machine.readl(LCDIF_BASE + 0x020),
      0,
      'LCDIF SFTRST must restore TIMING defaults',
    );
    assert.equal(
      await machine.readl(LCDIF_BASE + 0x040),
      0,
      'LCDIF SFTRST must restore VDCTRL1 defaults',
    );
  });
}

async function testLcdifBytePackingContract() {
  await withMachine(async (machine) => {
    await machine.writel(LCDIF_BASE + 0x008, 0xc0000000);
    await machine.writel(LCDIF_BASE + 0x000, 0x00020000);
    await machine.writeb(LCDIF_BASE + 0x0b0, 0x2c);
    await machine.writel(LCDIF_BASE + 0x010, 0x00070000);
    await machine.writel(LCDIF_BASE + 0x000, 0x00070003);
    await machine.writel(LCDIF_BASE + 0x0b0, 0xaabbccdd);

    assert.equal(
      (await machine.readl(LCDIF_BASE + 0x000)) & 0x00010000,
      0,
      'LCDIF must consume only the three valid BYTE_PACKING_FORMAT subwords',
    );

    await machine.writel(LCDIF_BASE + 0x000, 0x20020000);
    await machine.writeb(LCDIF_BASE + 0x0b0, 0x2e);
    assert.equal(await machine.readb(LCDIF_BASE + 0x0b0), 0xdd);
    assert.equal(await machine.readb(LCDIF_BASE + 0x0b0), 0xcc);
    assert.equal(await machine.readb(LCDIF_BASE + 0x0b0), 0xbb);
    assert.equal(
      await machine.readb(LCDIF_BASE + 0x0b0),
      0,
      'LCDIF must not transmit the byte masked by BYTE_PACKING_FORMAT',
    );
  });
}

async function testLcdifDataSwizzleContract() {
  await withMachine(async (machine) => {
    await machine.writel(LCDIF_BASE + 0x008, 0xc0000000);
    await machine.writel(LCDIF_BASE + 0x000, 0x00020000);
    await machine.writeb(LCDIF_BASE + 0x0b0, 0x2c);
    await machine.writel(LCDIF_BASE + 0x000, 0x00270004);
    await machine.writel(LCDIF_BASE + 0x0b0, 0x11223344);

    await machine.writel(LCDIF_BASE + 0x000, 0x20020000);
    await machine.writeb(LCDIF_BASE + 0x0b0, 0x2e);
    assert.equal(await machine.readb(LCDIF_BASE + 0x0b0), 0x11);
    assert.equal(await machine.readb(LCDIF_BASE + 0x0b0), 0x22);
    assert.equal(await machine.readb(LCDIF_BASE + 0x0b0), 0x33);
    assert.equal(await machine.readb(LCDIF_BASE + 0x0b0), 0x44);
  });
}

async function testLcdifDataShiftContract() {
  await withMachine(async (machine) => {
    await machine.writel(LCDIF_BASE + 0x008, 0xc0000000);
    await machine.writel(LCDIF_BASE + 0x000, 0x00020000);
    await machine.writeb(LCDIF_BASE + 0x0b0, 0x2c);
    await machine.writel(LCDIF_BASE + 0x000, 0x0c070004);
    await machine.writel(LCDIF_BASE + 0x0b0, 0xaabbccdd);

    await machine.writel(LCDIF_BASE + 0x000, 0x20020000);
    await machine.writeb(LCDIF_BASE + 0x0b0, 0x2e);
    assert.equal(await machine.readb(LCDIF_BASE + 0x0b0), 0x37);
    assert.equal(await machine.readb(LCDIF_BASE + 0x0b0), 0x33);
    assert.equal(await machine.readb(LCDIF_BASE + 0x0b0), 0x2e);
    assert.equal(await machine.readb(LCDIF_BASE + 0x0b0), 0x2a);
  });
}

async function testLcdifIdleOnlyControlContract() {
  await withMachine(async (machine) => {
    await machine.writel(LCDIF_BASE + 0x008, 0xc0000000);
    await machine.writel(LCDIF_BASE + 0x000, 0x00010001);
    await machine.writel(LCDIF_BASE + 0x004, 0x00040000);
    await machine.writel(LCDIF_BASE + 0x014, 0x00000002);
    assert.equal((await machine.readl(LCDIF_BASE + 0x000)) & 0x00040000, 0);
    assert.equal((await machine.readl(LCDIF_BASE + 0x010)) & 0x00000002, 0);

    await machine.writel(LCDIF_BASE + 0x008, 0x00010000);
    await machine.writel(LCDIF_BASE + 0x004, 0x00040000);
    await machine.writel(LCDIF_BASE + 0x014, 0x00000002);
    assert.notEqual((await machine.readl(LCDIF_BASE + 0x000)) & 0x00040000, 0);
    assert.notEqual((await machine.readl(LCDIF_BASE + 0x010)) & 0x00000002, 0);
  });
}

async function testLcdifFifoStatusContract() {
  await withMachine(async (machine) => {
    await machine.writel(LCDIF_BASE + 0x008, 0xc0000000);
    await machine.writel(LCDIF_BASE + 0x000, 0x00030004);
    assert.equal(
      await machine.readl(LCDIF_BASE + 0x0c0),
      0xd4000000,
      'LCDIF STAT must report an empty TX FIFO and asserted DMA request during an enabled write transfer',
    );
  });
}

async function testLcdifStreamingEndContract() {
  await withMachine(async (machine) => {
    await machine.writel(LCDIF_BASE + 0x008, 0xc0000000);
    await machine.writel(LCDIF_BASE + 0x000, 0x00910000);
    await machine.writel(LCDIF_BASE + 0x008, 0x00100000);
    assert.equal(
      (await machine.readl(LCDIF_BASE + 0x000)) & 0x00010000,
      0,
      'LCDIF ending a bypassed VSYNC stream must clear RUN after its empty FIFO is flushed',
    );
  });
}

async function testLcdifFirstReadDummyContract() {
  await withMachine(async (machine) => {
    await machine.writel(LCDIF_BASE + 0x008, 0xc0000000);
    await machine.writel(LCDIF_BASE + 0x000, 0x00020000);
    await machine.writeb(LCDIF_BASE + 0x0b0, 0x2c);
    await machine.writel(LCDIF_BASE + 0x000, 0x00070004);
    await machine.writel(LCDIF_BASE + 0x0b0, 0x44332211);

    await machine.writel(LCDIF_BASE + 0x000, 0x00020000);
    await machine.writeb(LCDIF_BASE + 0x0b0, 0x2e);
    await machine.writel(LCDIF_BASE + 0x010, 0x000f0010);
    await machine.writel(LCDIF_BASE + 0x000, 0x20030003);
    assert.equal(
      await machine.readb(LCDIF_BASE + 0x0b0),
      0x22,
      'LCDIF FIRST_READ_DUMMY must discard the initial panel response before filling the read FIFO',
    );
  });
}

async function testUsbPhyRegisterContract() {
  await withMachine(async (machine) => {
    const resetValues = [
      [0x000, 0x001f7c00, 'PWD'],
      [0x010, 0x10060607, 'TX'],
      [0x020, 0x00000000, 'RX'],
      [0x030, 0xc0000001, 'CTRL'],
      [0x040, 0x00000000, 'STATUS'],
      [0x050, 0x7f180000, 'DEBUG'],
      [0x060, 0x0000900d, 'DEBUG0_STATUS'],
      [0x070, 0x00001000, 'DEBUG1'],
      [0x080, 0x03000000, 'VERSION'],
    ];

    for (const [offset, expected, name] of resetValues) {
      assert.equal(
        await machine.readl(USBPHY_BASE + offset),
        expected,
        `USBPHY ${name} must decode at its PDF address and reset to its documented value`,
      );
    }

    const writable = [
      [0x000, 0x001f7c00, 'PWD'],
      [0x010, 0x1faf2f8f, 'TX'],
      [0x020, 0x00400033, 'RX'],
      [0x050, 0x7f1f1f3f, 'DEBUG'],
      [0x070, 0x0000700f, 'DEBUG1'],
    ];

    for (const [offset, expected, name] of writable) {
      await machine.writel(USBPHY_BASE + offset, 0);
      await machine.writel(USBPHY_BASE + offset + 0x004, 0xffffffff);
      assert.equal(
        await machine.readl(USBPHY_BASE + offset),
        expected,
        `USBPHY ${name}_SET must preserve only documented writable fields`,
      );
      await machine.writel(USBPHY_BASE + offset + 0x008, 0xffffffff);
      assert.equal(
        await machine.readl(USBPHY_BASE + offset),
        0,
        `USBPHY ${name}_CLR must clear documented writable fields`,
      );
    }

    await machine.writel(USBPHY_BASE + 0x040, 0xffffffff);
    assert.equal(
      await machine.readl(USBPHY_BASE + 0x040),
      0x00000100,
      'USBPHY STATUS must expose OTGID_STATUS as its sole writable field',
    );

    await machine.writel(USBPHY_BASE + 0x050, 0);
    await machine.writel(USBPHY_BASE + 0x038, 0x80000000);
    assert.equal(
      ((await machine.readl(USBPHY_BASE + 0x030)) & 0xc0000000) >>> 0,
      0x40000000,
      'USBPHY clearing SFTRST must preserve a separately gated clock',
    );
    await machine.writel(USBPHY_BASE + 0x034, 0x80000000);
    assert.equal(await machine.readl(USBPHY_BASE + 0x000), 0x001f7c00);
    assert.equal(await machine.readl(USBPHY_BASE + 0x010), 0x10060607);
    assert.equal(await machine.readl(USBPHY_BASE + 0x020), 0);
    assert.equal(
      ((await machine.readl(USBPHY_BASE + 0x030)) & 0xc0000001) >>> 0,
      0xc0000001,
      'USBPHY SFTRST must restore CTRL together with PWD/TX/RX',
    );
    assert.equal(
      await machine.readl(USBPHY_BASE + 0x050),
      0,
      'USBPHY SFTRST must not reset DEBUG outside its documented reset domain',
    );
  });
}

async function testUsbCapabilityRegisterContract() {
  await withMachine(async (machine) => {
    const fixedRegisters = [
      [0x000, 0x0042fa05, 'ID'],
      [0x004, 0x00000015, 'ARC_GENERAL'],
      [0x008, 0x10020001, 'HWHOST'],
      [0x00c, 0x0000000b, 'HWDEVICE'],
      [0x010, 0x00050810, 'HWTXBUF'],
      [0x014, 0x00000610, 'HWRXBUF'],
      [0x104, 0x00010011, 'HCSPARAMS'],
      [0x108, 0x00000006, 'HCCPARAMS'],
      [0x124, 0x00000185, 'DCCPARAMS'],
    ];

    for (const [offset, expected, name] of fixedRegisters) {
      assert.equal(
        await machine.readl(USB_BASE + offset),
        expected,
        `USBCTRL ${name} must decode to its PDF reset value`,
      );
    }

    assert.equal(
      await machine.readb(USB_BASE + 0x100),
      0x40,
      'USBCTRL CAPLENGTH must accept its documented 8-bit access',
    );
    assert.equal(
      await machine.readw(USB_BASE + 0x102),
      0x0100,
      'USBCTRL HCIVERSION must accept its documented 16-bit access',
    );
    assert.equal(
      await machine.readw(USB_BASE + 0x120),
      0x0001,
      'USBCTRL DCIVERSION must accept its documented 16-bit access',
    );
  });
}

async function testUsbDeviceControlContract() {
  await withMachine(async (machine) => {
    assert.equal(await machine.readl(USB_BASE + 0x140), 0x00080000);
    assert.equal(await machine.readl(USB_BASE + 0x144), 0x00000000);
    assert.equal(await machine.readl(USB_BASE + 0x148), 0x00000000);
    assert.equal(await machine.readl(USB_BASE + 0x160), 0x00001010);
    assert.equal(await machine.readl(USB_BASE + 0x1a4), 0x00000020);
    assert.equal(await machine.readl(USB_BASE + 0x1a8), 0x00000000);
    assert.equal(
      (await machine.readl(USB_BASE + 0x184)) & 0x00001005,
      0,
      'USBCTRL PORTSC1 must not report a powered, enabled, or connected port at reset',
    );

    await machine.writel(USB_BASE + 0x148, 0xffffffff);
    assert.equal(
      await machine.readl(USB_BASE + 0x148),
      0x030d05ff,
      'USBCTRL USBINTR must retain only the Table 278 interrupt-enable fields',
    );

    await machine.writel(USB_BASE + 0x154, 0xffffffff);
    assert.equal(
      await machine.readl(USB_BASE + 0x154),
      0xff000000,
      'USBCTRL DEVICEADDR must retain only USBADR and USBADRA',
    );
    await machine.writel(USB_BASE + 0x158, 0xffffffff);
    assert.equal(
      await machine.readl(USB_BASE + 0x158),
      0xfffff800,
      'USBCTRL ENDPTLISTADDR must preserve its 2 KiB alignment',
    );
    await machine.writel(USB_BASE + 0x15c, 0xffffffff);
    assert.equal(
      await machine.readl(USB_BASE + 0x15c),
      0x7f000000,
      'USBCTRL TTCTRL must retain only TTHA[30:24]',
    );
    await machine.writel(USB_BASE + 0x160, 0xffffffff);
    assert.equal(
      await machine.readl(USB_BASE + 0x160),
      0x0000ffff,
      'USBCTRL BURSTSIZE must retain only TXPBURST and RXPBURST',
    );

    await machine.writel(USB_BASE + 0x1a8, 0xffffffff);
    assert.equal(
      await machine.readl(USB_BASE + 0x1a8),
      0x0000003f,
      'USBCTRL USBMODE must retain its six documented control bits',
    );
    await machine.writel(USB_BASE + 0x1a8, 0x00000000);
    assert.equal(
      await machine.readl(USB_BASE + 0x1a8),
      0x0000003f,
      'USBCTRL USBMODE must ignore subsequent writes until controller reset',
    );

    await machine.writel(USB_BASE + 0x140, 0x00000002);
    assert.equal(
      await machine.readl(USB_BASE + 0x140),
      0x00080000,
      'USBCTRL USBCMD.RST must self-clear into the device-mode reset value',
    );
    assert.equal(await machine.readl(USB_BASE + 0x144), 0x00000000);
    assert.equal(await machine.readl(USB_BASE + 0x148), 0x00000000);
    assert.equal(await machine.readl(USB_BASE + 0x154), 0x00000000);
    assert.equal(await machine.readl(USB_BASE + 0x158), 0x00000000);
    assert.equal(await machine.readl(USB_BASE + 0x15c), 0x00000000);
    assert.equal(await machine.readl(USB_BASE + 0x160), 0x00001010);
    assert.equal(await machine.readl(USB_BASE + 0x1a4), 0x00000020);
    assert.equal(await machine.readl(USB_BASE + 0x1a8), 0x00000000);

    await machine.writel(USB_BASE + 0x1a8, 0x00000002);
    await machine.writel(USB_BASE + 0x1a8, 0x00000003);
    assert.equal(
      await machine.readl(USB_BASE + 0x1a8),
      0x00000002,
      'USBCTRL controller reset must permit exactly one new USBMODE selection',
    );
  });
}

async function testUsbEndpointRegisterContract() {
  await withMachine(async (machine) => {
    assert.equal(
      await machine.readl(USB_BASE + 0x1c0),
      0x00800080,
      'USBCTRL ENDPTCTRL0 must reset as the fixed enabled control endpoint',
    );
    await machine.writel(USB_BASE + 0x1c0, 0xffffffff);
    assert.equal(
      await machine.readl(USB_BASE + 0x1c0),
      0x008d008d,
      'USBCTRL ENDPTCTRL0 must retain fixed enables and only Table 318 writable fields',
    );

    await machine.writel(USB_BASE + 0x1b0, 0xffffffff);
    assert.equal(
      await machine.readl(USB_BASE + 0x1b8),
      0x001f001f,
      'USBCTRL ENDPTPRIME must expose ready bits only for endpoints 0 through 4',
    );
    await machine.writel(USB_BASE + 0x1b4, 0xffffffff);
    assert.equal(
      await machine.readl(USB_BASE + 0x1b8),
      0,
      'USBCTRL ENDPTFLUSH must clear only documented endpoint ready bits',
    );

    await machine.writel(USB_BASE + 0x1c4, 0xffffffff);
    assert.equal(
      await machine.readl(USB_BASE + 0x1c4),
      0x00af00af,
      'USBCTRL ENDPTCTRL1 must preserve only Table 320 writable fields',
    );

    await machine.writel(USB_BASE + 0x17c, 0xffffffff);
    assert.equal(
      await machine.readl(USB_BASE + 0x17c),
      0x001f001f,
      'USBCTRL ENDPTNAKEN must retain only RX/TX endpoint bits 0 through 4',
    );

    await machine.writel(USB_BASE + 0x164, 0xffffffff);
    assert.equal(
      await machine.readl(USB_BASE + 0x164),
      0x003f007f,
      'USBCTRL TXFILLTUNING must hide reserved bits and clear TXSCHHEALTH writes',
    );
    await machine.writel(USB_BASE + 0x1d4, 0xffffffff);
    assert.equal(
      await machine.readl(USB_BASE + 0x1d4),
      0,
      'USBCTRL ENDPTCTRL5 must remain unimplemented because only endpoints 0 through 4 exist',
    );
  });
}

async function testUsbGptimerContract() {
  await withMachine(async (machine) => {
    await machine.writel(USB_BASE + 0x080, 0x00000001);
    assert.equal(
      await machine.readl(USB_BASE + 0x080),
      0x00000001,
      'USBCTRL GPTIMER0LD must retain its documented 24-bit load value',
    );
    await machine.writel(USB_BASE + 0x148, 0x01000000);
    await machine.writel(USB_BASE + 0x084, 0x40000000);
    assert.equal(
      await machine.readl(USB_BASE + 0x084),
      0x00000001,
      'USBCTRL GPTRST must load GPTCNT and self-clear while the timer remains stopped',
    );

    await machine.clockStep(1_000);
    assert.equal(
      await machine.readl(USB_BASE + 0x084),
      0x00000001,
      'USBCTRL GPTRST without GTPRUN must retain GPTCNT while the timer is stopped',
    );
    await machine.writel(USB_BASE + 0x084, 0x80000000);
    assert.equal(
      await machine.readl(USB_BASE + 0x084),
      0x80000001,
      'USBCTRL GTPRUN must start from the reset-loaded GPTCNT without changing it',
    );

    await machine.clockStep(1_000);
    assert.equal(
      await machine.readl(USB_BASE + 0x084),
      0x80000001,
      'USBCTRL GPTIMER0 must retain GPTCNT for the first 1 microsecond interval',
    );
    assert.equal(
      (await machine.readl(USB_BASE + 0x144)) & 0x01000000,
      0,
      'USBCTRL TI0 must remain clear before GPTCNT transitions to zero',
    );

    await machine.clockStep(1_000);
    assert.equal(
      await machine.readl(USB_BASE + 0x084),
      0x00000000,
      'USBCTRL GPTIMER0 must stop at zero when the countdown expires',
    );
    assert.notEqual(
      (await machine.readl(USB_BASE + 0x144)) & 0x01000000,
      0,
      'USBCTRL GPTIMER0 expiry must set USBSTS.TI0',
    );
    assert.notEqual(
      (await machine.readl(ICOLL_BASE + 0x040)) & (1 << 11),
      0,
      'USBCTRL TIE0 and TI0 must assert the USB interrupt on ICOLL source 11',
    );
    await machine.writel(USB_BASE + 0x144, 0x01000000);
    assert.equal(
      (await machine.readl(USB_BASE + 0x144)) & 0x01000000,
      0,
      'USBCTRL USBSTS.TI0 must clear by write-one-to-clear',
    );
    assert.equal(
      (await machine.readl(ICOLL_BASE + 0x040)) & (1 << 11),
      0,
      'USBCTRL USB interrupt must deassert after TI0 is acknowledged',
    );
    await machine.writel(USB_BASE + 0x084, 0x80000000);
    await machine.clockStep(2_000);
    assert.equal(
      await machine.readl(USB_BASE + 0x084),
      0,
      'USBCTRL one-shot must remain stopped when GTPRUN is written without GPTRST',
    );
    assert.equal(
      (await machine.readl(USB_BASE + 0x144)) & 0x01000000,
      0,
      'USBCTRL one-shot must not reassert TI0 until software resets GPTCNT',
    );

    await machine.writel(USB_BASE + 0x080, 0x00000009);
    await machine.writel(USB_BASE + 0x084, 0xc0000000);
    await machine.clockStep(2_000);
    await machine.writel(USB_BASE + 0x080, 0x00000003);
    await machine.writel(USB_BASE + 0x084, 0x40000000);
    assert.equal(
      await machine.readl(USB_BASE + 0x084),
      0x00000003,
      'USBCTRL GPTRST must reload GPTCNT even when it stops an active timer',
    );
    await machine.clockStep(10_000);
    assert.equal(
      await machine.readl(USB_BASE + 0x084),
      0x00000003,
      'USBCTRL GPTRST without GTPRUN must leave a reloaded active timer stopped',
    );

    await machine.writel(USB_BASE + 0x088, 0x00000000);
    await machine.writel(USB_BASE + 0x148, 0x02000000);
    await machine.writel(USB_BASE + 0x08c, 0xc1000000);
    await machine.clockStep(1_000);
    assert.notEqual(
      (await machine.readl(USB_BASE + 0x144)) & 0x02000000,
      0,
      'USBCTRL GPTIMER1 repeat mode must set USBSTS.TI1 on expiry',
    );
    await machine.writel(USB_BASE + 0x144, 0x02000000);
    await machine.clockStep(1_000);
    assert.notEqual(
      (await machine.readl(USB_BASE + 0x144)) & 0x02000000,
      0,
      'USBCTRL GPTIMER1 repeat mode must automatically reload after expiry',
    );
  });
}

async function testSspRegisterLayoutAndResetContract() {
  await withMachine(async (machine) => {
    for (const [name, base] of [['SSP1', SSP1_BASE], ['SSP2', SSP2_BASE]]) {
      assert.equal(
        await machine.readl(base + 0x000),
        0xc0000001,
        `${name} CTRL0 must reset with SFTRST, CLKGATE, and XFER_COUNT=1`,
      );
      assert.equal(
        await machine.readl(base + 0x060),
        0x00200080,
        `${name} CTRL1 must reset with FIFO_UNDERRUN_IRQ and eight-bit word length`,
      );
      assert.equal(
        await machine.readl(base + 0x0c0),
        0xe0000020,
        `${name} STATUS must report present controllers and an empty FIFO`,
      );
      assert.equal(
        await machine.readl(base + 0x100),
        0,
        `${name} DEBUG must be read-only and reset clear at its documented address`,
      );
      assert.equal(
        await machine.readl(base + 0x110),
        0x02000000,
        `${name} VERSION must be 2.0 at its documented address`,
      );
    }

    await machine.writel(SSP1_BASE + 0x008, 0xc0000000);
    await machine.writel(SSP1_BASE + 0x010, 0x00123456);
    await machine.writel(SSP1_BASE + 0x020, 0x89abcdef);
    await machine.writel(SSP1_BASE + 0x030, 0x10203040);
    await machine.writel(SSP1_BASE + 0x040, 0x55667788);
    await machine.writel(SSP1_BASE + 0x050, 0x00001234);

    assert.equal(await machine.readl(SSP1_BASE + 0x010), 0x00123456, 'SSP CMD0 must be mapped at +0x10');
    assert.equal(await machine.readl(SSP1_BASE + 0x020), 0x89abcdef, 'SSP CMD1 must be mapped at +0x20');
    assert.equal(await machine.readl(SSP1_BASE + 0x030), 0x10203040, 'SSP COMPREF must be mapped at +0x30');
    assert.equal(await machine.readl(SSP1_BASE + 0x040), 0x55667788, 'SSP COMPMASK must be mapped at +0x40');
    assert.equal(await machine.readl(SSP1_BASE + 0x050), 0x00001234, 'SSP TIMING must be mapped at +0x50');
  });
}

async function testSspSoftResetAndClockGateContract() {
  await withMachine(async (machine) => {
    await machine.writel(SSP1_BASE + 0x008, 0x80000000);
    assert.equal(
      await machine.readl(SSP1_BASE + 0x000),
      0x40000001,
      'SSP CTRL0_CLR.SFTRST must release reset without clearing CLKGATE',
    );

    await machine.writel(SSP1_BASE + 0x008, 0x40000000);
    assert.equal(
      await machine.readl(SSP1_BASE + 0x000),
      0x00000001,
      'SSP CTRL0_CLR.CLKGATE must independently release the clock gate',
    );

    await machine.writel(SSP1_BASE + 0x000, 0x80000000);
    assert.equal(
      await machine.readl(SSP1_BASE + 0x000),
      0xc0000001,
      'SSP soft reset must restore documented reset state including CLKGATE',
    );
  });
}

async function testSspSoftResetHoldContract() {
  await withMachine(async (machine) => {
    await machine.writel(SSP1_BASE + 0x008, 0xc0000000);
    await machine.writel(SSP1_BASE + 0x060, 0x13579bdf);
    await machine.writel(SSP1_BASE + 0x010, 0x00123456);

    await machine.writel(SSP1_BASE + 0x004, 0x80000000);
    assert.equal(
      await machine.readl(SSP1_BASE + 0x000),
      0xc0000001,
      'SSP SFTRST must hold the module in its documented reset state',
    );

    await machine.writel(SSP1_BASE + 0x060, 0xffffffff);
    await machine.writel(SSP1_BASE + 0x010, 0x001fffff);
    assert.equal(
      await machine.readl(SSP1_BASE + 0x060),
      0x00200080,
      'SSP configuration writes must not escape a held SFTRST',
    );
    assert.equal(
      await machine.readl(SSP1_BASE + 0x010),
      0,
      'SSP CMD0 must remain reset while SFTRST is held',
    );
  });
}

async function testSspCtrl1WritableMaskContract() {
  await withMachine(async (machine) => {
    await machine.writel(SSP1_BASE + 0x008, 0xc0000000);
    await machine.writel(SSP1_BASE + 0x068, 0xffffffff);
    assert.equal(await machine.readl(SSP1_BASE + 0x060), 0, 'SSP CTRL1_CLR must clear all documented writable fields');

    await machine.writel(SSP1_BASE + 0x064, 0xffffffff);
    assert.equal(
      await machine.readl(SSP1_BASE + 0x060),
      0xffffffff,
      'SSP CTRL1 contains only documented writable status, enable, and mode fields',
    );

    await machine.writel(SSP1_BASE + 0x068, 1 << 21);
    assert.equal(
      (await machine.readl(SSP1_BASE + 0x060)) & (1 << 21),
      0,
      'SSP CTRL1_CLR must use write-one-to-clear semantics for FIFO_UNDERRUN_IRQ',
    );
  });
}

async function testSspSctAndCmd0ReservedContract() {
  await withMachine(async (machine) => {
    await machine.writel(SSP1_BASE + 0x008, 0xc0000000);
    await machine.writel(SSP1_BASE + 0x010, 0xffe12345);
    assert.equal(
      await machine.readl(SSP1_BASE + 0x010),
      0x00012345,
      'SSP CMD0 must retain only documented bits 20:0',
    );

    await machine.writel(SSP1_BASE + 0x014, 0x00100000);
    assert.equal(
      await machine.readl(SSP1_BASE + 0x014),
      0,
      'SSP CMD0_SET must read as a write-only SCT alias',
    );
    assert.equal(
      await machine.readl(SSP1_BASE + 0x010),
      0x00112345,
      'SSP CMD0_SET must update documented CMD0 bits',
    );

    await machine.writel(SSP1_BASE + 0x020, 0x11223344);
    await machine.writel(SSP1_BASE + 0x024, 0xaabbccdd);
    assert.equal(
      await machine.readl(SSP1_BASE + 0x024),
      0,
      'SSP CMD1 must not decode an undocumented SCT alias',
    );
    assert.equal(
      await machine.readl(SSP1_BASE + 0x020),
      0x11223344,
      'SSP CMD1 must ignore an undocumented SCT alias write',
    );

    assert.equal(
      await machine.readl(SSP1_BASE + 0x004),
      0,
      'SSP CTRL0_SET must read as a write-only SCT alias',
    );
    assert.equal(
      await machine.readl(SSP1_BASE + 0x064),
      0,
      'SSP CTRL1_SET must read as a write-only SCT alias',
    );
  });
}

async function testSspErrorIrqPairingContract() {
  await withMachine(async (machine) => {
    const errorPairs = [
      [31, 30, 'SDIO'],
      [29, 28, 'response error'],
      [27, 26, 'response timeout'],
      [25, 24, 'data timeout'],
      [23, 22, 'data CRC'],
      [21, 20, 'FIFO underrun'],
      [19, 18, 'CE-ATA CCS error'],
      [17, 16, 'receive timeout'],
      [15, 14, 'FIFO overrun'],
    ];

    await machine.writel(SSP1_BASE + 0x008, 0xc0000000);
    await machine.writel(SSP1_BASE + 0x068, 0xffffffff);
    for (const [statusBit, enableBit, name] of errorPairs) {
      const statusMask = 2 ** statusBit;
      const enableMask = 2 ** enableBit;

      await machine.writel(SSP1_BASE + 0x064, statusMask + enableMask);
      assert.notEqual(
        (await machine.readl(ICOLL_BASE + 0x040)) & (1 << 15),
        0,
        `SSP1 ${name} status and enable must assert ICOLL source 15`,
      );

      await machine.writel(SSP1_BASE + 0x068, statusMask);
      assert.equal(
        (await machine.readl(ICOLL_BASE + 0x040)) & (1 << 15),
        0,
        `SSP1 ${name} status clear must deassert ICOLL source 15`,
      );
      await machine.writel(SSP1_BASE + 0x068, enableMask);
    }
  });
}

async function testSspDataEmptyReadContract() {
  await withMachine(async (machine) => {
    await machine.writel(SSP1_BASE + 0x008, 0xc0000000);
    await machine.writel(SSP1_BASE + 0x068, 1 << 21);

    assert.equal(await machine.readl(SSP1_BASE + 0x070), 0, 'SSP DATA empty read must return zeroed FIFO content');
    assert.equal(
      (await machine.readl(SSP1_BASE + 0x060)) & (1 << 21),
      0,
      'SSP DATA reads must not advance or underflow the FIFO while RUN is clear',
    );
    assert.equal(
      (await machine.readl(SSP1_BASE + 0x0c0)) & (1 << 4),
      0,
      'SSP STATUS.FIFO_UNDRFLW must remain clear while RUN is clear',
    );

    await machine.writel(SSP1_BASE + 0x004, 1 << 29);
    assert.equal(await machine.readl(SSP1_BASE + 0x070), 0, 'SSP DATA empty read must return zeroed FIFO content');
    assert.notEqual(
      (await machine.readl(SSP1_BASE + 0x060)) & (1 << 21),
      0,
      'SSP DATA empty read must raise FIFO_UNDERRUN_IRQ when RUN is set',
    );
    assert.notEqual(
      (await machine.readl(SSP1_BASE + 0x0c0)) & (1 << 4),
      0,
      'SSP DATA empty read must expose STATUS.FIFO_UNDRFLW when RUN is set',
    );
    assert.equal(
      (await machine.readl(SSP1_BASE + 0x000)) & (1 << 29),
      0,
      'SSP RUN must clear after the reset XFER_COUNT of one word completes',
    );
  });
}

async function testGpmiTiming2Contract() {
  await withMachine(async (machine) => {
    await machine.writel(GPMI_BASE + 0x080, 0xffffffff);
    assert.equal(
      await machine.readl(GPMI_BASE + 0x080),
      0xffff0000,
      'GPMI TIMING1 must retain only DEVICE_BUSY_TIMEOUT and read its lower reserved field as zero',
    );

    assert.equal(
      await machine.readl(GPMI_BASE + 0x090),
      0x09020101,
      'GPMI TIMING2 must occupy 0x90 and expose all four documented reset bytes',
    );

    await machine.writel(GPMI_BASE + 0x090, 0x5a3c1708);
    assert.equal(
      await machine.readl(GPMI_BASE + 0x090),
      0x5a3c1708,
      'GPMI TIMING2 must retain all four documented UDMA timing fields',
    );

    for (const offset of [0x094, 0x098, 0x09c]) {
      await machine.writel(GPMI_BASE + offset, 0xffffffff);
      assert.equal(
        await machine.readl(GPMI_BASE + 0x090),
        0x5a3c1708,
        `GPMI TIMING2 must not decode an undocumented alias at 0x${offset.toString(16)}`,
      );
    }
  });
}

async function testGpmiCtrl1Contract() {
  await withMachine(async (machine) => {
    assert.equal(
      await machine.readl(GPMI_BASE + 0x060),
      0x00000004,
      'GPMI CTRL1 must reset with ATA_IRQRDY_POLARITY asserted',
    );

    await machine.writel(GPMI_BASE + 0x064, 0x00004001);
    assert.equal(
      await machine.readl(GPMI_BASE + 0x060),
      0x00004005,
      'GPMI CTRL1_SET must affect only documented control fields',
    );
    await machine.writel(GPMI_BASE + 0x068, 0x00004001);
    assert.equal(
      await machine.readl(GPMI_BASE + 0x060),
      0x00000004,
      'GPMI CTRL1_CLR must clear documented control fields',
    );

    await machine.writel(GPMI_BASE + 0x060, 0xffffffff);
    assert.equal(
      await machine.readl(GPMI_BASE + 0x060),
      0x000079ff,
      'GPMI CTRL1 must ignore reserved bits and not software-set IRQ status',
    );
    await machine.writel(GPMI_BASE + 0x06c, 0x00004001);
    assert.equal(
      await machine.readl(GPMI_BASE + 0x060),
      0x000039fe,
      'GPMI CTRL1_TOG must affect only documented control fields',
    );
  });
}

async function testGpmiEccRegisterContract() {
  await withMachine(async (machine) => {
    await machine.writel(GPMI_BASE + 0x020, 0xffffffff);
    assert.equal(
      await machine.readl(GPMI_BASE + 0x020),
      0xffff71ff,
      'GPMI ECCCTRL must retain only HANDLE, ECC_CMD, ENABLE_ECC, and BUFFER_MASK',
    );
    await machine.writel(GPMI_BASE + 0x028, 0x00005001);
    assert.equal(
      await machine.readl(GPMI_BASE + 0x020),
      0xffff21fe,
      'GPMI ECCCTRL_CLR must clear documented fields',
    );
    await machine.writel(GPMI_BASE + 0x02c, 0x00002002);
    assert.equal(
      await machine.readl(GPMI_BASE + 0x020),
      0xffff01fc,
      'GPMI ECCCTRL_TOG must toggle documented fields',
    );

    await machine.writel(GPMI_BASE + 0x010, 0x12345678);
    for (const offset of [0x014, 0x018, 0x01c]) {
      await machine.writel(GPMI_BASE + offset, 0xffffffff);
      assert.equal(
        await machine.readl(GPMI_BASE + 0x010),
        0x12345678,
        `GPMI COMPARE must not decode an undocumented alias at 0x${offset.toString(16)}`,
      );
    }

    await machine.writel(GPMI_BASE + 0x030, 0xffff1234);
    for (const offset of [0x034, 0x038, 0x03c]) {
      await machine.writel(GPMI_BASE + offset, 0xffffffff);
      assert.equal(
        await machine.readl(GPMI_BASE + 0x030),
        0x00001234,
        `GPMI ECCCOUNT must retain only its documented count and reject alias 0x${offset.toString(16)}`,
      );
    }

    await machine.writel(GPMI_BASE + 0x040, 0x12345679);
    for (const offset of [0x044, 0x048, 0x04c]) {
      await machine.writel(GPMI_BASE + offset, 0xffffffff);
      assert.equal(
        await machine.readl(GPMI_BASE + 0x040),
        0x12345678,
        `GPMI PAYLOAD must remain word-aligned and reject alias 0x${offset.toString(16)}`,
      );
    }

    await machine.writel(GPMI_BASE + 0x050, 0xcafebabf);
    for (const offset of [0x054, 0x058, 0x05c]) {
      await machine.writel(GPMI_BASE + offset, 0xffffffff);
      assert.equal(
        await machine.readl(GPMI_BASE + 0x050),
        0xcafebabc,
        `GPMI AUXILIARY must remain word-aligned and reject alias 0x${offset.toString(16)}`,
      );
    }
  });
}

async function testGpmiCompareSenseContract() {
  await withMachine(async (machine) => {
    const senseDescriptor = SRAM_BASE + 0x3000;
    const successDescriptor = SRAM_BASE + 0x3040;
    const errorDescriptor = SRAM_BASE + 0x3080;
    const compareDescriptor = SRAM_BASE + 0x30c0;
    const apbhChannel4NextCommand = APBH_BASE + 0x210;
    const apbhChannel4CurrentCommand = APBH_BASE + 0x200;
    const apbhChannel4Semaphore = APBH_BASE + 0x240;
    const dmaSense = 3;
    const dmaTerminal = (1 << 6) | (1 << 3);
    const runStatusCompare = async (compare, cs = 0, eightBit = true) => {
      await machine.writel(GPMI_BASE + 0x000, 1 << 23);
      await machine.writeb(GPMI_BASE + 0x0a0, 0x70);
      await machine.writel(GPMI_BASE + 0x000, (1 << 29) | (1 << 23) | (cs << 20) | (1 << 17) | 1);
      await machine.writel(GPMI_BASE + 0x010, compare);
      await machine.writel(GPMI_BASE + 0x000, (1 << 29) | (2 << 24) | (eightBit ? (1 << 23) : 0) | (cs << 20) | 1);
    };
    const writeDescriptor = async (address, next, command, bar) => {
      await machine.writel(address + 0x00, next);
      await machine.writel(address + 0x04, command);
      await machine.writel(address + 0x08, bar);
    };
    const runSenseDescriptor = async () => {
      await machine.writel(apbhChannel4NextCommand, senseDescriptor);
      await machine.writel(apbhChannel4Semaphore, 1);
      return await machine.readl(apbhChannel4CurrentCommand);
    };
    const runStatusCompareOnChannel4 = async (compare, cs) => {
      await machine.writel(GPMI_BASE + 0x000, 1 << 23);
      await machine.writeb(GPMI_BASE + 0x0a0, 0x70);
      await machine.writel(GPMI_BASE + 0x000, (1 << 29) | (1 << 23) | (cs << 20) | (1 << 17) | 1);
      await writeDescriptor(
        compareDescriptor,
        0,
        2 << 12,
        0,
      );
      await machine.writel(compareDescriptor + 0x0c, (2 << 24) | (1 << 23) | (cs << 20) | 1);
      await machine.writel(compareDescriptor + 0x10, compare);
      await machine.writel(apbhChannel4NextCommand, compareDescriptor);
      await machine.writel(apbhChannel4Semaphore, 1);
    };

    await machine.writel(GPMI_BASE + 0x000, 0);
    await machine.writel(APBH_BASE + 0x008, 0xc0000000);
    await writeDescriptor(senseDescriptor, successDescriptor, dmaSense, errorDescriptor);
    await writeDescriptor(successDescriptor, 0, dmaTerminal, 0);
    await writeDescriptor(errorDescriptor, 0, dmaTerminal, 0);

    await runStatusCompare(0x00ff00e0);
    assert.equal((await machine.readl(GPMI_BASE + 0x0b0)) & 1, 0, 'GPMI matching compare must keep DEV0_ERROR clear');
    assert.equal((await machine.readl(GPMI_BASE + 0x0c0)) & (1 << 20), 0, 'GPMI matching compare must clear SENSE0');
    assert.equal(
      await runSenseDescriptor(),
      successDescriptor,
      'APBH DMA_SENSE must follow NXTCMDAR when the GPMI sense line is false',
    );

    await runStatusCompare(0x00ff0040);
    assert.notEqual((await machine.readl(GPMI_BASE + 0x0b0)) & 1, 0, 'GPMI compare mismatch must set DEV0_ERROR');
    assert.notEqual((await machine.readl(GPMI_BASE + 0x0c0)) & (1 << 20), 0, 'GPMI compare mismatch must set SENSE0');
    assert.equal(
      await runSenseDescriptor(),
      errorDescriptor,
      'APBH DMA_SENSE must follow BAR when the GPMI sense line is true',
    );

    await runStatusCompare(0xff0000e0, 0, false);
    assert.notEqual(
      (await machine.readl(GPMI_BASE + 0x0b0)) & 1,
      0,
      'GPMI compare must apply the upper 8 bits of its 16-bit mask',
    );

    await runStatusCompareOnChannel4(0x00ff0040, 1);
    assert.notEqual(
      (await machine.readl(GPMI_BASE + 0x0b0)) & (1 << 1),
      0,
      'GPMI compare must report DEV1_ERROR for a failure on chip select 1',
    );
    assert.notEqual(
      (await machine.readl(GPMI_BASE + 0x0c0)) & (1 << 20),
      0,
      'APBH channel 4 must sample GPMI SENSE0 even when the command uses chip select 1',
    );
    assert.equal(
      (await machine.readl(GPMI_BASE + 0x0c0)) & (1 << 21),
      0,
      'chip select must not select the GPMI SENSE line',
    );
    assert.equal(
      await runSenseDescriptor(),
      errorDescriptor,
      'APBH channel 4 must branch on its own GPMI SENSE0 result',
    );
  });
}

async function testGpmiWaitForReadyContract() {
  await withMachine(async (machine) => {
    const apbhChannel4CurrentCommand = APBH_BASE + 0x200;
    const apbhChannel4NextCommand = APBH_BASE + 0x210;
    const apbhChannel4Semaphore = APBH_BASE + 0x240;
    const waitDescriptor = SRAM_BASE + 0x3100;
    const doneDescriptor = SRAM_BASE + 0x3140;
    const timeoutSenseDescriptor = SRAM_BASE + 0x3180;
    const timeoutSuccessDescriptor = SRAM_BASE + 0x31c0;
    const timeoutErrorDescriptor = SRAM_BASE + 0x3200;
    const noDmaXfer = 0;
    const dmaSense = 3;
    const commandWaitForReady = 3 << 24;
    const wordLength8Bit = 1 << 23;
    const runBit = 1 << 29;
    const chainBit = 1 << 2;
    const irqOnCompleteBit = 1 << 3;
    const nandWaitForReadyBit = 1 << 5;
    const semaphoreBit = 1 << 6;
    const wait4EndCmdBit = 1 << 7;
    const onePioWord = 1 << 12;
    const dmaTerminal = semaphoreBit | irqOnCompleteBit;

    const writeDescriptor = async (address, next, command, bar, ctrl0 = undefined) => {
      await machine.writel(address + 0x00, next);
      await machine.writel(address + 0x04, command);
      await machine.writel(address + 0x08, bar);
      if (ctrl0 !== undefined) {
        await machine.writel(address + 0x0c, ctrl0);
      }
    };

    const waitCtrl0 = runBit | commandWaitForReady | wordLength8Bit;

    await machine.writel(CLKCTRL_BASE + 0x080, 0x00000001);
    await machine.writel(GPMI_BASE + 0x000, 0);
    await machine.writel(APBH_BASE + 0x008, 0xc0000000);
    await machine.setIrqIn('/machine/soc/gpmi', 'rdy-busy', 0, 0);

    assert.equal(
      await machine.readl(GPMI_BASE + 0x0c0),
      0,
      'GPMI DEBUG reset value must clear READY, WAIT_FOR_READY_END, SENSE, and CMD_END views',
    );

    await machine.writel(GPMI_BASE + 0x000, waitCtrl0);
    assert.equal(
      (await machine.readl(GPMI_BASE + 0x0c0)) & ((1 << 24) | (1 << 12)),
      0,
      'PIO WAIT_FOR_READY must not complete immediately before the ready input changes',
    );
    await machine.setIrqIn('/machine/soc/gpmi', 'rdy-busy', 0, 1);
    assert.notEqual(
      (await machine.readl(GPMI_BASE + 0x0c0)) & (1 << 28),
      0,
      'GPMI READY0 view must track the normalized ready input state',
    );
    assert.notEqual(
      (await machine.readl(GPMI_BASE + 0x0c0)) & ((1 << 24) | (1 << 12)),
      0,
      'PIO WAIT_FOR_READY must toggle WAIT_FOR_READY_END0 and CMD_END0 when ready arrives',
    );
    await machine.setIrqIn('/machine/soc/gpmi', 'rdy-busy', 0, 0);

    await writeDescriptor(
      waitDescriptor,
      doneDescriptor,
      onePioWord | wait4EndCmdBit | nandWaitForReadyBit | chainBit | noDmaXfer,
      0,
      commandWaitForReady | wordLength8Bit,
    );
    await writeDescriptor(doneDescriptor, 0, dmaTerminal, 0);

    await machine.writel(apbhChannel4NextCommand, waitDescriptor);
    await machine.writel(apbhChannel4Semaphore, 1);
    assert.equal(
      await machine.readl(apbhChannel4CurrentCommand),
      waitDescriptor,
      'APBH WAIT4ENDCMD + NANDWAIT4READY must hold the current descriptor until ready/endcmd occur',
    );
    assert.equal(
      await machine.readl(apbhChannel4Semaphore),
      0x00010000,
      'APBH semaphore must remain non-zero while WAIT_FOR_READY is still pending',
    );
    await machine.setIrqIn('/machine/soc/gpmi', 'rdy-busy', 0, 1);
    assert.equal(
      await machine.readl(apbhChannel4CurrentCommand),
      doneDescriptor,
      'APBH WAIT_FOR_READY chain must resume at the next descriptor after ready/endcmd occur',
    );
    assert.equal(
      await machine.readl(apbhChannel4Semaphore),
      0,
      'APBH completion path must consume the terminal semaphore after WAIT_FOR_READY finishes',
    );
    await machine.setIrqIn('/machine/soc/gpmi', 'rdy-busy', 0, 0);

    await machine.writel(GPMI_BASE + 0x080, 0x00010000);
    await machine.writel(GPMI_BASE + 0x004, 1 << 27);
    await machine.writel(GPMI_BASE + 0x000, waitCtrl0);
    await machine.clockStep(200_000);
    assert.notEqual(
      (await machine.readl(GPMI_BASE + 0x060)) & (1 << 9),
      0,
      'GPMI CTRL1.TIMEOUT_IRQ must latch when WAIT_FOR_READY times out',
    );
    assert.notEqual(
      (await machine.readl(GPMI_BASE + 0x0b0)) & (1 << 8),
      0,
      'GPMI STAT.RDY_TIMEOUT0 must latch when channel 0 WAIT_FOR_READY times out',
    );
    assert.notEqual(
      (await machine.readl(GPMI_BASE + 0x0b0)) & 1,
      0,
      'GPMI WAIT_FOR_READY timeout must report DEV0_ERROR',
    );
    assert.notEqual(
      (await machine.readl(GPMI_BASE + 0x0c0)) & (1 << 20),
      0,
      'GPMI WAIT_FOR_READY timeout must set SENSE0',
    );

    await writeDescriptor(timeoutSenseDescriptor, timeoutSuccessDescriptor, dmaSense, timeoutErrorDescriptor);
    await writeDescriptor(timeoutSuccessDescriptor, 0, dmaTerminal, 0);
    await writeDescriptor(timeoutErrorDescriptor, 0, dmaTerminal, 0);
    await machine.writel(apbhChannel4NextCommand, timeoutSenseDescriptor);
    await machine.writel(apbhChannel4Semaphore, 1);
    assert.equal(
      await machine.readl(apbhChannel4CurrentCommand),
      timeoutErrorDescriptor,
      'APBH DMA_SENSE must branch to BAR after WAIT_FOR_READY timeout sets the GPMI sense flop',
    );
  });
}

async function testEcc8CompletionResultContract() {
  await withMachine(async (machine) => {
    const payload = SRAM_BASE + 0x1000;
    const auxiliary = SRAM_BASE + 0x2000;

    await machine.writel(BCH_BASE + 0x000, 0);
    await machine.writel(GPMI_BASE + 0x000, 0);

    await machine.writel(payload, 0x11111111);
    await machine.writel(payload + 0x200, 0x22222222);
    await machine.writel(auxiliary, 0x33333333);
    await machine.writel(GPMI_BASE + 0x040, payload);
    await machine.writel(GPMI_BASE + 0x050, auxiliary);
    await machine.writel(GPMI_BASE + 0x020, 0x00001002);
    await machine.writel(GPMI_BASE + 0x000, (1 << 29) | (1 << 24) | 1);
    assert.equal(
      await machine.readl(payload),
      0x11111111,
      'ECC8 must not write unselected payload buffer 0',
    );
    assert.equal(
      await machine.readl(payload + 0x200),
      0xffffffff,
      'ECC8 must transfer the selected payload buffer 1',
    );
    assert.equal(
      await machine.readl(auxiliary),
      0x33333333,
      'ECC8 must not write the auxiliary buffer unless BUFFER_MASK.AUXILIARY is set',
    );
    await machine.writel(BCH_BASE + 0x008, 1);

    await machine.writel(GPMI_BASE + 0x020, 0x1234110f);
    await machine.writel(GPMI_BASE + 0x000, (1 << 29) | (1 << 24) | (2 << 20) | 1);
    const firstStatus0 = await machine.readl(BCH_BASE + 0x010);
    assert.equal(firstStatus0 >>> 16, 0x1234, 'ECC8 STATUS0 must retain the GPMI ECC handle');
    assert.equal(firstStatus0 & 0x3, 2, 'ECC8 STATUS0 must report the completing chip select');
    assert.equal((firstStatus0 >>> 8) & 0xf, 0, 'ECC8 STATUS0 must report a checked auxiliary block');
    assert.equal(
      await machine.readl(BCH_BASE + 0x020),
      0xcccc0000,
      'ECC8 STATUS1 must mark unrequested payload blocks as not checked',
    );

    await machine.writel(GPMI_BASE + 0x020, 0xbeef110f);
    await machine.writel(GPMI_BASE + 0x000, (1 << 29) | (1 << 24) | (1 << 20) | 1);
    assert.equal(
      await machine.readl(BCH_BASE + 0x010),
      firstStatus0,
      'ECC8 must retain unread completion results until COMPLETE_IRQ is cleared',
    );

    await machine.writel(BCH_BASE + 0x008, 1);
    await machine.writel(GPMI_BASE + 0x000, (1 << 29) | (1 << 24) | (1 << 20) | 1);
    const secondStatus0 = await machine.readl(BCH_BASE + 0x010);
    assert.equal(secondStatus0 >>> 16, 0xbeef, 'ECC8 must accept a new result after COMPLETE_IRQ is cleared');
    assert.equal(secondStatus0 & 0x3, 1, 'ECC8 must report the new completing chip select');
  });
}

async function testEcc8RegisterContract() {
  await withMachine(async (machine) => {
    assert.equal(
      await machine.readl(BCH_BASE + 0x000),
      0xe0000000,
      'ECC8 CTRL must reset with SFTRST, CLKGATE, and AHBM_SFTRST asserted',
    );
    assert.equal(
      await machine.readl(BCH_BASE + 0x010),
      0x0000fc10,
      'ECC8 STATUS0 must reset with all four capabilities, NOT_CHECKED auxiliary status, and ALLONES',
    );
    assert.equal(
      await machine.readl(BCH_BASE + 0x020),
      0xcccccccc,
      'ECC8 STATUS1 must reset with every payload marked NOT_CHECKED',
    );
    assert.equal(
      await machine.readl(BCH_BASE + 0x080),
      0x38434345,
      'ECC8 BLOCKNAME must expose the fixed ASCII ECC8 identifier',
    );
    assert.equal(
      await machine.readl(BCH_BASE + 0x0a0),
      0x01000000,
      'ECC8 VERSION must expose the documented v1.0 value',
    );
    for (const offset of [0x040, 0x050, 0x060, 0x070]) {
      assert.equal(
        await machine.readl(BCH_BASE + offset),
        0,
        `ECC8 debug read register 0x${offset.toString(16)} must reset to zero`,
      );
    }

    await machine.writel(GPMI_BASE + 0x000, 0);
    await machine.writel(GPMI_BASE + 0x020, 0x0000110f);
    await machine.writel(GPMI_BASE + 0x000, (1 << 29) | (1 << 24) | 1);
    assert.equal((await machine.readl(BCH_BASE + 0x000)) & 1, 0, 'ECC8 must not complete while SFTRST is asserted');
    await machine.writel(BCH_BASE + 0x008, 0x80000000);
    await machine.writel(GPMI_BASE + 0x000, (1 << 29) | (1 << 24) | 1);
    assert.equal((await machine.readl(BCH_BASE + 0x000)) & 1, 0, 'ECC8 must not complete while CLKGATE or AHBM_SFTRST is asserted');
    await machine.writel(BCH_BASE + 0x008, (1 << 30) | (1 << 29));
    await machine.writel(GPMI_BASE + 0x000, (1 << 29) | (1 << 24) | 1);
    assert.notEqual((await machine.readl(BCH_BASE + 0x000)) & 1, 0, 'ECC8 must complete after SFTRST, CLKGATE, and AHBM_SFTRST are all clear');
    await machine.writel(BCH_BASE + 0x008, 1);

    await machine.writel(BCH_BASE + 0x000, 0xffffffff);
    assert.equal(
      await machine.readl(BCH_BASE + 0x000),
      0xef000700,
      'ECC8 CTRL base write must retain documented configuration fields but not manufacture IRQ status',
    );
    await machine.writel(BCH_BASE + 0x008, 0x80000000);
    assert.equal(
      (await machine.readl(BCH_BASE + 0x000)) & (1 << 30),
      1 << 30,
      'ECC8 CLKGATE must remain asserted until explicitly cleared after SFTRST release',
    );
    await machine.writel(BCH_BASE + 0x008, 0xe0000000);

    await machine.writel(GPMI_BASE + 0x000, (1 << 29) | (1 << 24) | 1);
    const completedStatus = await machine.readl(BCH_BASE + 0x010);
    assert.notEqual(completedStatus, 0x00001c01, 'ECC8 completion must update STATUS0');
    assert.notEqual((await machine.readl(BCH_BASE + 0x000)) & 1, 0, 'ECC8 completion must set COMPLETE_IRQ');
    await machine.writel(BCH_BASE + 0x010, 0xffffffff);
    assert.equal(await machine.readl(BCH_BASE + 0x010), completedStatus, 'ECC8 STATUS0 must be read-only');
    await machine.writel(BCH_BASE + 0x014, 0xffffffff);
    assert.equal(await machine.readl(BCH_BASE + 0x010), completedStatus, 'ECC8 STATUS0 must reject undocumented aliases');
    await machine.writel(BCH_BASE + 0x000, 1);
    assert.notEqual((await machine.readl(BCH_BASE + 0x000)) & 1, 0, 'ECC8 base CTRL write must not clear COMPLETE_IRQ');
    await machine.writel(BCH_BASE + 0x008, 1);
    assert.equal((await machine.readl(BCH_BASE + 0x000)) & 1, 0, 'ECC8 CTRL_CLR must clear COMPLETE_IRQ');

    await machine.writel(BCH_BASE + 0x030, 0x01ffff3f);
    await machine.writel(BCH_BASE + 0x004, 0x80000000);
    assert.equal(
      await machine.readl(BCH_BASE + 0x010),
      0x0000fc10,
      'ECC8 SFTRST must restore STATUS0 defaults',
    );
    assert.equal(
      await machine.readl(BCH_BASE + 0x020),
      0xcccccccc,
      'ECC8 SFTRST must restore STATUS1 defaults',
    );
    assert.equal(await machine.readl(BCH_BASE + 0x030), 0, 'ECC8 SFTRST must reset DEBUG0');

    for (const offset of [0x080, 0x084, 0x088, 0x08c, 0x0a0, 0x0a4, 0x0a8, 0x0ac]) {
      await machine.writel(BCH_BASE + offset, 0xffffffff);
    }
    assert.equal(await machine.readl(BCH_BASE + 0x080), 0x38434345, 'ECC8 BLOCKNAME must remain read-only');
    assert.equal(await machine.readl(BCH_BASE + 0x0a0), 0x01000000, 'ECC8 VERSION must remain read-only');
  });
}

async function testGpmiDataFifoContract() {
  await withMachine(async (machine) => {
    await machine.writel(GPMI_BASE + 0x000, 0);
    assert.equal(
      (await machine.readl(GPMI_BASE + 0x0b0)) & 0x30,
      0x20,
      'GPMI STAT must report an empty, non-full FIFO after reset is released',
    );

    await machine.writew(GPMI_BASE + 0x0a0, 0x3412);
    assert.equal(
      (await machine.readl(GPMI_BASE + 0x0b0)) & 0x30,
      0,
      'GPMI STAT must clear FIFO_EMPTY after a 16-bit DATA write',
    );
    assert.equal(
      await machine.readw(GPMI_BASE + 0x0a0),
      0x3412,
      'GPMI DATA must preserve a 16-bit transfer in 16-bit mode',
    );
    assert.equal(
      (await machine.readl(GPMI_BASE + 0x0b0)) & 0x30,
      0x20,
      'GPMI STAT must restore FIFO_EMPTY after the last DATA byte is read',
    );

    await machine.writel(GPMI_BASE + 0x004, 1 << 23);
    await machine.writeb(GPMI_BASE + 0x0a0, 0x5a);
    await machine.writew(GPMI_BASE + 0x0a0, 0x3412);
    await machine.writel(GPMI_BASE + 0x0a0, 0x88776655);
    assert.equal(await machine.readb(GPMI_BASE + 0x0a0), 0x5a);
    assert.equal(await machine.readw(GPMI_BASE + 0x0a0), 0x3412);
    assert.equal(await machine.readl(GPMI_BASE + 0x0a0), 0x88776655);

    await machine.writeb(GPMI_BASE + 0x0a0, 0xa1);
    await machine.writeb(GPMI_BASE + 0x0a0, 0xb2);
    await machine.writeb(GPMI_BASE + 0x0a0, 0xc3);
    await machine.writel(GPMI_BASE + 0x000, (1 << 29) | (1 << 23) | 3);
    assert.equal(
      (await machine.readl(GPMI_BASE + 0x0b0)) & 0x30,
      0x20,
      'GPMI WRITE command must consume every queued DATA byte through XFER_COUNT',
    );

    await machine.writel(GPMI_BASE + 0x0a4, 0xdeadbeef);
    assert.equal(
      (await machine.readl(GPMI_BASE + 0x0b0)) & 0x30,
      0x20,
      'GPMI DATA must reject an undocumented SCT alias without altering FIFO state',
    );

    for (let byte = 0; byte < 64; byte += 1) {
      await machine.writeb(GPMI_BASE + 0x0a0, byte);
    }
    assert.equal(
      (await machine.readl(GPMI_BASE + 0x0b0)) & 0x30,
      0x10,
      'GPMI STAT must report a full, non-empty FIFO after 64 queued 8-bit bus cycles',
    );
    await machine.writeb(GPMI_BASE + 0x0a0, 0xff);
    assert.equal(
      await machine.readb(GPMI_BASE + 0x0a0),
      0,
      'GPMI DATA must leave the FIFO unchanged when it is full',
    );

    await machine.writel(GPMI_BASE + 0x000, 0);
    for (let word = 0; word < 32; word += 1) {
      await machine.writew(GPMI_BASE + 0x0a0, word);
    }
    assert.equal(
      (await machine.readl(GPMI_BASE + 0x0b0)) & 0x30,
      0x10,
      'GPMI STAT must report a full FIFO after 32 queued 16-bit bus cycles',
    );

    await machine.writel(GPMI_BASE + 0x000, 0);
    for (let word = 0; word < 32; word += 1) {
      await machine.writew(GPMI_BASE + 0x0a0, word);
    }
    await machine.writel(GPMI_BASE + 0x000, (1 << 29) | (1 << 23));
    assert.equal(
      (await machine.readl(GPMI_BASE + 0x0b0)) & 0x30,
      0x20,
      'GPMI XFER_COUNT=0 must consume the available 64K-word transfer stream rather than zero words',
    );
  });
}

async function testApbxDma64KAndAhbErrorContract() {
  await withMachine(async (machine) => {
    const apbxCh2Nxtcmdar = APBX_BASE + 0x130;
    const apbxCh2Cmd = APBX_BASE + 0x140;
    const apbxCh2Bar = APBX_BASE + 0x150;
    const apbxCh2Sema = APBX_BASE + 0x160;

    /* Release DMA from reset and clock gate. */
    await machine.writel(APBX_BASE + 0x008, 0xc0000000);

    /* 64-KiB transfer: XFER_COUNT=0 with a valid SRAM byte address. */
    const okDescriptor = 0x00000500;
    await machine.writel(okDescriptor + 0x00, 0);
    await machine.writel(okDescriptor + 0x04, (1 << 6) | 1); /* SEMAPHORE, DMA_WRITE */
    await machine.writel(okDescriptor + 0x08, 0x00010000);
    await machine.writel(okDescriptor + 0x0c, 0);

    await machine.writel(0x00010000, 0xdeadbeef);
    await machine.writel(apbxCh2Nxtcmdar, okDescriptor);
    await machine.writel(apbxCh2Sema, 1);
    assert.equal(
      await machine.readl(0x00010000),
      0,
      'APBX XFER_COUNT=0 must transfer 64 KiB bytes to the byte address in BAR',
    );
    assert.equal(
      await machine.readl(apbxCh2Bar),
      0x00010000,
      'APBX CH2 BAR must reflect the loaded descriptor buffer address',
    );
    assert.equal(
      await machine.readl(apbxCh2Cmd) & 0x0000ffff,
      0x41,
      'APBX CH2 CMD must preserve the loaded descriptor command word',
    );

    /* AHB error: XFER_COUNT=0 with an unmapped byte address. */
    const errDescriptor = 0x00000510;
    await machine.writel(errDescriptor + 0x00, 0);
    await machine.writel(errDescriptor + 0x04, (1 << 6) | 1); /* SEMAPHORE, DMA_WRITE */
    await machine.writel(errDescriptor + 0x08, 0xdead0000);
    await machine.writel(errDescriptor + 0x0c, 0);

    await machine.writel(apbxCh2Nxtcmdar, errDescriptor);
    await machine.writel(apbxCh2Sema, 1);
    assert.equal(
      (await machine.readl(APBX_BASE + 0x010)) & (1 << 18),
      1 << 18,
      'APBX CH2 AHB_ERROR_IRQ status must be set on a bus error',
    );
    assert.notEqual(
      (await machine.readl(ICOLL_BASE + 0x040)) & (1 << 9),
      0,
      'APBX CH2 AHB error must assert the SPDIF_DMA ICOLL source (raw bit 9)',
    );
  });
}

async function testDmaCtrl1AndDevselContract() {
  await withMachine(async (machine) => {
    /* Release both DMAs from reset and clock gate. */
    await machine.writel(APBH_BASE + 0x008, 0xc0000000);
    await machine.writel(APBX_BASE + 0x008, 0xc0000000);

    /* CTRL1: bits 31:24 are RSVD RO, bits 23:0 are all RW. */
    /* Write all-1s to CTRL1 base; RSVD bits must stay 0. */
    await machine.writel(APBH_BASE + 0x010, 0xffffffff);
    assert.equal(
      await machine.readl(APBH_BASE + 0x010),
      0x00ffffff,
      'APBH CTRL1 must not accept writes to RSVD bits 31:24',
    );

    /* Clear all sticky/enable bits via CLR. */
    await machine.writel(APBH_BASE + 0x018, 0x00ffffff);
    assert.equal(
      await machine.readl(APBH_BASE + 0x010),
      0,
      'APBH CTRL1 CLR must clear all writable bits',
    );

    /* SET must be able to set AHB_ERROR_IRQ sticky bits (not just enables). */
    await machine.writel(APBH_BASE + 0x014, (1 << 16));
    assert.equal(
      (await machine.readl(APBH_BASE + 0x010)) & (1 << 16),
      1 << 16,
      'APBH CTRL1 SET must be able to set CH0_AHB_ERROR_IRQ',
    );
    await machine.writel(APBH_BASE + 0x018, (1 << 16));

    /* APBH DEVSEL: all fields RO, writes must be ignored. */
    await machine.writel(APBH_BASE + 0x020, 0xffffffff);
    assert.equal(
      await machine.readl(APBH_BASE + 0x020),
      0,
      'APBH DEVSEL must be entirely read-only',
    );

    /* APBX DEVSEL: only CH7 (31:28), CH6 (27:24), CH2 (11:8) are RW. */
    await machine.writel(APBX_BASE + 0x020, 0xffffffff);
    const apbxDevsel = await machine.readl(APBX_BASE + 0x020);
    assert.equal(
      apbxDevsel,
      0xff000f00,
      'APBX DEVSEL must only accept writes to CH7/CH6/CH2 fields',
    );

    /* CLR APBX DEVSEL back to 0. */
    await machine.writel(APBX_BASE + 0x028, 0xffffffff);
    assert.equal(
      await machine.readl(APBX_BASE + 0x020),
      0,
      'APBX DEVSEL CLR must clear all writable fields',
    );

    /* APBX CTRL0: bits 15:8 (CLKGATE_CHANNEL) are RSVD RO, unlike APBH. */
    /* First, clear SFTRST/CLKGATE so other bits are writable. */
    await machine.writel(APBX_BASE + 0x008, 0xc0000000);
    /* Write all-1s to CTRL0; APBX must not accept bits 15:8. */
    await machine.writel(APBX_BASE + 0x000, 0x0000ff00);
    assert.equal(
      (await machine.readl(APBX_BASE + 0x000)) & 0x0000ff00,
      0,
      'APBX CTRL0 bits 15:8 (CLKGATE_CHANNEL) must be read-only',
    );
    /* APBH CTRL0 bits 15:8 (CLKGATE_CHANNEL) must be writable. */
    await machine.writel(APBH_BASE + 0x008, 0xc0000000);
    await machine.writel(APBH_BASE + 0x000, 0x0000ff00);
    assert.notEqual(
      (await machine.readl(APBH_BASE + 0x000)) & 0x0000ff00,
      0,
      'APBH CTRL0 bits 15:8 (CLKGATE_CHANNEL) must be writable',
    );
    await machine.writel(APBH_BASE + 0x008, 0x0000ff00);
  });
}

async function testOnChipRomAndSramMirrorContract() {
  await withMachine(async (machine) => {
    await machine.writel(SRAM_BASE + 0x1234, 0x11223344);
    assert.equal(
      await machine.readl(0x00081234),
      0x11223344,
      'STMP3770 OCRAM must mirror every 512 KiB across the documented low 1 GiB window',
    );
    assert.equal(
      await machine.readl(0x3ff81234),
      0x11223344,
      'STMP3770 OCRAM last low-window mirror must alias physical OCRAM',
    );

    await machine.writel(0x3ff81234, 0xaabbccdd);
    assert.equal(
      await machine.readl(SRAM_BASE + 0x1234),
      0xaabbccdd,
      'writes through an OCRAM mirror must update the base OCRAM instance',
    );

    assert.equal(
      await machine.readl(OCROM_BASE),
      0,
      'STMP3770 OCROM reset vector storage must be mapped at 0xffff0000',
    );
    await machine.writel(OCROM_BASE, 0xdeadbeef);
    assert.equal(
      await machine.readl(OCROM_BASE),
      0,
      'STMP3770 OCROM must remain read-only to CPU writes',
    );
  });
}

async function testDcpRegisterAndMemcopyContract() {
  await withMachine(async (machine) => {
    assert.equal(
      await machine.readl(DCP_BASE + 0x000),
      0xf0800000,
      'DCP CTRL must reset with SFTRST, CLKGATE, crypto, CSC, and gather capability bits',
    );
    assert.equal(
      await machine.readl(DCP_BASE + 0x010),
      0x10000000,
      'DCP STAT must report OTP_KEY_READY after reset',
    );
    assert.equal(
      await machine.readl(DCP_BASE + 0x020),
      0,
      'DCP CHANNELCTRL must reset disabled',
    );
    assert.equal(
      await machine.readl(DCP_BASE + 0x030),
      0x00000404,
      'DCP CAPABILITY0 must report four channels and four key slots',
    );
    assert.equal(
      await machine.readl(DCP_BASE + 0x040),
      0x00010001,
      'DCP CAPABILITY1 must report SHA1 and AES128 support',
    );
    assert.equal(
      await machine.readl(DCP_BASE + 0x420),
      0x01000000,
      'DCP VERSION must report v1.0',
    );

    await machine.writel(DCP_BASE + 0x008, 0xc0800000);
    assert.equal(
      await machine.readl(DCP_BASE + 0x000),
      0x30000000,
      'DCP CTRL_CLR must release reset and gate while preserving read-only present bits',
    );
    await machine.writel(DCP_BASE + 0x004, 0x00e001ff);
    assert.equal(
      await machine.readl(DCP_BASE + 0x000),
      0x30e001ff,
      'DCP CTRL_SET must retain only documented writable control bits',
    );
    await machine.writel(DCP_BASE + 0x00c, 0xffffffff);
    assert.equal(
      await machine.readl(DCP_BASE + 0x000),
      0xf0800000,
      'DCP CTRL_TOG must reset the block while preserving the documented reset contract',
    );
    await machine.writel(DCP_BASE + 0x008, 0xc0000000);
    await machine.writel(DCP_BASE + 0x100, 0x00000100);
    assert.equal(
      await machine.readl(DCP_BASE + 0x100),
      0x00000100,
      'DCP CH0CMDPTR must retain the descriptor address',
    );
    await machine.writel(DCP_BASE + 0x110, 0x00000002);
    assert.equal(
      await machine.readl(DCP_BASE + 0x110),
      0x00020000,
      'DCP CH0SEMA must expose its atomic count only in VALUE[23:16]',
    );
    await machine.writel(DCP_BASE + 0x118, 0x000000ff);
    assert.equal(
      await machine.readl(DCP_BASE + 0x110),
      0,
      'DCP CH0SEMA_CLR must clear the semaphore count',
    );
    await machine.writel(DCP_BASE + 0x120, 0xffffffff);
    assert.equal(
      await machine.readl(DCP_BASE + 0x120),
      0x00ff003e,
      'DCP CH0STAT must retain only its documented software-clearable error fields',
    );
    await machine.writel(DCP_BASE + 0x128, 0x00ff003e);

    await machine.writel(DCP_BASE + 0x004, 0x00000001);
    await machine.writel(DCP_BASE + 0x024, 0x00000001);

    const descriptor = 0x00000100;
    const source = 0x00000200;
    const destination = 0x00000300;
    const tag = 0x5a;
    const control = (tag << 24) | 0x00000013;

    await machine.writel(source, 0x11223344);
    await machine.writel(source + 4, 0x55667788);
    await machine.writel(descriptor + 0x00, 0);
    await machine.writel(descriptor + 0x04, control);
    await machine.writel(descriptor + 0x08, 0);
    await machine.writel(descriptor + 0x0c, source);
    await machine.writel(descriptor + 0x10, destination);
    await machine.writel(descriptor + 0x14, 8);
    await machine.writel(descriptor + 0x18, 0);
    await machine.writel(descriptor + 0x1c, 0);

    await machine.writel(DCP_BASE + 0x100, descriptor);
    await machine.writel(DCP_BASE + 0x110, 1);
    assert.equal(
      await machine.readl(destination),
      0x11223344,
      'DCP CH0 memcopy must transfer the first source word into SRAM',
    );
    assert.equal(
      await machine.readl(destination + 4),
      0x55667788,
      'DCP CH0 memcopy must transfer the complete requested buffer',
    );
    assert.equal(
      await machine.readl(descriptor + 0x1c),
      0x5a000001,
      'DCP must write the descriptor completion status with the command tag',
    );
    assert.equal(
      await machine.readl(DCP_BASE + 0x120),
      0x5a000000,
      'DCP CH0STAT must retain the completed command tag',
    );
    assert.equal(
      await machine.readl(DCP_BASE + 0x090),
      control,
      'DCP PACKET1 must expose the active descriptor control snapshot',
    );
    assert.equal(
      await machine.readl(DCP_BASE + 0x0b0),
      source,
      'DCP PACKET3 must expose the active descriptor source snapshot',
    );
    assert.equal(
      await machine.readl(DCP_BASE + 0x0c0),
      destination,
      'DCP PACKET4 must expose the active descriptor destination snapshot',
    );
    assert.notEqual(
      (await machine.readl(DCP_BASE + 0x010)) & 1,
      0,
      'DCP must latch CH0 interrupt status after an interrupting descriptor',
    );
    assert.notEqual(
      (await machine.readl(ICOLL_BASE + 0x050)) & (1 << 21),
      0,
      'DCP CH0 must assert the dedicated DCP VMI interrupt on ICOLL source 53',
    );
    await machine.writel(DCP_BASE + 0x018, 1);
    assert.equal(
      (await machine.readl(ICOLL_BASE + 0x050)) & (1 << 21),
      0,
      'DCP STAT_CLR must deassert the DCP VMI interrupt after acknowledgment',
    );
  });
}

async function testDcpChannelRegisterMapContract() {
  await withMachine(async (machine) => {
    for (const channel of [1, 2, 3]) {
      const base = DCP_BASE + 0x100 + channel * 0x40;
      const command = 0x01020304 * (channel + 1);

      await machine.writel(base + 0x00, command);
      assert.equal(
        await machine.readl(base + 0x00),
        command,
        `DCP CH${channel}CMDPTR must retain its descriptor pointer`,
      );
      await machine.writel(base + 0x10, 2);
      assert.equal(
        await machine.readl(base + 0x10),
        0x00020000,
        `DCP CH${channel}SEMA must expose its count in VALUE[23:16] only`,
      );
      await machine.writel(base + 0x20, 0xffffffff);
      assert.equal(
        await machine.readl(base + 0x20),
        0x00ff003e,
        `DCP CH${channel}STAT must mask reserved bits`,
      );
      await machine.writel(base + 0x30, 0xffffffff);
      assert.equal(
        await machine.readl(base + 0x30),
        0x0000ffff,
        `DCP CH${channel}OPTS must retain RECOVERY_TIMER only`,
      );
    }
  });
}

async function testDcpKeyAndContextRegisterContract() {
  await withMachine(async (machine) => {
    await machine.writel(DCP_BASE + 0x050, 0x10203040);
    assert.equal(
      await machine.readl(DCP_BASE + 0x050),
      0x10203040,
      'DCP CONTEXT must retain its complete pointer value',
    );
    await machine.writel(DCP_BASE + 0x060, 0xffffffff);
    assert.equal(
      await machine.readl(DCP_BASE + 0x060),
      0x00000033,
      'DCP KEY must retain only INDEX and SUBWORD fields',
    );
    await machine.writel(DCP_BASE + 0x070, 0xa5a55a5a);
    assert.equal(
      await machine.readl(DCP_BASE + 0x060),
      0x00000030,
      'DCP KEYDATA writes must advance KEY.SUBWORD with wraparound',
    );
    await machine.writel(DCP_BASE + 0x060, 0x00000033);
    assert.equal(
      await machine.readl(DCP_BASE + 0x070),
      0xa5a55a5a,
      'DCP KEYDATA must expose the selected stored key word',
    );
  });
}

async function testDcpCscRegisterMapContract() {
  await withMachine(async (machine) => {
    const coefficientResets = [
      [0x380, 0x012a8010],
      [0x390, 0x01980204],
      [0x3a0, 0x00d00064],
    ];
    const coefficientMasks = [0x03ffffff, 0x03ff03ff, 0x03ff03ff];

    for (const [offset, value] of coefficientResets) {
      assert.equal(
        await machine.readl(DCP_BASE + offset),
        value,
        `DCP CSC coefficient at 0x${offset.toString(16)} must have its documented reset`,
      );
    }
    await machine.writel(DCP_BASE + 0x300, 0xffffffff);
    assert.equal(
      await machine.readl(DCP_BASE + 0x300),
      0x00007ff1,
      'DCP CSCCTRL0 must mask all reserved control bits',
    );
    await machine.writel(DCP_BASE + 0x310, 0xffffffff);
    assert.equal(
      await machine.readl(DCP_BASE + 0x310),
      0x00ff0035,
      'DCP CSCSTAT must retain only documented status fields',
    );
    await machine.writel(DCP_BASE + 0x320, 0xffffffff);
    assert.equal(
      await machine.readl(DCP_BASE + 0x320),
      0x00ffffff,
      'DCP CSCOUTBUFPARAM must retain its 24 documented bits',
    );
    await machine.writel(DCP_BASE + 0x330, 0xffffffff);
    assert.equal(
      await machine.readl(DCP_BASE + 0x330),
      0x00000fff,
      'DCP CSCINBUFPARAM must retain its 12 documented bits',
    );
    for (const offset of [0x340, 0x350, 0x360, 0x370]) {
      await machine.writel(DCP_BASE + offset, 0x10203040 + offset);
      assert.equal(
        await machine.readl(DCP_BASE + offset),
        0x10203040 + offset,
        `DCP CSC working pointer at 0x${offset.toString(16)} must be writable`,
      );
    }
    for (const [index, [offset]] of coefficientResets.entries()) {
      await machine.writel(DCP_BASE + offset, 0xffffffff);
      assert.equal(
        await machine.readl(DCP_BASE + offset),
        coefficientMasks[index],
        `DCP CSC coefficient at 0x${offset.toString(16)} must hide reserved bits`,
      );
    }
    for (const offset of [0x3e0, 0x3f0]) {
      await machine.writel(DCP_BASE + offset, 0xffffffff);
      assert.equal(
        await machine.readl(DCP_BASE + offset),
        0x03ffffff,
        `DCP CSC scale at 0x${offset.toString(16)} must retain documented bits`,
      );
    }
    await machine.writel(DCP_BASE + 0x400, 0xffffffff);
    assert.equal(
      await machine.readl(DCP_BASE + 0x400),
      0x000000ff,
      'DCP DBGSELECT must retain only its byte-wide selector',
    );
    await machine.writel(DCP_BASE + 0x410, 0xffffffff);
    assert.equal(
      await machine.readl(DCP_BASE + 0x410),
      0,
      'DCP DBGDATA must remain read-only',
    );
  });
}

async function testLcdifDataAccessContract() {
  await withMachine(async (machine) => {
    const ctrl = 0x00030001;

    await machine.writel(LCDIF_BASE + 0x008, 0xc0000000);
    await machine.writel(LCDIF_BASE + 0x000, ctrl);
    await machine.writeb(LCDIF_BASE + 0x0b0, 0xdb);
    assert.equal(
      (await machine.readl(LCDIF_BASE + 0x000)) & 0x00010000,
      0,
      'LCDIF byte DATA write must consume COUNT and clear RUN at transfer completion',
    );
    assert.equal(
      await machine.readb(LCDIF_BASE + 0x0b0),
      0x80,
      'LCDIF DATA must support byte reads from the selected panel register',
    );
  });
}

async function testPinctrlCtrl() {
  await withMachine(async (machine) => {
    /* Reset: SFTRST and CLKGATE set, PRESENT0/1/2 set, PRESENT3 clear, IRQOUT clear. */
    const ctrl = await machine.readl(PINCTRL_BASE + 0x000);
    assert.equal(
      (ctrl & 0xFC00000F) >>> 0,
      0xDC000000,
      'PINCTRL CTRL must reset with SFTRST/CLKGATE/PRESENT0/1/2 set and IRQOUT clear',
    );
    assert.equal(ctrl & (1 << 29), 0, 'PINCTRL PRESENT3 must be 0 on STMP3770');

    /* Clear SFTRST/CLKGATE and set up a level-active GPIO0 interrupt. */
    await machine.writel(PINCTRL_BASE + 0x008, 0xC0000000); /* CTRL_CLR */
    await machine.writel(PINCTRL_BASE + 0x600, 0x00000001); /* DOE0 */
    await machine.writel(PINCTRL_BASE + 0x700, 0x00000001); /* PIN2IRQ0 */
    await machine.writel(PINCTRL_BASE + 0x800, 0x00000001); /* IRQEN0 */
    await machine.writel(PINCTRL_BASE + 0x900, 0x00000001); /* IRQLEVEL0 */
    await machine.writel(PINCTRL_BASE + 0xA00, 0x00000001); /* IRQPOL0 */
    await machine.writel(PINCTRL_BASE + 0x400, 0x00000001); /* DOUT0 high */

    const ctrl_irq = await machine.readl(PINCTRL_BASE + 0x000);
    assert.equal(
      ctrl_irq & 0xF,
      0x1,
      'PINCTRL CTRL IRQOUT0 must reflect the active level-sensitive GPIO0 interrupt',
    );

    /* Writing SFTRST=1 must clear the IRQOUT view and leave PRESENT bits untouched. */
    await machine.writel(PINCTRL_BASE + 0x000, 0xFFFFFFFF); /* CTRL */
    const ctrl_after = await machine.readl(PINCTRL_BASE + 0x000);
    assert.equal(
      (ctrl_after & 0xFC00000F) >>> 0,
      0xDC000000,
      'PINCTRL CTRL must re-enter reset state (SFTRST/CLKGATE/PRESENT preserved, IRQOUT cleared)',
    );
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

async function testPinctrlDriveAndPullMasks() {
  await withMachine(async (machine) => {
    assert.equal(
      await machine.readl(PINCTRL_BASE + 0x270),
      0x00044444,
      'PINCTRL DRIVE7 must reset every documented voltage-select field high',
    );
    assert.equal(
      await machine.readl(PINCTRL_BASE + 0x200),
      0x44444444,
      'PINCTRL DRIVE0 must reset every documented voltage-select field high',
    );
    assert.equal(
      await machine.readl(PINCTRL_BASE + 0x280),
      0x44444444,
      'PINCTRL DRIVE8 must reset every documented voltage-select field high',
    );
    assert.equal(
      await machine.readl(PINCTRL_BASE + 0x2e0),
      0x00444444,
      'PINCTRL DRIVE14 must reset every documented voltage-select field high',
    );
    assert.equal(
      await machine.readl(PINCTRL_BASE + 0x230),
      0x00444444,
      'PINCTRL DRIVE3 must reset every documented voltage-select field high',
    );

    await machine.writel(PINCTRL_BASE + 0x270, 0xffffffff);
    assert.equal(
      await machine.readl(PINCTRL_BASE + 0x270),
      0x00077777,
      'PINCTRL DRIVE7 must expose only the Bank 1 pin 24-28 fields',
    );

    await machine.writel(PINCTRL_BASE + 0x280, 0xffffffff);
    assert.equal(
      await machine.readl(PINCTRL_BASE + 0x280),
      0x77777777,
      'PINCTRL DRIVE8 must keep every per-pin reserved bit clear',
    );

    await machine.writel(PINCTRL_BASE + 0x2e0, 0xffffffff);
    assert.equal(
      await machine.readl(PINCTRL_BASE + 0x2e0),
      0x00777777,
      'PINCTRL DRIVE14 must hide bits 31:24 and each reserved pad bit',
    );

    await machine.writel(PINCTRL_BASE + 0x230, 0xffffffff);
    assert.equal(
      await machine.readl(PINCTRL_BASE + 0x230),
      0x00777777,
      'PINCTRL DRIVE3 must hide bits 31:24 and each reserved pad bit',
    );

    await machine.writel(PINCTRL_BASE + 0x300, 0xffffffff);
    assert.equal(
      await machine.readl(PINCTRL_BASE + 0x300),
      0x3c1000fe,
      'PINCTRL PULL0 must retain only documented pullup-enable bits',
    );

    await machine.writel(PINCTRL_BASE + 0x310, 0xffffffff);
    assert.equal(
      await machine.readl(PINCTRL_BASE + 0x310),
      0x0f400000,
      'PINCTRL PULL1 must retain only documented pullup-enable bits',
    );

    await machine.writel(PINCTRL_BASE + 0x320, 0xffffffff);
    assert.equal(
      await machine.readl(PINCTRL_BASE + 0x320),
      0x00004000,
      'PINCTRL PULL2 must retain only the EMI_CE2N pullup-enable bit',
    );

    assert.equal(
      await machine.readl(PINCTRL_BASE + 0x330),
      0,
      'PINCTRL PULL3 must reset to 0 (all pad keepers enabled)',
    );
    await machine.writel(PINCTRL_BASE + 0x330, 0xffffffff);
    assert.equal(
      await machine.readl(PINCTRL_BASE + 0x330),
      0x0003ffff,
      'PINCTRL PULL3 must retain only bits 17:0 (pad-keeper disable bits)',
    );

    assert.equal(
      await machine.readl(PINCTRL_BASE + 0x304),
      0,
      'PINCTRL PULL0_SET is write-only and must read back zero',
    );
    assert.equal(
      await machine.readl(PINCTRL_BASE + 0x278),
      0,
      'PINCTRL DRIVE7_CLR is write-only and must read back zero',
    );
  });
}

async function testPinctrlMuxselDefaultAndMask() {
  await withMachine(async (machine) => {
    /* Reset: every documented pin defaults to 0b11 (GPIO) with reserved bits zero. */
    assert.equal(await machine.readl(PINCTRL_BASE + 0x100), 0xffffffff, 'PINCTRL MUXSEL0 must reset to GPIO for all 16 pins');
    assert.equal(await machine.readl(PINCTRL_BASE + 0x110), 0x0fffffff, 'PINCTRL MUXSEL1 must reset to GPIO for pins 16-29');
    assert.equal(await machine.readl(PINCTRL_BASE + 0x120), 0xffffffff, 'PINCTRL MUXSEL2 must reset to GPIO for all 16 pins');
    assert.equal(await machine.readl(PINCTRL_BASE + 0x130), 0x03ffffff, 'PINCTRL MUXSEL3 must reset to GPIO for pins 16-28');
    assert.equal(await machine.readl(PINCTRL_BASE + 0x140), 0xffffffff, 'PINCTRL MUXSEL4 must reset to GPIO for all 16 pins');
    assert.equal(await machine.readl(PINCTRL_BASE + 0x150), 0xffffffff, 'PINCTRL MUXSEL5 must reset to GPIO for all 16 pins');
    assert.equal(await machine.readl(PINCTRL_BASE + 0x160), 0xffffffff, 'PINCTRL MUXSEL6 must reset to GPIO for all 16 pins');
    assert.equal(await machine.readl(PINCTRL_BASE + 0x170), 0x00000fff, 'PINCTRL MUXSEL7 must reset to GPIO for pins 16-21');

    /* Reserved bits must be write-zero and not writable. */
    await machine.writel(PINCTRL_BASE + 0x110, 0xffffffff);
    assert.equal(await machine.readl(PINCTRL_BASE + 0x110), 0x0fffffff, 'PINCTRL MUXSEL1 must preserve reserved bits 31:28');

    await machine.writel(PINCTRL_BASE + 0x130, 0xffffffff);
    assert.equal(await machine.readl(PINCTRL_BASE + 0x130), 0x03ffffff, 'PINCTRL MUXSEL3 must preserve reserved bits 31:26');

    await machine.writel(PINCTRL_BASE + 0x170, 0xffffffff);
    assert.equal(await machine.readl(PINCTRL_BASE + 0x170), 0x00000fff, 'PINCTRL MUXSEL7 must preserve reserved bits 31:12');

    await machine.writel(PINCTRL_BASE + 0x100, 0xffffffff);
    assert.equal(await machine.readl(PINCTRL_BASE + 0x100), 0xffffffff, 'PINCTRL MUXSEL0 must accept all bits');
  });
}

async function testPinctrlGpioIrqstatContract() {
  await withMachine(async (machine) => {
    /* Drive Bank 0 pin 0 via DOUT/DOE to simulate the looped-back input. */
    await machine.writel(PINCTRL_BASE + 0x600, 0x00000001); /* DOE0 */
    await machine.writel(PINCTRL_BASE + 0x700, 0x00000001); /* PIN2IRQ0 */
    await machine.writel(PINCTRL_BASE + 0x800, 0x00000001); /* IRQEN0 */
    await machine.writel(PINCTRL_BASE + 0x900, 0x00000001); /* IRQLEVEL0 level */
    await machine.writel(PINCTRL_BASE + 0xA00, 0x00000001); /* IRQPOL0 active high */

    /* Level-sensitive: IRQSTAT follows the input state. */
    await machine.writel(PINCTRL_BASE + 0x400, 0x00000001); /* DOUT0 high */
    assert.equal(
      await machine.readl(PINCTRL_BASE + 0xB00) & 0x1,
      0x1,
      'PINCTRL IRQSTAT level-sensitive must set when active high input is asserted',
    );

    /* Software clear must be ignored while the level-active condition remains. */
    await machine.writel(PINCTRL_BASE + 0xB08, 0x00000001); /* IRQSTAT0_CLR */
    assert.equal(
      await machine.readl(PINCTRL_BASE + 0xB00) & 0x1,
      0x1,
      'PINCTRL IRQSTAT level-sensitive must remain set while input stays active',
    );

    await machine.writel(PINCTRL_BASE + 0x400, 0x00000000); /* DOUT0 low */
    assert.equal(
      await machine.readl(PINCTRL_BASE + 0xB00) & 0x1,
      0x0,
      'PINCTRL IRQSTAT level-sensitive must clear when input returns inactive',
    );

    /* Edge-sensitive: IRQSTAT latches on the active transition. */
    await machine.writel(PINCTRL_BASE + 0x900, 0x00000000); /* IRQLEVEL0 edge */
    await machine.writel(PINCTRL_BASE + 0xB08, 0x00000001); /* clear IRQSTAT */
    await machine.writel(PINCTRL_BASE + 0x400, 0x00000001); /* DOUT0 high -> rising edge */
    assert.equal(
      await machine.readl(PINCTRL_BASE + 0xB00) & 0x1,
      0x1,
      'PINCTRL IRQSTAT edge-sensitive must set on active high rising edge',
    );

    await machine.writel(PINCTRL_BASE + 0x400, 0x00000000); /* DOUT0 low */
    assert.equal(
      await machine.readl(PINCTRL_BASE + 0xB00) & 0x1,
      0x1,
      'PINCTRL IRQSTAT edge-sensitive must remain latched after input deasserts',
    );

    await machine.writel(PINCTRL_BASE + 0xB08, 0x00000001); /* clear IRQSTAT */
    assert.equal(
      await machine.readl(PINCTRL_BASE + 0xB00) & 0x1,
      0x0,
      'PINCTRL IRQSTAT edge-sensitive must clear on software clear',
    );
  });
}

async function testIcollCoreContract() {
  await withMachine(async (machine) => {
    const ctrlReset = await machine.readl(ICOLL_BASE + 0x020);
    const version = await machine.readl(ICOLL_BASE + 0x1d0);
    const debugRead0 = await machine.readl(ICOLL_BASE + 0x180);
    const debugRead1 = await machine.readl(ICOLL_BASE + 0x190);

    assert.equal(
      ctrlReset,
      0xc0030000,
      `ICOLL CTRL reset mismatch: got 0x${ctrlReset.toString(16)}`,
    );
    assert.equal(version, 0x02000000, `ICOLL VERSION mismatch: got 0x${version.toString(16)}`);
    assert.equal(debugRead0, 0xeca94567, `ICOLL DEBUGRD0 mismatch: got 0x${debugRead0.toString(16)}`);
    assert.equal(debugRead1, 0x1356da98, `ICOLL DEBUGRD1 mismatch: got 0x${debugRead1.toString(16)}`);

    await machine.writel(ICOLL_BASE + 0x028, 0xc0000000);
    await machine.writel(ICOLL_BASE + 0x160, 0xffffffff);
    await machine.writel(ICOLL_BASE + 0x060, 0xffffffff);

    const vbase = await machine.readl(ICOLL_BASE + 0x160);
    const priority0 = await machine.readl(ICOLL_BASE + 0x060);

    assert.equal(vbase, 0xfffffffc, `ICOLL VBASE must keep word alignment: got 0x${vbase.toString(16)}`);
    assert.equal(
      priority0,
      0x0f0f0f0f,
      `ICOLL PRIORITY0 must hide reserved nibbles: got 0x${priority0.toString(16)}`,
    );

    await machine.writel(ICOLL_BASE + 0x060, 0);
    await machine.writel(ICOLL_BASE + 0x000, 0);
    await machine.writel(ICOLL_BASE + 0x010, 0x00000008);

    await machine.writel(ICOLL_BASE + 0x160, 0x00001000);
    await machine.writel(ICOLL_BASE + 0x060, 0x00000f00);

    const vector = await machine.readl(ICOLL_BASE + 0x000);
    const stat = await machine.readl(ICOLL_BASE + 0x030);

    assert.equal(
      vector,
      0x00001004,
      `ICOLL must select the highest-priority SOFTIRQ source: got 0x${vector.toString(16)}`,
    );
    assert.equal(stat & 0x3f, 1, `ICOLL STAT must report selected source 1: got 0x${stat.toString(16)}`);

    await machine.writel(ICOLL_BASE + 0x024, 0x00200000);
    const pitchOneVector = await machine.readl(ICOLL_BASE + 0x000);
    assert.equal(
      pitchOneVector,
      0x00001004,
      `ICOLL VECTOR_PITCH=1 must remain a 4-byte stride: got 0x${pitchOneVector.toString(16)}`,
    );
  });
}

async function testIcollSameLevelPriorityContract() {
  await withMachine(async (machine) => {
    await machine.writel(ICOLL_BASE + 0x028, 0xc0000000);
    await machine.writel(ICOLL_BASE + 0x160, 0x00001000);
    await machine.writel(ICOLL_BASE + 0x060, 0x0c00000c);

    assert.equal(
      await machine.readl(ICOLL_BASE + 0x000),
      0x0000100c,
      'ICOLL must select the highest-numbered source when same-level requests coincide',
    );
    assert.equal(
      (await machine.readl(ICOLL_BASE + 0x030)) & 0x3f,
      3,
      'ICOLL STAT must report the highest-numbered same-level source',
    );
  });
}

async function testIcollBypassSameLevelPriorityContract() {
  await withMachine(async (machine) => {
    await machine.writel(ICOLL_BASE + 0x028, 0xc0000000);
    await machine.writel(ICOLL_BASE + 0x024, 0x00100000);
    await machine.writel(ICOLL_BASE + 0x160, 0x00001000);
    await machine.writel(ICOLL_BASE + 0x060, 0x0c00000c);

    assert.equal(
      await machine.readl(ICOLL_BASE + 0x000),
      0x0000100c,
      'ICOLL BYPASS_FSM must use the highest-numbered coincident request',
    );
  });
}

async function testIcollVectorAcknowledgeContract() {
  await withMachine(async (machine) => {
    await machine.writel(ICOLL_BASE + 0x028, 0xc0000000);
    await machine.writel(ICOLL_BASE + 0x160, 0x00002000);
    await machine.writel(ICOLL_BASE + 0x060, 0x00000f0c);

    const highVector = await machine.readl(ICOLL_BASE + 0x000);
    assert.equal(highVector, 0x00002004, `ICOLL should select level 3 source first: got 0x${highVector.toString(16)}`);

    await machine.writel(ICOLL_BASE + 0x000, 0);
    await machine.writel(ICOLL_BASE + 0x068, 0x00000800);

    const beforeAcknowledge = await machine.readl(ICOLL_BASE + 0x000);
    assert.equal(
      beforeAcknowledge,
      0x00002004,
      `ICOLL VECTOR must remain on the in-service level until LEVELACK: got 0x${beforeAcknowledge.toString(16)}`,
    );

    await machine.writel(ICOLL_BASE + 0x010, 0x00000008);
    const afterAcknowledge = await machine.readl(ICOLL_BASE + 0x000);
    assert.equal(
      afterAcknowledge,
      0x00002000,
      `ICOLL LEVELACK bit 3 must release level 3 for level 0: got 0x${afterAcknowledge.toString(16)}`,
    );
  });
}

async function testIcollArmRseModeContract() {
  await withMachine(async (machine) => {
    await machine.writel(ICOLL_BASE + 0x028, 0xc0000000);
    await machine.writel(ICOLL_BASE + 0x024, 0x00040000);
    await machine.writel(ICOLL_BASE + 0x160, 0x00003000);
    await machine.writel(ICOLL_BASE + 0x060, 0x00000f0c);

    const highVector = await machine.readl(ICOLL_BASE + 0x000);
    assert.equal(highVector, 0x00003004, `ICOLL should select level 3 source first: got 0x${highVector.toString(16)}`);

    await machine.writel(ICOLL_BASE + 0x068, 0x00000800);
    const beforeAcknowledge = await machine.readl(ICOLL_BASE + 0x000);
    assert.equal(
      beforeAcknowledge,
      0x00003004,
      `ICOLL ARM_RSE_MODE read must enter service before LEVELACK: got 0x${beforeAcknowledge.toString(16)}`,
    );

    await machine.writel(ICOLL_BASE + 0x010, 0x00000008);
    const afterAcknowledge = await machine.readl(ICOLL_BASE + 0x000);
    assert.equal(
      afterAcknowledge,
      0x00003000,
      `ICOLL ARM_RSE_MODE must let LEVELACK release the read vector: got 0x${afterAcknowledge.toString(16)}`,
    );
  });
}

async function testIcollNoNestingContract() {
  await withMachine(async (machine) => {
    await machine.writel(ICOLL_BASE + 0x028, 0xc0000000);
    await machine.writel(ICOLL_BASE + 0x024, 0x00080000);
    await machine.writel(ICOLL_BASE + 0x160, 0x00004000);
    await machine.writel(ICOLL_BASE + 0x060, 0x0000000c);

    const lowVector = await machine.readl(ICOLL_BASE + 0x000);
    assert.equal(lowVector, 0x00004000, `ICOLL should select level 0 source first: got 0x${lowVector.toString(16)}`);

    await machine.writel(ICOLL_BASE + 0x000, 0);
    await machine.writel(ICOLL_BASE + 0x064, 0x00000f00);

    const whileInService = await machine.readl(ICOLL_BASE + 0x000);
    assert.equal(
      whileInService,
      0x00004000,
      `ICOLL NO_NESTING must block higher priority preemption: got 0x${whileInService.toString(16)}`,
    );
  });
}

async function testIcollDebugFlagContract() {
  await withMachine(async (machine) => {
    const reset = await machine.readl(ICOLL_BASE + 0x1a0);
    assert.equal(reset, 0, `ICOLL DEBUGFLAG should reset to 0: got 0x${reset.toString(16)}`);

    await machine.writel(ICOLL_BASE + 0x1a0, 0xffffffff);
    const written = await machine.readl(ICOLL_BASE + 0x1a0);
    assert.equal(written, 0x0000ffff, `ICOLL DEBUGFLAG must expose only bits 15:0: got 0x${written.toString(16)}`);

    await machine.writel(ICOLL_BASE + 0x1a8, 0x000000f0);
    const cleared = await machine.readl(ICOLL_BASE + 0x1a0);
    assert.equal(cleared, 0x0000ff0f, `ICOLL DEBUGFLAG_CLR should clear selected flags: got 0x${cleared.toString(16)}`);

    await machine.writel(ICOLL_BASE + 0x1ac, 0x0000000f);
    const toggled = await machine.readl(ICOLL_BASE + 0x1a0);
    assert.equal(toggled, 0x0000ff00, `ICOLL DEBUGFLAG_TOG should toggle selected flags: got 0x${toggled.toString(16)}`);
  });
}

async function testIcollDebugStateContract() {
  await withMachine(async (machine) => {
    await machine.writel(ICOLL_BASE + 0x028, 0xc0000000);
    await machine.writel(ICOLL_BASE + 0x024, 0x00000001);
    await machine.writel(ICOLL_BASE + 0x0d0, 0x00000008);

    const debug = await machine.readl(ICOLL_BASE + 0x170);
    const requestLow = await machine.readl(ICOLL_BASE + 0x1b0);

    assert.equal(
      debug & (1 << 16),
      0,
      `ICOLL DEBUG.IRQ must remain low for a source routed to FIQ: debug=0x${debug.toString(16)}`,
    );
    assert.notEqual(
      debug & (1 << 17),
      0,
      `ICOLL DEBUG.FIQ must reflect the asserted CPU FIQ output: debug=0x${debug.toString(16)}`,
    );
    assert.notEqual(
      requestLow & (1 << 28),
      0,
      `ICOLL DBGREQUEST0 must expose software request 28: request=0x${requestLow.toString(16)}`,
    );

    await machine.writel(ICOLL_BASE + 0x170, 0xffffffff);
    await machine.writel(ICOLL_BASE + 0x1b0, 0xffffffff);
    assert.equal(
      await machine.readl(ICOLL_BASE + 0x170),
      debug,
      'ICOLL DEBUG must be read-only',
    );
    assert.equal(
      await machine.readl(ICOLL_BASE + 0x1b0),
      requestLow,
      'ICOLL DBGREQUEST0 must be read-only',
    );

    await machine.writel(ICOLL_BASE + 0x028, 0x00020000);
    const debugWithFinalFiqDisabled = await machine.readl(ICOLL_BASE + 0x170);
    assert.equal(
      debugWithFinalFiqDisabled & (1 << 17),
      0,
      `ICOLL DEBUG.FIQ must follow FIQ_FINAL_ENABLE: debug=0x${debugWithFinalFiqDisabled.toString(16)}`,
    );
  });
}

async function testIcollSoftResetContract() {
  await withMachine(async (machine) => {
    await machine.writel(ICOLL_BASE + 0x028, 0xc0000000);
    await machine.writel(ICOLL_BASE + 0x1a0, 0x0000005a);

    await machine.writel(ICOLL_BASE + 0x024, 0xc0000000);
    assert.equal(
      await machine.readl(ICOLL_BASE + 0x1a0),
      0x0000005a,
      'ICOLL simultaneous SFTRST and CLKGATE must leave state unchanged',
    );

    await machine.writel(ICOLL_BASE + 0x028, 0xc0000000);
    await machine.writel(ICOLL_BASE + 0x024, 0x80000000);
    const ctrlBeforeResetCompletes = await machine.readl(ICOLL_BASE + 0x020);
    assert.notEqual(
      ctrlBeforeResetCompletes & 0x80000000,
      0,
      `ICOLL SFTRST must remain asserted while reset is pending: ctrl=0x${ctrlBeforeResetCompletes.toString(16)}`,
    );
    assert.equal(
      ctrlBeforeResetCompletes & 0x40000000,
      0,
      `ICOLL CLKGATE must not assert before the reset delay elapses: ctrl=0x${ctrlBeforeResetCompletes.toString(16)}`,
    );

    await machine.clockStep(125);
    assert.equal(
      (await machine.readl(ICOLL_BASE + 0x020)) & 0x40000000,
      0,
      'ICOLL CLKGATE must remain low before four reset clocks',
    );

    await machine.clockStep(42);
    const ctrlAfterResetCompletes = await machine.readl(ICOLL_BASE + 0x020);
    assert.notEqual(
      ctrlAfterResetCompletes & 0x40000000,
      0,
      `ICOLL CLKGATE must assert when soft reset completes: ctrl=0x${ctrlAfterResetCompletes.toString(16)}`,
    );
    assert.equal(
      await machine.readl(ICOLL_BASE + 0x1a0),
      0,
      'ICOLL soft reset must clear DEBUGFLAG state',
    );
  });
}

async function testIcollBypassFsmContract() {
  await withMachine(async (machine) => {
    await machine.writel(ICOLL_BASE + 0x028, 0xc0000000);
    await machine.writel(ICOLL_BASE + 0x024, 0x00100000);
    await machine.writel(ICOLL_BASE + 0x160, 0x00005000);
    await machine.writel(ICOLL_BASE + 0x060, 0x00000800);

    const firstVector = await machine.readl(ICOLL_BASE + 0x000);
    assert.equal(
      firstVector,
      0x00005004,
      `ICOLL BYPASS_FSM should initially expose source 1: got 0x${firstVector.toString(16)}`,
    );

    await machine.writel(ICOLL_BASE + 0x060, 0x00000008);
    const bypassVector = await machine.readl(ICOLL_BASE + 0x000);
    assert.equal(
      bypassVector,
      0x00005000,
      `ICOLL BYPASS_FSM must continuously update the vector without VECTOR acknowledgement: got 0x${bypassVector.toString(16)}`,
    );

    const debug = await machine.readl(ICOLL_BASE + 0x170);
    assert.equal(
      debug & 0x3ff,
      0,
      `ICOLL BYPASS_FSM must bypass the vector request FSM: debug=0x${debug.toString(16)}`,
    );
  });
}

async function testIcollRequestHoldingContract() {
  await withMachine(async (machine) => {
    await machine.writel(ICOLL_BASE + 0x028, 0xc0000000);
    await machine.writel(ICOLL_BASE + 0x160, 0x00006000);

    assert.equal(
      await machine.readl(ICOLL_BASE + 0x040),
      0,
      'ICOLL RAW0 should reset low before exercising its read-only aliases',
    );
    await machine.writel(ICOLL_BASE + 0x040, 0xffffffff);
    await machine.writel(ICOLL_BASE + 0x044, 0xffffffff);
    await machine.writel(ICOLL_BASE + 0x048, 0xffffffff);
    await machine.writel(ICOLL_BASE + 0x04c, 0xffffffff);
    assert.equal(
      await machine.readl(ICOLL_BASE + 0x040),
      0,
      'ICOLL RAW0 and its aliases must not manufacture raw interrupt inputs',
    );

    await machine.writel(ICOLL_BASE + 0x060, 0x0000000c);
    assert.equal(
      await machine.readl(ICOLL_BASE + 0x1b0),
      0x00000001,
      'ICOLL DBGREQUEST0 should capture the first software request',
    );

    await machine.writel(ICOLL_BASE + 0x068, 0x00000008);
    await machine.writel(ICOLL_BASE + 0x064, 0x00000c00);
    assert.equal(
      await machine.readl(ICOLL_BASE + 0x1b0),
      0x00000001,
      'ICOLL DBGREQUEST0 must retain the closed holding-register snapshot until VECTOR acknowledgement',
    );

    await machine.writel(ICOLL_BASE + 0x000, 0);
    assert.equal(
      await machine.readl(ICOLL_BASE + 0x1b0),
      0x00000002,
      'ICOLL VECTOR acknowledgement must reopen the holding register for current requests',
    );

    await machine.writel(ICOLL_BASE + 0x010, 0x00000001);
    assert.equal(
      await machine.readl(ICOLL_BASE + 0x000),
      0x00006004,
      'ICOLL must service the newly sampled request after LEVELACK releases the prior level',
    );
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

    await machine.writel(CLKCTRL_BASE + 0x0f0, 0x00000001);
    const ctrlAfterDigReset = await machine.readl(DIGCTL_BASE + 0x000);
    assert.equal(
      ctrlAfterDigReset,
      0x0000000c,
      `DIGCTL CTRL.DEBUG_DISABLE should survive RESET.DIG and only recover after power-on/chip reset: got 0x${ctrlAfterDigReset.toString(16)}`,
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

async function testDigctlWriteonceResetsWithDigReset() {
  await withMachine(async (machine) => {
    const writeonceReset = await machine.readl(DIGCTL_BASE + 0x060);
    const statusReset = await machine.readl(DIGCTL_BASE + 0x010);

    assert.equal(
      writeonceReset,
      0xa5a5a5a5,
      `DIGCTL WRITEONCE should reset to its documented seed: got 0x${writeonceReset.toString(16)}`,
    );
    assert.equal(
      statusReset & 0x1,
      0,
      `DIGCTL STATUS.WRITTEN should reset low: got 0x${statusReset.toString(16)}`,
    );

    await machine.writel(DIGCTL_BASE + 0x060, 0x12345678);
    const writeonceWritten = await machine.readl(DIGCTL_BASE + 0x060);
    const statusAfterWrite = await machine.readl(DIGCTL_BASE + 0x010);

    assert.equal(
      writeonceWritten,
      0x12345678,
      `DIGCTL WRITEONCE should accept the first write: got 0x${writeonceWritten.toString(16)}`,
    );
    assert.notEqual(
      statusAfterWrite & 0x1,
      0,
      `DIGCTL STATUS.WRITTEN should set after a successful WRITEONCE write: got 0x${statusAfterWrite.toString(16)}`,
    );

    await machine.writel(DIGCTL_BASE + 0x060, 0x87654321);
    const writeonceLocked = await machine.readl(DIGCTL_BASE + 0x060);
    const statusAfterSecondWrite = await machine.readl(DIGCTL_BASE + 0x010);

    assert.equal(
      writeonceLocked,
      0x12345678,
      `DIGCTL WRITEONCE should ignore later writes until chip-wide reset: got 0x${writeonceLocked.toString(16)}`,
    );
    assert.notEqual(
      statusAfterSecondWrite & 0x1,
      0,
      `DIGCTL STATUS.WRITTEN should remain set after ignored WRITEONCE writes: got 0x${statusAfterSecondWrite.toString(16)}`,
    );

    await machine.writel(CLKCTRL_BASE + 0x0f0, 0x00000001);
    const writeonceAfterDigReset = await machine.readl(DIGCTL_BASE + 0x060);
    const statusAfterDigReset = await machine.readl(DIGCTL_BASE + 0x010);

    assert.equal(
      writeonceAfterDigReset,
      0xa5a5a5a5,
      `DIGCTL WRITEONCE should reset with RESET.DIG: got 0x${writeonceAfterDigReset.toString(16)}`,
    );
    assert.equal(
      statusAfterDigReset & 0x1,
      0,
      `DIGCTL STATUS.WRITTEN should clear with RESET.DIG: got 0x${statusAfterDigReset.toString(16)}`,
    );

    await machine.writel(DIGCTL_BASE + 0x060, 0x87654321);
    const writeonceAfterDigResetWrite = await machine.readl(DIGCTL_BASE + 0x060);
    assert.equal(
      writeonceAfterDigResetWrite,
      0x87654321,
      `DIGCTL WRITEONCE should accept a first write after RESET.DIG: got 0x${writeonceAfterDigResetWrite.toString(16)}`,
    );
  });
}

async function testDigctlHclkCountContract() {
  await withMachine(async (machine) => {
    const hclkStart = await machine.readl(DIGCTL_BASE + 0x020);
    await machine.clockStep(1_000);
    const hclkAt24Mhz = await machine.readl(DIGCTL_BASE + 0x020);

    assert.equal(
      (hclkAt24Mhz - hclkStart) >>> 0,
      24,
      `DIGCTL HCLKCOUNT must advance once per 24 MHz HCLK edge: start=0x${hclkStart.toString(16)}, end=0x${hclkAt24Mhz.toString(16)}`,
    );

    const hclkBeforeFractionalReads = await machine.readl(DIGCTL_BASE + 0x020);
    await machine.clockStep(20);
    const hclkAfterFirstFractionalRead = await machine.readl(DIGCTL_BASE + 0x020);
    await machine.clockStep(22);
    const hclkAfterSecondFractionalRead = await machine.readl(DIGCTL_BASE + 0x020);

    assert.equal(
      (hclkAfterFirstFractionalRead - hclkBeforeFractionalReads) >>> 0,
      0,
      'DIGCTL HCLKCOUNT must not increment before a 24 MHz HCLK edge',
    );
    assert.equal(
      (hclkAfterSecondFractionalRead - hclkBeforeFractionalReads) >>> 0,
      1,
      'DIGCTL HCLKCOUNT must retain fractional time across reads until an HCLK edge occurs',
    );

    await machine.writel(CLKCTRL_BASE + 0x030, 0x00000002);
    const hclkBeforeDivide = await machine.readl(DIGCTL_BASE + 0x020);
    await machine.clockStep(1_000);
    const hclkAt12Mhz = await machine.readl(DIGCTL_BASE + 0x020);

    assert.equal(
      (hclkAt12Mhz - hclkBeforeDivide) >>> 0,
      12,
      `DIGCTL HCLKCOUNT must follow HBUS.DIV=2: start=0x${hclkBeforeDivide.toString(16)}, end=0x${hclkAt12Mhz.toString(16)}`,
    );

    await machine.writel(CLKCTRL_BASE + 0x030, 0x00000001);
    await machine.writel(CLKCTRL_BASE + 0x0d8, 0x00000080);
    await machine.writel(CLKCTRL_BASE + 0x004, 0x00010000);
    await machine.writel(CLKCTRL_BASE + 0x0e8, 0x00000080);
    await machine.writel(CLKCTRL_BASE + 0x020, 0x00000600);

    const hclkBeforeCpuFractionalDivide = await machine.readl(DIGCTL_BASE + 0x020);
    await machine.clockStep(1_000);
    const hclkAt240Mhz = await machine.readl(DIGCTL_BASE + 0x020);

    assert.equal(
      (hclkAt240Mhz - hclkBeforeCpuFractionalDivide) >>> 0,
      240,
      `DIGCTL HCLKCOUNT must honor CPU.DIV_CPU_FRAC_EN at bit 10: start=0x${hclkBeforeCpuFractionalDivide.toString(16)}, end=0x${hclkAt240Mhz.toString(16)}`,
    );
  });
}

async function testDigctlReadOnlyStatusContract() {
  await withMachine(async (machine) => {
    const sjtagReset = await machine.readl(DIGCTL_BASE + 0x0b0);
    const dbgrd = await machine.readl(DIGCTL_BASE + 0x0d0);
    const dbg = await machine.readl(DIGCTL_BASE + 0x0e0);
    const chipId = await machine.readl(DIGCTL_BASE + 0x310);

    assert.equal(sjtagReset, 0x00020000, `DIGCTL SJTAGDBG reset mismatch: got 0x${sjtagReset.toString(16)}`);
    assert.equal(dbgrd, 0x789abcde, `DIGCTL DBGRD fixed complement mismatch: got 0x${dbgrd.toString(16)}`);
    assert.equal(dbg, 0x87654321, `DIGCTL DBG fixed value mismatch: got 0x${dbg.toString(16)}`);
    assert.equal(chipId, 0x37b00000, `DIGCTL CHIPID table reset mismatch: got 0x${chipId.toString(16)}`);

    await machine.writel(DIGCTL_BASE + 0x0b0, 0xffffffff);
    await machine.writel(DIGCTL_BASE + 0x0d0, 0xffffffff);
    await machine.writel(DIGCTL_BASE + 0x0e0, 0xffffffff);
    await machine.writel(DIGCTL_BASE + 0x310, 0xffffffff);

    assert.equal(
      await machine.readl(DIGCTL_BASE + 0x0b0),
      0x00020003,
      'DIGCTL SJTAGDBG must retain only diagnostic output bits 1:0',
    );
    assert.equal(await machine.readl(DIGCTL_BASE + 0x0d0), dbgrd, 'DIGCTL DBGRD must be read-only');
    assert.equal(await machine.readl(DIGCTL_BASE + 0x0e0), dbg, 'DIGCTL DBG must be read-only');
    assert.equal(await machine.readl(DIGCTL_BASE + 0x310), chipId, 'DIGCTL CHIPID must be read-only');

    await machine.writel(DIGCTL_BASE + 0x0f0, 0x00000101);
    assert.equal(
      await machine.readl(DIGCTL_BASE + 0x0f0),
      0x00000106,
      'DIGCTL OCRAM_BIST_CSR must self-clear START and report a completed passing BIST',
    );
  });
}

async function testDigctlDcpBistStatusContract() {
  await withMachine(async (machine) => {
    /* STATUS reset: USB features present, no DCP BIST, no JTAG, package type 0 */
    assert.equal(
      await machine.readl(DIGCTL_BASE + 0x010),
      0xf0000000,
      'DIGCTL STATUS must reset with USB features present and DCP BIST not done',
    );

    /* Enable DCP BIST clock and start it */
    await machine.writel(DIGCTL_BASE + 0x000, 0x00c00004);
    assert.equal(
      await machine.readl(DIGCTL_BASE + 0x000),
      0x00c00004,
      'DIGCTL CTRL must accept DCP_BIST_CLKEN and DCP_BIST_START',
    );
    assert.equal(
      await machine.readl(DIGCTL_BASE + 0x010),
      0xf0000300,
      'DIGCTL STATUS must report DCP BIST done and pass after start',
    );

    /* Clear DCP_BIST_START (leave clock enabled and USB clock gate intact) */
    await machine.writel(DIGCTL_BASE + 0x008, 0x00400000);
    assert.equal(
      await machine.readl(DIGCTL_BASE + 0x000),
      0x00800004,
      'DIGCTL CTRL DCP_BIST_START must clear via CLR alias',
    );
    assert.equal(
      await machine.readl(DIGCTL_BASE + 0x010),
      0xf0000300,
      'DIGCTL STATUS DCP BIST done/pass must remain sticky after start bit clears',
    );

    /* STATUS is read-only and rejects write attempts */
    await machine.writel(DIGCTL_BASE + 0x010, 0xffffffff);
    assert.equal(
      await machine.readl(DIGCTL_BASE + 0x010),
      0xf0000300,
      'DIGCTL STATUS must be read-only and preserve DCP BIST sticky bits',
    );
  });
}

async function testDigctlTrapContract() {
  await withMachine(async (machine) => {
    const trapRangeLow = 0x8001c010;
    const trapRangeHigh = 0x8001c01f;
    const trapIrqMask = 1 << 15; /* ICOLL RAW1 bit 15 = source 47 DIGCTL_TRAP */

    /* Set trap address range and enable trap with TRAP_IN_RANGE=1 */
    await machine.writel(DIGCTL_BASE + 0x2c0, trapRangeLow);
    await machine.writel(DIGCTL_BASE + 0x2d0, trapRangeHigh);
    await machine.writel(DIGCTL_BASE + 0x000, 0x00000034);

    assert.equal(
      await machine.readl(DIGCTL_BASE + 0x2c0),
      trapRangeLow,
      'DIGCTL DEBUG_TRAP_ADDR_LOW must retain written value',
    );
    assert.equal(
      await machine.readl(DIGCTL_BASE + 0x2d0),
      trapRangeHigh,
      'DIGCTL DEBUG_TRAP_ADDR_HIGH must retain written value',
    );
    assert.equal(
      await machine.readl(DIGCTL_BASE + 0x000),
      0x00000034,
      'DIGCTL CTRL must reflect TRAP_ENABLE, TRAP_IN_RANGE and USB_CLKGATE',
    );
    assert.equal(
      await machine.readl(ICOLL_BASE + 0x050),
      0,
      'ICOLL RAW1 source 47 must be deasserted before a trap',
    );

    /* Access STATUS (0x8001c010) which is inside the trap range */
    const status = await machine.readl(DIGCTL_BASE + 0x010);
    assert.equal(
      status,
      0xf0000000,
      'DIGCTL STATUS read must still return the correct value when trapped',
    );
    assert.equal(
      await machine.readl(DIGCTL_BASE + 0x000),
      0x20000034,
      'DIGCTL CTRL.TRAP_IRQ must set when an in-range AHB access is trapped',
    );
    assert.equal(
      await machine.readl(ICOLL_BASE + 0x050),
      trapIrqMask,
      'ICOLL source 47 (DIGCTL_TRAP) must assert when TRAP_IRQ is set',
    );

    /* Clear TRAP_IRQ via CLR alias (0x8001c008 is outside the trap range) */
    await machine.writel(DIGCTL_BASE + 0x008, 0x20000000);
    assert.equal(
      await machine.readl(DIGCTL_BASE + 0x000),
      0x00000034,
      'DIGCTL CTRL.TRAP_IRQ must clear via CLR alias',
    );
    assert.equal(
      await machine.readl(ICOLL_BASE + 0x050),
      0,
      'ICOLL source 47 must deassert when TRAP_IRQ is cleared',
    );

    /* Change TRAP_IN_RANGE to 0 (outside range triggers) and read CTRL (0x8001c000) which is outside */
    await machine.writel(DIGCTL_BASE + 0x000, 0x00000014);
    await machine.readl(DIGCTL_BASE + 0x000);
    assert.equal(
      await machine.readl(DIGCTL_BASE + 0x000),
      0x20000014,
      'DIGCTL CTRL.TRAP_IRQ must set when an out-of-range AHB access is trapped and TRAP_IN_RANGE=0',
    );
    assert.equal(
      await machine.readl(ICOLL_BASE + 0x050),
      trapIrqMask,
      'ICOLL source 47 must assert on out-of-range trap',
    );

    /* W1C clear TRAP_IRQ via base write while preserving TRAP_ENABLE/TRAP_IN_RANGE/USB_CLKGATE */
    await machine.writel(DIGCTL_BASE + 0x000, 0x20000034);
    assert.equal(
      await machine.readl(DIGCTL_BASE + 0x000),
      0x00000034,
      'DIGCTL CTRL.TRAP_IRQ must be W1C-clearable via base write',
    );
    assert.equal(
      await machine.readl(ICOLL_BASE + 0x050),
      0,
      'ICOLL source 47 must deassert when TRAP_IRQ is W1C-cleared',
    );

    /* Disable trap and confirm no more triggers */
    await machine.writel(DIGCTL_BASE + 0x000, 0x00000004);
    await machine.readl(DIGCTL_BASE + 0x010);
    assert.equal(
      await machine.readl(DIGCTL_BASE + 0x000),
      0x00000004,
      'DIGCTL CTRL.TRAP_IRQ must not set when TRAP_ENABLE is disabled',
    );
    assert.equal(
      await machine.readl(ICOLL_BASE + 0x050),
      0,
      'ICOLL source 47 must stay deasserted when TRAP_ENABLE is disabled',
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

async function testClkctrlGatedDividerContract() {
  await withMachine(async (machine) => {
    const dividerRegs = [
      ['PIX', CLKCTRL_BASE + 0x060],
      ['SSP', CLKCTRL_BASE + 0x070],
      ['GPMI', CLKCTRL_BASE + 0x080],
    ];

    for (const [name, addr] of dividerRegs) {
      const reset = await machine.readl(addr);
      assert.equal(reset, 0x80000001, `CLKCTRL ${name} should reset gated with DIV=1: got 0x${reset.toString(16)}`);

      await machine.writel(addr, 0x80000028);
      const whileGated = await machine.readl(addr);
      assert.equal(
        whileGated,
        0x80000001,
        `CLKCTRL ${name} should ignore DIV writes while CLKGATE=1: got 0x${whileGated.toString(16)}`,
      );

      await machine.writel(addr, 0x00000028);
      const ungatedNoRetune = await machine.readl(addr);
      assert.equal(
        ungatedNoRetune,
        0x00000001,
        `CLKCTRL ${name} should not retune DIV in the same write that ungates the clock: got 0x${ungatedNoRetune.toString(16)}`,
      );

      await machine.writel(addr, 0x00000028);
      await machine.readl(addr);
      const retuned = await machine.readl(addr);
      assert.equal(
        retuned,
        0x00000028,
        `CLKCTRL ${name} should accept a new DIV only after the clock is already ungated: got 0x${retuned.toString(16)}`,
      );
    }
  });
}

async function testClkctrlWritableFieldMasks() {
  await withMachine(async (machine) => {
    await machine.writel(CLKCTRL_BASE + 0x000, 0xffffffff);
    await machine.writel(CLKCTRL_BASE + 0x020, 0xffffffff);
    await machine.writel(CLKCTRL_BASE + 0x030, 0xffffffff);
    await machine.writel(CLKCTRL_BASE + 0x040, 0xffffffff);
    await machine.writel(CLKCTRL_BASE + 0x050, 0xffffffff);
    await machine.writel(CLKCTRL_BASE + 0x090, 0xffffffff);
    await machine.writel(CLKCTRL_BASE + 0x0d0, 0xe3e3ffe3);
    await machine.writel(CLKCTRL_BASE + 0x0e0, 0xffffffff);

    const pllctrl0 = await machine.readl(CLKCTRL_BASE + 0x000);
    await machine.readl(CLKCTRL_BASE + 0x020);
    const cpu = await machine.readl(CLKCTRL_BASE + 0x020);
    await machine.readl(CLKCTRL_BASE + 0x030);
    const hbus = await machine.readl(CLKCTRL_BASE + 0x030);
    await machine.readl(CLKCTRL_BASE + 0x040);
    const xbus = await machine.readl(CLKCTRL_BASE + 0x040);
    const xtal = await machine.readl(CLKCTRL_BASE + 0x050);
    const spdif = await machine.readl(CLKCTRL_BASE + 0x090);
    const frac = await machine.readl(CLKCTRL_BASE + 0x0d0);
    const clkseq = await machine.readl(CLKCTRL_BASE + 0x0e0);

    assert.equal(pllctrl0, 0x33350000, `CLKCTRL PLLCTRL0 should only expose documented writable fields: got 0x${pllctrl0.toString(16)}`);
    assert.equal(cpu, 0x07ff17ff, `CLKCTRL CPU should ignore busy/reserved bits on write: got 0x${cpu.toString(16)}`);
    assert.equal(hbus, 0x07f7003f, `CLKCTRL HBUS should ignore reserved/busy bits on write: got 0x${hbus.toString(16)}`);
    assert.equal(xbus, 0x000007ff, `CLKCTRL XBUS should only expose DIV_FRAC_EN/DIV: got 0x${xbus.toString(16)}`);
    assert.equal(xtal, 0xfc000001, `CLKCTRL XTAL should keep DIV_UART fixed at 1 and ignore reserved bits: got 0x${xtal.toString(16)}`);
    assert.equal(spdif, 0x80000000, `CLKCTRL SPDIF should only expose CLKGATE: got 0x${spdif.toString(16)}`);
    assert.equal(frac, 0xe3e300e3, `CLKCTRL FRAC should ignore software writes to STABLE/reserved bits while preserving stable toggles from divider changes: got 0x${frac.toString(16)}`);
    assert.equal(clkseq, 0x000000ba, `CLKCTRL CLKSEQ should keep BYPASS_SAIF cleared after software writes: got 0x${clkseq.toString(16)}`);
  });
}

async function testClkctrlFracStableContract() {
  await withMachine(async (machine) => {
    let frac = await machine.readl(CLKCTRL_BASE + 0x0d0);
    assert.equal((frac >> 30) & 1, 0, `CLKCTRL FRAC IO_STABLE should reset low: got 0x${frac.toString(16)}`);
    assert.equal((frac >> 22) & 1, 0, `CLKCTRL FRAC PIX_STABLE should reset low: got 0x${frac.toString(16)}`);
    assert.equal((frac >> 6) & 1, 0, `CLKCTRL FRAC CPU_STABLE should reset low: got 0x${frac.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x0d0, 0x92920093);
    let updated = await machine.readl(CLKCTRL_BASE + 0x0d0);
    assert.equal(updated & 0x3f, 0x13, `CLKCTRL FRAC CPUFRAC should accept the new divider: got 0x${updated.toString(16)}`);
    assert.equal((updated >> 6) & 1, 1, `CLKCTRL FRAC CPU_STABLE should invert when CPUFRAC changes: got 0x${updated.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x0d0, 0x92930093);
    frac = await machine.readl(CLKCTRL_BASE + 0x0d0);
    assert.equal((frac >> 16) & 0x3f, 0x13, `CLKCTRL FRAC PIXFRAC should accept the new divider: got 0x${frac.toString(16)}`);
    assert.equal((frac >> 22) & 1, 1, `CLKCTRL FRAC PIX_STABLE should invert when PIXFRAC changes: got 0x${frac.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x0d0, 0x93930093);
    frac = await machine.readl(CLKCTRL_BASE + 0x0d0);
    assert.equal((frac >> 24) & 0x3f, 0x13, `CLKCTRL FRAC IOFRAC should accept the new divider: got 0x${frac.toString(16)}`);
    assert.equal((frac >> 30) & 1, 1, `CLKCTRL FRAC IO_STABLE should invert when IOFRAC changes: got 0x${frac.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x0d8, 0x00800080);
    updated = await machine.readl(CLKCTRL_BASE + 0x0d0);
    assert.equal(updated & 0x00800080, 0, `CLKCTRL FRAC gate clear should ungate PIX/CPU clocks: got 0x${updated.toString(16)}`);
    assert.equal((updated >> 22) & 1, 1, `CLKCTRL FRAC PIX_STABLE should not invert on CLKGATE changes alone: got 0x${updated.toString(16)}`);
    assert.equal((updated >> 6) & 1, 1, `CLKCTRL FRAC CPU_STABLE should not invert on CLKGATE changes alone: got 0x${updated.toString(16)}`);
  });
}

async function testClkctrlPllctrl1ReservedContract() {
  await withMachine(async (machine) => {
    const reset = await machine.readl(CLKCTRL_BASE + 0x010);
    assert.equal(reset, 0x00000000, `CLKCTRL PLLCTRL1 should reset to 0: got 0x${reset.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x000, 0x00010000);
    const afterPllPowerOn = await machine.readl(CLKCTRL_BASE + 0x010);
    assert.equal(
      afterPllPowerOn,
      0x00000000,
      `CLKCTRL PLLCTRL1 LOCK/LOCK_COUNT are reserved and should stay 0 after PLL power-on: got 0x${afterPllPowerOn.toString(16)}`,
    );

    await machine.writel(CLKCTRL_BASE + 0x010, 0xffffffff);
    const afterWrite = await machine.readl(CLKCTRL_BASE + 0x010);
    assert.equal(
      afterWrite,
      0x40000000,
      `CLKCTRL PLLCTRL1 should only expose the documented FORCE_LOCK writable bit: got 0x${afterWrite.toString(16)}`,
    );
  });
}

async function testClkctrlResetSelfClears() {
  await withMachine(async (machine) => {
    await machine.writel(POWER_BASE + 0x0e0, 0x3e770001);
    const powerResetBeforeDig = await machine.readl(POWER_BASE + 0x0e0);
    assert.equal(
      powerResetBeforeDig,
      0x00000001,
      `POWER RESET should accept the unlocked write before DIG reset: got 0x${powerResetBeforeDig.toString(16)}`,
    );

    await machine.writel(CLKCTRL_BASE + 0x0f0, 0x00000001);
    let reset = await machine.readl(CLKCTRL_BASE + 0x0f0);
    assert.equal(
      reset,
      0x00000000,
      `CLKCTRL RESET.DIG should self-clear after the reset cycle completes: got 0x${reset.toString(16)}`,
    );
    const powerResetAfterDig = await machine.readl(POWER_BASE + 0x0e0);
    assert.equal(
      powerResetAfterDig,
      0x00000001,
      `CLKCTRL RESET.DIG should not reset the POWER module state: got 0x${powerResetAfterDig.toString(16)}`,
    );

    await machine.writel(CLKCTRL_BASE + 0x0f0, 0x00000002);
    reset = await machine.readl(CLKCTRL_BASE + 0x0f0);
    assert.equal(
      reset,
      0x00000000,
      `CLKCTRL RESET.CHIP should self-clear after the reset cycle completes: got 0x${reset.toString(16)}`,
    );
  });
}

async function testClkctrlDividerRangeContract() {
    await withMachine(async (machine) => {
      await machine.writel(CLKCTRL_BASE + 0x020, 0x00030002);
    await machine.readl(CLKCTRL_BASE + 0x020);
    let cpu = await machine.readl(CLKCTRL_BASE + 0x020);
    assert.equal(cpu, 0x00030002, `CLKCTRL CPU should accept valid DIV_XTAL/DIV_CPU values: got 0x${cpu.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x020, 0x00000002);
    cpu = await machine.readl(CLKCTRL_BASE + 0x020);
    assert.equal(cpu, 0x00030002, `CLKCTRL CPU should reject DIV_XTAL=0 and preserve the previous valid divider: got 0x${cpu.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x020, 0x00030000);
    cpu = await machine.readl(CLKCTRL_BASE + 0x020);
    assert.equal(cpu, 0x00030002, `CLKCTRL CPU should reject DIV_CPU=0 and preserve the previous valid divider: got 0x${cpu.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x030, 0x00000002);
    await machine.readl(CLKCTRL_BASE + 0x030);
    let hbus = await machine.readl(CLKCTRL_BASE + 0x030);
    assert.equal(hbus, 0x00000002, `CLKCTRL HBUS should accept a valid divider: got 0x${hbus.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x030, 0x00000000);
    hbus = await machine.readl(CLKCTRL_BASE + 0x030);
    assert.equal(hbus, 0x00000002, `CLKCTRL HBUS should reject DIV=0 and preserve the previous valid divider: got 0x${hbus.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x040, 0x00000004);
    await machine.readl(CLKCTRL_BASE + 0x040);
    let xbus = await machine.readl(CLKCTRL_BASE + 0x040);
    assert.equal(xbus, 0x00000004, `CLKCTRL XBUS should accept a valid divider: got 0x${xbus.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x040, 0x00000000);
    xbus = await machine.readl(CLKCTRL_BASE + 0x040);
    assert.equal(xbus, 0x00000004, `CLKCTRL XBUS should reject DIV=0 and preserve the previous valid divider: got 0x${xbus.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x060, 0x00000028);
    await machine.writel(CLKCTRL_BASE + 0x060, 0x00000028);
    await machine.readl(CLKCTRL_BASE + 0x060);
    let pix = await machine.readl(CLKCTRL_BASE + 0x060);
    assert.equal(pix, 0x00000028, `CLKCTRL PIX should accept a valid divider once ungated: got 0x${pix.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x060, 0x00000000);
    pix = await machine.readl(CLKCTRL_BASE + 0x060);
    assert.equal(pix, 0x00000028, `CLKCTRL PIX should reject DIV=0 and preserve the previous valid divider: got 0x${pix.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x060, 0x00000123);
    pix = await machine.readl(CLKCTRL_BASE + 0x060);
    assert.equal(pix, 0x00000028, `CLKCTRL PIX should reject DIV values above 255: got 0x${pix.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x070, 0x00000028);
    await machine.writel(CLKCTRL_BASE + 0x070, 0x00000028);
    await machine.readl(CLKCTRL_BASE + 0x070);
    let ssp = await machine.readl(CLKCTRL_BASE + 0x070);
    assert.equal(ssp, 0x00000028, `CLKCTRL SSP should accept a valid divider once ungated: got 0x${ssp.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x070, 0x00000000);
    ssp = await machine.readl(CLKCTRL_BASE + 0x070);
    assert.equal(ssp, 0x00000028, `CLKCTRL SSP should reject DIV=0 and preserve the previous valid divider: got 0x${ssp.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x080, 0x00000028);
    await machine.writel(CLKCTRL_BASE + 0x080, 0x00000028);
    await machine.readl(CLKCTRL_BASE + 0x080);
    let gpmi = await machine.readl(CLKCTRL_BASE + 0x080);
    assert.equal(gpmi, 0x00000028, `CLKCTRL GPMI should accept a valid divider once ungated: got 0x${gpmi.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x080, 0x00000000);
    gpmi = await machine.readl(CLKCTRL_BASE + 0x080);
    assert.equal(gpmi, 0x00000028, `CLKCTRL GPMI should reject DIV=0 and preserve the previous valid divider: got 0x${gpmi.toString(16)}`);
  });
}

async function testClkctrlBusyContract() {
  await withMachine(async (machine) => {
    const cpuReset = await machine.readl(CLKCTRL_BASE + 0x020);
    assert.equal(cpuReset & 0x30000000, 0, `CLKCTRL CPU busy bits should reset low: got 0x${cpuReset.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x020, 0x00010002);
    let cpu = await machine.readl(CLKCTRL_BASE + 0x020);
    assert.equal((cpu >>> 28) & 1, 1, `CLKCTRL CPU should raise BUSY_REF_CPU when DIV_CPU changes: got 0x${cpu.toString(16)}`);
    cpu = await machine.readl(CLKCTRL_BASE + 0x020);
    assert.equal((cpu >>> 28) & 1, 0, `CLKCTRL CPU BUSY_REF_CPU should clear after the transfer completes: got 0x${cpu.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x020, 0x00020002);
    cpu = await machine.readl(CLKCTRL_BASE + 0x020);
    assert.equal((cpu >>> 29) & 1, 1, `CLKCTRL CPU should raise BUSY_REF_XTAL when DIV_XTAL changes: got 0x${cpu.toString(16)}`);
    cpu = await machine.readl(CLKCTRL_BASE + 0x020);
    assert.equal((cpu >>> 29) & 1, 0, `CLKCTRL CPU BUSY_REF_XTAL should clear after the transfer completes: got 0x${cpu.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x030, 0x00000002);
    let hbus = await machine.readl(CLKCTRL_BASE + 0x030);
    assert.equal((hbus >>> 29) & 1, 1, `CLKCTRL HBUS should raise BUSY when DIV changes: got 0x${hbus.toString(16)}`);
    hbus = await machine.readl(CLKCTRL_BASE + 0x030);
    assert.equal((hbus >>> 29) & 1, 0, `CLKCTRL HBUS BUSY should clear after the transfer completes: got 0x${hbus.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x040, 0x00000004);
    let xbus = await machine.readl(CLKCTRL_BASE + 0x040);
    assert.equal((xbus >>> 31) & 1, 1, `CLKCTRL XBUS should raise BUSY when DIV changes: got 0x${xbus.toString(16)}`);
    xbus = await machine.readl(CLKCTRL_BASE + 0x040);
    assert.equal((xbus >>> 31) & 1, 0, `CLKCTRL XBUS BUSY should clear after the transfer completes: got 0x${xbus.toString(16)}`);

    for (const [name, addr] of [
      ['PIX', CLKCTRL_BASE + 0x060],
      ['SSP', CLKCTRL_BASE + 0x070],
      ['GPMI', CLKCTRL_BASE + 0x080],
    ]) {
      await machine.writel(addr, 0x00000028);
      await machine.writel(addr, 0x00000028);
      let reg = await machine.readl(addr);
      assert.equal((reg >>> 29) & 1, 1, `CLKCTRL ${name} should raise BUSY when DIV changes while ungated: got 0x${reg.toString(16)}`);
      reg = await machine.readl(addr);
      assert.equal((reg >>> 29) & 1, 0, `CLKCTRL ${name} BUSY should clear after the transfer completes: got 0x${reg.toString(16)}`);
    }
  });
}

async function testClkctrlFracRangeContract() {
  await withMachine(async (machine) => {
    let frac = await machine.readl(CLKCTRL_BASE + 0x0d0);
    assert.equal(frac, 0x92920092, `CLKCTRL FRAC should reset to the documented 0x12 dividers: got 0x${frac.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x0d0, 0x92920011);
    frac = await machine.readl(CLKCTRL_BASE + 0x0d0);
    assert.equal(frac & 0x3f, 0x12, `CLKCTRL FRAC should reject CPUFRAC values below 18: got 0x${frac.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x0d0, 0x92920023);
    frac = await machine.readl(CLKCTRL_BASE + 0x0d0);
    assert.equal(frac & 0x3f, 0x23, `CLKCTRL FRAC should accept CPUFRAC values within 18..35: got 0x${frac.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x0d0, 0x92920024);
    frac = await machine.readl(CLKCTRL_BASE + 0x0d0);
    assert.equal(frac & 0x3f, 0x23, `CLKCTRL FRAC should reject CPUFRAC values above 35 and preserve the previous valid value: got 0x${frac.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x0d0, 0x92120024);
    frac = await machine.readl(CLKCTRL_BASE + 0x0d0);
    assert.equal((frac >> 16) & 0x3f, 0x12, `CLKCTRL FRAC should reject PIXFRAC values below 18: got 0x${frac.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x0d0, 0x92230023);
    frac = await machine.readl(CLKCTRL_BASE + 0x0d0);
    assert.equal((frac >> 16) & 0x3f, 0x23, `CLKCTRL FRAC should accept PIXFRAC values within 18..35: got 0x${frac.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x0d0, 0x92240023);
    frac = await machine.readl(CLKCTRL_BASE + 0x0d0);
    assert.equal((frac >> 16) & 0x3f, 0x23, `CLKCTRL FRAC should reject PIXFRAC values above 35 and preserve the previous valid value: got 0x${frac.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x0d0, 0x11240024);
    frac = await machine.readl(CLKCTRL_BASE + 0x0d0);
    assert.equal((frac >> 24) & 0x3f, 0x12, `CLKCTRL FRAC should reject IOFRAC values below 18: got 0x${frac.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x0d0, 0x23230023);
    frac = await machine.readl(CLKCTRL_BASE + 0x0d0);
    assert.equal((frac >> 24) & 0x3f, 0x23, `CLKCTRL FRAC should accept IOFRAC values within 18..35: got 0x${frac.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x0d0, 0x24230023);
    frac = await machine.readl(CLKCTRL_BASE + 0x0d0);
    assert.equal((frac >> 24) & 0x3f, 0x23, `CLKCTRL FRAC should reject IOFRAC values above 35 and preserve the previous valid value: got 0x${frac.toString(16)}`);
  });
}

async function testClkctrlClkseqGateContract() {
  await withMachine(async (machine) => {
    const clkseqReset = await machine.readl(CLKCTRL_BASE + 0x0e0);
    assert.equal(clkseqReset, 0x000000bb, `CLKCTRL CLKSEQ should reset with all documented bypass bits set: got 0x${clkseqReset.toString(16)}`);

    await machine.writel(CLKCTRL_BASE + 0x0e0, 0x0000003b);
    let clkseq = await machine.readl(CLKCTRL_BASE + 0x0e0);
    assert.equal(
      clkseq,
      0x000000ba,
      `CLKCTRL CLKSEQ should ignore CPU bypass switching while FRAC.CLKGATECPU keeps ref_cpu gated: got 0x${clkseq.toString(16)}`,
    );

    await machine.writel(CLKCTRL_BASE + 0x0d8, 0x00000080);
    await machine.writel(CLKCTRL_BASE + 0x0e0, 0x0000003a);
    clkseq = await machine.readl(CLKCTRL_BASE + 0x0e0);
    assert.equal(
      clkseq,
      0x0000003a,
      `CLKCTRL CLKSEQ should allow CPU bypass switching once ref_cpu is ungated: got 0x${clkseq.toString(16)}`,
    );

    await machine.writel(CLKCTRL_BASE + 0x0d4, 0x00000080);
    await machine.writel(CLKCTRL_BASE + 0x0e0, 0x000000ba);
    clkseq = await machine.readl(CLKCTRL_BASE + 0x0e0);
    assert.equal(
      clkseq,
      0x0000003a,
      `CLKCTRL CLKSEQ should ignore CPU bypass switching back to XTAL while FRAC.CLKGATECPU is asserted: got 0x${clkseq.toString(16)}`,
    );
  });

  await withMachine(async (machine) => {
    await machine.writel(CLKCTRL_BASE + 0x0e0, 0x000000b3);
    let clkseq = await machine.readl(CLKCTRL_BASE + 0x0e0);
    assert.equal(
      clkseq,
      0x000000ba,
      `CLKCTRL CLKSEQ should ignore IR bypass switching while FRAC.CLKGATEIO keeps ref_io gated: got 0x${clkseq.toString(16)}`,
    );

    await machine.writel(CLKCTRL_BASE + 0x0d8, 0x80000000);
    await machine.writel(CLKCTRL_BASE + 0x0e0, 0x000000b2);
    clkseq = await machine.readl(CLKCTRL_BASE + 0x0e0);
    assert.equal(
      clkseq,
      0x000000b2,
      `CLKCTRL CLKSEQ should allow IR bypass switching once ref_io is ungated: got 0x${clkseq.toString(16)}`,
    );

    await machine.writel(CLKCTRL_BASE + 0x0e0, 0x00000092);
    clkseq = await machine.readl(CLKCTRL_BASE + 0x0e0);
    assert.equal(
      clkseq,
      0x000000b2,
      `CLKCTRL CLKSEQ should ignore SSP bypass switching while SSP.CLKGATE is still asserted: got 0x${clkseq.toString(16)}`,
    );

    await machine.writel(CLKCTRL_BASE + 0x070, 0x00000028);
    await machine.writel(CLKCTRL_BASE + 0x0e0, 0x00000092);
    clkseq = await machine.readl(CLKCTRL_BASE + 0x0e0);
    assert.equal(
      clkseq,
      0x00000092,
      `CLKCTRL CLKSEQ should allow SSP bypass switching once ref_io and SSP are both ungated: got 0x${clkseq.toString(16)}`,
    );

    await machine.writel(CLKCTRL_BASE + 0x0e0, 0x00000082);
    clkseq = await machine.readl(CLKCTRL_BASE + 0x0e0);
    assert.equal(
      clkseq,
      0x00000092,
      `CLKCTRL CLKSEQ should ignore GPMI bypass switching while GPMI.CLKGATE is still asserted: got 0x${clkseq.toString(16)}`,
    );

    await machine.writel(CLKCTRL_BASE + 0x080, 0x00000028);
    await machine.writel(CLKCTRL_BASE + 0x0e0, 0x00000082);
    clkseq = await machine.readl(CLKCTRL_BASE + 0x0e0);
    assert.equal(
      clkseq,
      0x00000082,
      `CLKCTRL CLKSEQ should allow GPMI bypass switching once ref_io and GPMI are both ungated: got 0x${clkseq.toString(16)}`,
    );
  });

  await withMachine(async (machine) => {
    await machine.writel(CLKCTRL_BASE + 0x0e0, 0x000000b9);
    let clkseq = await machine.readl(CLKCTRL_BASE + 0x0e0);
    assert.equal(
      clkseq,
      0x000000ba,
      `CLKCTRL CLKSEQ should ignore PIX bypass switching while FRAC.CLKGATEPIX keeps ref_pix gated: got 0x${clkseq.toString(16)}`,
    );

    await machine.writel(CLKCTRL_BASE + 0x0d8, 0x00800000);
    await machine.writel(CLKCTRL_BASE + 0x0e0, 0x000000b8);
    clkseq = await machine.readl(CLKCTRL_BASE + 0x0e0);
    assert.equal(
      clkseq,
      0x000000ba,
      `CLKCTRL CLKSEQ should ignore PIX bypass switching while PIX.CLKGATE is still asserted: got 0x${clkseq.toString(16)}`,
    );

    await machine.writel(CLKCTRL_BASE + 0x060, 0x00000028);
    await machine.writel(CLKCTRL_BASE + 0x0e0, 0x000000b8);
    clkseq = await machine.readl(CLKCTRL_BASE + 0x0e0);
    assert.equal(
      clkseq,
      0x000000b8,
      `CLKCTRL CLKSEQ should allow PIX bypass switching once ref_pix and PIX are both ungated: got 0x${clkseq.toString(16)}`,
    );
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
  ['RTC reset and persistent0 contract', testRtcResetAndPersistent0Contract],
  ['RTC copy controller contract', testRtcCopyControllerContract],
  ['RTC clock gate contract', testRtcClockGateContract],
  ['RTC watchdog debug contract', testRtcWatchdogDebugContract],
  ['RTC alarm wake contract', testRtcAlarmWakeContract],
  ['RTC suppress copy-to-analog contract', testRtcSuppressCopyToAnalogContract],
  ['RTC analog state survives chip reset', testRtcAnalogStateSurvivesChipReset],
  ['RTC PERSISTENT1 write mask contract', testRtcPersistent1WriteMaskContract],
  ['RTC analog seconds run while digital clock gated', testRtcAnalogSecondsRunWhileDigitalClockGated],
  ['RTC millisecond resolution contract', testRtcMsecResolutionContract],
  ['TIMROT tick and update contract', testTimrotTickAndUpdateContract],
  ['TIMROT external edge contract', testTimrotExternalEdgeContract],
  ['TIMROT duty-cycle contract', testTimrotDutyCycleContract],
  ['TIMROT rotary contract', testTimrotRotaryContract],
  ['TIMROT rotary invalid transition contract', testTimrotRotaryInvalidTransitionContract],
  ['PWM register contract', testPwmRegisterContract],
  ['PWM waveform contract', testPwmWaveformContract],
  ['PWM MATT contract', testPwmMattContract],
  ['PWM2 analog enable contract', testPwm2AnalogEnableContract],
  ['LRADC register contract', testLradcRegisterContract],
  ['LRADC IRQ contract', testLradcIrqContract],
  ['LRADC scheduler contract', testLradcSchedulerContract],
  ['LRADC touch/temperature contract', testLradcTouchTemperatureContract],
  ['LRADC divide-by-two contract', testLradcDivideByTwoContract],
  ['LRADC temperature current source contract', testLradcTempCurrentContract],
  ['LRADC CTRL3 power and discard contract', testLradcCtrl3PowerAndDiscardContract],
  ['LRADC STATUS and CTRL3 clock contract', testLradcStatusAndCtrl3ClockContract],
  ['I2C register contract', testI2cRegisterContract],
  ['I2C DMA IRQ ownership contract', testI2cDmaIrqOwnershipContract],
  ['Application UART register contract', testAppUartRegisterContract],
  ['Debug UART register contract', testDebugUartRegisterContract],
  ['LCDIF CTRL1 interrupt layout', testLcdifCtrl1Layout],
  ['LCDIF register map contract', testLcdifRegisterMapContract],
  ['LCDIF clock gate contract', testLcdifClockGateContract],
  ['LCDIF soft reset contract', testLcdifSoftResetContract],
  ['LCDIF byte packing contract', testLcdifBytePackingContract],
  ['LCDIF data swizzle contract', testLcdifDataSwizzleContract],
  ['LCDIF data shift contract', testLcdifDataShiftContract],
  ['LCDIF idle-only control contract', testLcdifIdleOnlyControlContract],
  ['LCDIF FIFO status contract', testLcdifFifoStatusContract],
  ['LCDIF streaming end contract', testLcdifStreamingEndContract],
  ['LCDIF first read dummy contract', testLcdifFirstReadDummyContract],
  ['USBPHY register contract', testUsbPhyRegisterContract],
  ['USBCTRL capability register contract', testUsbCapabilityRegisterContract],
  ['USBCTRL device control contract', testUsbDeviceControlContract],
  ['USBCTRL endpoint register contract', testUsbEndpointRegisterContract],
  ['USBCTRL GPTIMER contract', testUsbGptimerContract],
  ['SSP register layout and reset contract', testSspRegisterLayoutAndResetContract],
  ['SSP soft reset and clock gate contract', testSspSoftResetAndClockGateContract],
  ['SSP soft reset hold contract', testSspSoftResetHoldContract],
  ['SSP CTRL1 writable mask contract', testSspCtrl1WritableMaskContract],
  ['SSP SCT and CMD0 reserved contract', testSspSctAndCmd0ReservedContract],
  ['SSP error IRQ pairing contract', testSspErrorIrqPairingContract],
  ['SSP DATA empty read contract', testSspDataEmptyReadContract],
  ['GPMI TIMING2 contract', testGpmiTiming2Contract],
  ['GPMI CTRL1 contract', testGpmiCtrl1Contract],
  ['GPMI ECC register contract', testGpmiEccRegisterContract],
  ['GPMI compare and DMA sense contract', testGpmiCompareSenseContract],
  ['GPMI WAIT_FOR_READY contract', testGpmiWaitForReadyContract],
  ['ECC8 completion result contract', testEcc8CompletionResultContract],
  ['ECC8 register contract', testEcc8RegisterContract],
  ['GPMI DATA FIFO contract', testGpmiDataFifoContract],
  ['APBX DMA 64 KiB and AHB error contract', testApbxDma64KAndAhbErrorContract],
  ['DMA CTRL1 and DEVSEL writable mask contract', testDmaCtrl1AndDevselContract],
  ['on-chip ROM and SRAM mirror contract', testOnChipRomAndSramMirrorContract],
  ['DCP register and memcopy contract', testDcpRegisterAndMemcopyContract],
  ['DCP channel register map contract', testDcpChannelRegisterMapContract],
  ['DCP key and context register contract', testDcpKeyAndContextRegisterContract],
  ['DCP CSC register map contract', testDcpCscRegisterMapContract],
  ['LCDIF data access contract', testLcdifDataAccessContract],
  ['PINCTRL CTRL contract', testPinctrlCtrl],
  ['PINCTRL Bank 3 absence', testPinctrlBank3Absent],
  ['PINCTRL drive and pull masks', testPinctrlDriveAndPullMasks],
  ['PINCTRL MUXSEL default and reserved mask', testPinctrlMuxselDefaultAndMask],
  ['PINCTRL GPIO IRQSTAT edge/level contract', testPinctrlGpioIrqstatContract],
  ['ICOLL core contract', testIcollCoreContract],
  ['ICOLL same-level priority contract', testIcollSameLevelPriorityContract],
  ['ICOLL BYPASS same-level priority contract', testIcollBypassSameLevelPriorityContract],
  ['ICOLL vector acknowledge contract', testIcollVectorAcknowledgeContract],
  ['ICOLL ARM_RSE mode contract', testIcollArmRseModeContract],
  ['ICOLL no nesting contract', testIcollNoNestingContract],
  ['ICOLL debug flag contract', testIcollDebugFlagContract],
  ['ICOLL debug state contract', testIcollDebugStateContract],
  ['ICOLL soft reset contract', testIcollSoftResetContract],
  ['ICOLL bypass FSM contract', testIcollBypassFsmContract],
  ['ICOLL request holding contract', testIcollRequestHoldingContract],
  ['POWER version and reset contract', testPowerVersionAndResetContract],
  ['POWER reset values', testPowerResetValues],
  ['DFLPT PTE_2048 contract', testDflptPte2048Contract],
  ['DFLPT MPTE locator remap', testDflptMpteTracksLocator],
  ['DIGCTL writable field masks', testDigctlWritableFieldMasks],
  ['DIGCTL scratch and microseconds contract', testDigctlScratchAndMicrosecondsContract],
  ['DIGCTL undocumented alias decode', testDigctlUndocumentedAliasDecode],
  ['DIGCTL ctrl behavior contract', testDigctlCtrlBehaviorContract],
  ['DIGCTL writeonce resets with dig reset', testDigctlWriteonceResetsWithDigReset],
  ['DIGCTL HCLK counter contract', testDigctlHclkCountContract],
  ['DIGCTL read-only status contract', testDigctlReadOnlyStatusContract],
  ['DIGCTL DCP BIST status contract', testDigctlDcpBistStatusContract],
  ['DIGCTL trap contract', testDigctlTrapContract],
  ['CLKCTRL reset contract', testClkctrlResetContract],
  ['CLKCTRL gated divider contract', testClkctrlGatedDividerContract],
  ['CLKCTRL writable field masks', testClkctrlWritableFieldMasks],
  ['CLKCTRL FRAC stable contract', testClkctrlFracStableContract],
  ['CLKCTRL PLLCTRL1 reserved contract', testClkctrlPllctrl1ReservedContract],
  ['CLKCTRL reset self-clear contract', testClkctrlResetSelfClears],
  ['CLKCTRL divider range contract', testClkctrlDividerRangeContract],
  ['CLKCTRL busy contract', testClkctrlBusyContract],
  ['CLKCTRL FRAC range contract', testClkctrlFracRangeContract],
  ['CLKCTRL CLKSEQ gate contract', testClkctrlClkseqGateContract],
  ['OCOTP bank-open contract', testOcotpBankOpenContract],
  ['OCOTP lock and shadow contract', testOcotpLockAndShadowContract],
];

const filter = process.env.EMUGII_QTEST_FILTER;
const selectedTests = filter
  ? tests.filter(([name]) => name.includes(filter))
  : tests;

if (filter && selectedTests.length === 0) {
  console.error(`FAIL No qtest contract matched filter: ${filter}`);
  process.exit(1);
}

let failures = 0;

for (const [name, fn] of selectedTests) {
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
