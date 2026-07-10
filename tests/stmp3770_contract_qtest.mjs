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
const PWM_BASE = 0x80064000;

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

    await machine.writel(PWM_BASE + 0x000, 0xffffffff);
    assert.equal(
      await machine.readl(PWM_BASE + 0x000),
      0xfe00003f,
      'PWM CTRL must preserve present bits and only store documented control bits',
    );
    await machine.writel(PWM_BASE + 0x008, 0xff000000);
    assert.equal(
      await machine.readl(PWM_BASE + 0x000),
      0x3e00003f,
      'PWM CTRL_CLR must only clear writable reset and gate bits, not present bits',
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
  ['PWM register contract', testPwmRegisterContract],
  ['LCDIF CTRL1 interrupt layout', testLcdifCtrl1Layout],
  ['PINCTRL Bank 3 absence', testPinctrlBank3Absent],
  ['ICOLL core contract', testIcollCoreContract],
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
