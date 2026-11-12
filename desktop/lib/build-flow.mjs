// The "Build ByOS and install it" flow of the Updates page, as plain data: which of the four
// steps is where, from the byos-cli lines (docs/BYOS_CLI.md, "Build ByOS"). No DOM here, so the
// runner's tests and the page use the same code (served to the page as /build-flow.js).
//
//   firmware  firmware-fetch          op "download" (bytes), op "check" (permille)
//   build     build                   op "patch" (permille, "stage"), op "export" (bytes)
//   send      build-send --install    op "hash", op "put" (bytes per file)
//   install   build-send --install    op "reboot", op "reconnect" (no numbers: a wait)

export const STEPS = Object.freeze(['firmware', 'build', 'send', 'install']);

// The patcher's stages in order (core Stage enum), then the ByOS folder export.
export const BUILD_PHASES = Object.freeze(['check', 'read', 'apply', 'build', 'verify_rootfs', 'write', 'verify_upt', 'export']);

function step(status = 'todo') {
  return { status, phase: null, done: 0, total: 0, unit: null, detail: null };
}

export function emptyFlow() {
  return {
    phase: 'idle', // idle | running | done | failed
    steps: Object.fromEntries(STEPS.map((s) => [s, step()])),
    buildSize: 0, // size of the patched firmware
    sendTotal: 0, // bytes build-send copies (ByOS folder + firmware), from the build result
    sendFiles: {}, // path -> bytes sent
    parked: [],
    result: null, // the final build-send result when the install finished
    error: null, // the error line that stopped the flow
    errorStep: null,
  };
}

// What is already done before anything runs, from "build-info".
export function applyInfo(flow, info) {
  if (!info || !info.ok) return flow;
  const { steps } = flow;
  if (info.firmware_ready) Object.assign(steps.firmware, { status: 'done', detail: 'cached' });
  if (info.build && info.build.hibyos_version === info.hibyos_version) {
    Object.assign(steps.build, { status: 'done', detail: 'cached' });
    flow.buildSize = info.build.size ?? 0;
    flow.sendTotal = (info.build.size ?? 0) + (info.build.sd_size ?? 0);
  }
  return flow;
}

export function startStep(flow, name) {
  flow.phase = 'running';
  flow.error = null;
  flow.errorStep = null;
  Object.assign(flow.steps[name], step('active'));
  if (name === 'send') flow.sendFiles = {};
  return flow;
}

// One progress/warn line of the job that runs `name` (for build-send: "send", which also
// moves on to "install" when the HiBy restarts).
export function applyEvent(flow, name, ev) {
  if (!ev || typeof ev !== 'object') return flow;
  if (ev.event === 'warn' && ev.code === 'parked' && typeof ev.path === 'string') {
    if (!flow.parked.includes(ev.path)) flow.parked.push(ev.path);
    return flow;
  }
  if (ev.event !== 'progress') return flow;
  const num = (v) => (Number.isFinite(v) && v >= 0 ? v : 0);
  if (name === 'firmware') {
    Object.assign(flow.steps.firmware, {
      status: 'active', phase: ev.op === 'check' ? 'check' : 'download',
      done: num(ev.done), total: num(ev.total), unit: ev.unit ?? 'bytes',
    });
  } else if (name === 'build') {
    const phase = ev.op === 'export' ? 'export' : (BUILD_PHASES.includes(ev.stage) ? ev.stage : 'check');
    Object.assign(flow.steps.build, {
      status: 'active', phase, done: num(ev.done), total: num(ev.total), unit: ev.unit ?? 'permille',
    });
  } else if (name === 'send') {
    if (ev.op === 'reboot' || ev.op === 'reconnect') {
      Object.assign(flow.steps.send, { status: 'done', phase: null });
      Object.assign(flow.steps.install, { status: 'active', phase: ev.op, done: 0, total: 0, unit: 'none' });
    } else if (ev.op === 'hash') {
      Object.assign(flow.steps.send, { status: 'active', phase: 'hash' });
    } else if (ev.op === 'put' && typeof ev.path === 'string') {
      flow.sendFiles[ev.path] = num(ev.done);
      const done = Object.values(flow.sendFiles).reduce((a, b) => a + b, 0);
      const total = Math.max(flow.sendTotal, done);
      Object.assign(flow.steps.send, { status: 'active', phase: 'put', done, total, unit: 'bytes', detail: ev.path });
    }
  }
  return flow;
}

// The final line of the job that ran `name`.
export function finishStep(flow, name, final) {
  if (final && final.ok) {
    if (name === 'firmware') {
      Object.assign(flow.steps.firmware, { status: 'done', phase: null, detail: final.cached ? 'cached' : final.source ?? 'download' });
    } else if (name === 'build') {
      Object.assign(flow.steps.build, { status: 'done', phase: null, detail: final.cached ? 'cached' : 'built' });
      flow.buildSize = final.size ?? 0;
      flow.sendTotal = (final.size ?? 0) + (final.sd_size ?? 0);
    } else if (name === 'send') {
      Object.assign(flow.steps.send, { status: 'done', phase: null, done: flow.sendTotal, total: flow.sendTotal });
      if (final.state === 'installed') {
        Object.assign(flow.steps.install, { status: 'done', phase: null });
        flow.result = final;
        flow.phase = 'done';
      } else if (final.state === 'rebooting') {
        Object.assign(flow.steps.install, { status: 'active', phase: 'reboot' });
      }
    }
    return flow;
  }
  // which step failed: for build-send, "install" once the HiBy was asked to restart, or when
  // the HiBy refused to start it (battery): the copy itself had finished
  const atInstall = name === 'send' && (flow.steps.install.status === 'active' || final?.btp?.name === 'E_BATTERY');
  const failed = atInstall ? 'install' : name;
  if (atInstall) Object.assign(flow.steps.send, { status: 'done', phase: null });
  flow.steps[failed].status = final?.code === 'cancelled' ? 'todo' : 'failed';
  flow.phase = 'failed';
  flow.error = final ?? { ok: false, code: 'internal', message: 'no result' };
  flow.errorStep = failed;
  return flow;
}

// 0..1 for a bar, or null when the step has no numbers (a wait).
export function fraction(s) {
  if (!s || s.status !== 'active' || !(s.total > 0)) return null;
  return Math.min(1, Math.max(0, s.done / s.total));
}

// The steps still to run, in order.
export function pending(flow) {
  return STEPS.filter((s) => flow.steps[s].status !== 'done');
}
