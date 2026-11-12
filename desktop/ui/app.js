// ByOS desktop scaffold UI. Plain ES modules, no framework. Every device operation is one
// byos-cli run through the local runner (server.mjs): POST /api/jobs, then server-sent events.
// Names from the device are only ever inserted with textContent.

import { t, setLang, getLang, detectLang, LANGS, errorMessage } from '/i18n.js';
import { checkPath } from '/btp-path.js';
import { emptyFlow, applyInfo, startStep, applyEvent, finishStep, fraction, STEPS } from '/build-flow.js';
import {
  ageText, computerOpened, demoMedia, ICONS, icon, initMedia, libraryChanged, mediaDeviceChanged, mediaLocation, mediaTabOpened,
  messageCard, readMediaLocation, renderMedia, toast,
} from '/media.js';
import { demoSync, initSync, renderSync, syncDeviceChanged, syncKind, syncTabOpened } from '/sync.js';

const TOKEN = document.querySelector('meta[name="byos-token"]').content;
const $ = (id) => document.getElementById(id);
const el = (tag, props = {}, ...children) => {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(props)) {
    if (k === 'class') n.className = v;
    else if (k === 'text') n.textContent = v;
    else if (k === 'style') n.style.cssText = v; // CSSOM: allowed by the page's CSP, unlike style=""
    else if (k.startsWith('on')) n.addEventListener(k.slice(2), v);
    else if (v !== undefined && v !== null && v !== false) n.setAttribute(k, v === true ? '' : v);
  }
  for (const c of children) if (c) n.append(c);
  return n;
};

const state = {
  info: null,
  devices: [],
  selectedId: null,
  tab: 'summary',
  // "device": a HiBy's pages; "computer": the sidebar's Library (the music on this computer)
  view: 'device',
  status: null,
  statusError: null,
  cwd: '',
  entries: [],
  selected: new Set(),
  showHidden: false,
  jobs: [],
  scanning: false,
  // Updates page: "build-info", the build-and-install steps, the job that runs now.
  upd: { info: null, flow: emptyFlow(), running: false, job: null, demo: null },
};

// ---- formatting -------------------------------------------------------------------------

function fmtBytes(n) {
  if (n === undefined || n === null) return '';
  const units = ['B', 'KB', 'MB', 'GB', 'TB'];
  let i = 0;
  let v = n;
  while (v >= 1000 && i < units.length - 1) { v /= 1000; i++; }
  const s = new Intl.NumberFormat(getLang(), { maximumFractionDigits: v < 10 && i > 0 ? 1 : 0 }).format(v);
  return `${s} ${units[i]}`;
}
function fmtDate(sec) {
  if (!sec) return '';
  return new Intl.DateTimeFormat(getLang(), { dateStyle: 'medium', timeStyle: 'short' }).format(new Date(sec * 1000));
}
function kindOf(entry) {
  if (entry.kind === 'dir') return t('folder');
  const dot = entry.name.lastIndexOf('.');
  return dot > 0 ? t('fileKind', { ext: entry.name.slice(dot + 1).toUpperCase() }) : '';
}

// ---- runner API --------------------------------------------------------------------------

async function api(path, { method = 'GET', body, raw } = {}) {
  const res = await fetch(path, {
    method,
    headers: { 'X-ByOS-Token': TOKEN, ...(raw ? {} : { 'Content-Type': 'application/json' }) },
    body: raw ?? (body === undefined ? undefined : JSON.stringify(body)),
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw Object.assign(new Error(data.error || res.statusText), { status: res.status });
  return data;
}

// Follows one job to its end. Returns the final result/error line.
function follow(id, job) {
  return new Promise((resolve) => {
    const es = new EventSource(`/api/jobs/${id}/events?token=${encodeURIComponent(TOKEN)}`);
    es.onmessage = (m) => onJobEvent(job, JSON.parse(m.data));
    es.addEventListener('end', (m) => {
      es.close();
      resolve(JSON.parse(m.data));
    });
    es.onerror = () => {
      if (es.readyState === EventSource.CLOSED) {
        resolve({ v: 1, event: 'error', ok: false, code: 'internal', message: 'Lost contact with the runner' });
      }
    };
  });
}

// Runs a byos-cli command. opts: title, quiet (not listed in Activity), onEvent, upload.
async function runCli(command, params, opts = {}) {
  const job = {
    title: opts.title ?? command, command, state: 'running', progress: null,
    final: null, quiet: !!opts.quiet, onEvent: opts.onEvent, warnings: [],
  };
  // Resolves when the job ends, so an open prompt sheet closes by itself (for example when
  // the runner gives up waiting for an answer).
  job.ended = new Promise((r) => { job.markEnded = r; });
  if (!job.quiet) state.jobs.unshift(job);
  state.jobs = state.jobs.slice(0, 30);
  renderLcd();
  try {
    const { id } = opts.upload
      ? await api(opts.upload, { method: 'POST', raw: opts.file })
      : await api('/api/jobs', { method: 'POST', body: { command, params } });
    job.id = id;
    opts.onStarted?.(job);
    job.final = await follow(id, job);
  } catch (e) {
    job.final = { v: 1, event: 'error', ok: false, code: e.status === 503 ? 'no_cli' : 'usage', message: e.message };
  }
  job.state = job.final.ok ? 'done' : 'failed';
  job.markEnded();
  renderLcd();
  renderActivity();
  return job.final;
}

function onJobEvent(job, ev) {
  switch (ev.event) {
    case 'progress':
      job.progress = ev;
      renderLcd();
      break;
    case 'prompt':
      handlePrompt(job, ev);
      break;
    case 'warn':
      job.warnings.push(ev);
      break;
    default:
      break;
  }
  job.onEvent?.(ev);
}

async function handlePrompt(job, ev) {
  let answer = null;
  if (ev.kind === 'pair_code') {
    const r = await pairCodeSheet(ev, job.ended);
    answer = r.ok ? r.code : null;
  } else if (ev.kind === 'confirm') {
    const r = await sheet({ title: t('deleteTitle'), body: ev.message, okText: t('ok'), danger: true, abort: job.ended });
    answer = r.ok;
  } else if (ev.kind === 'confirm_mass_delete') {
    // the count, typed (design 3.3.3)
    const r = await sheet({
      title: t('pvMassTitle', { n: ev.count }), body: t('pvMassBody', { n: ev.count, size: fmtBytes(ev.bytes ?? 0) }), okText: t('deleteAction'),
      danger: true, input: '', abort: job.ended,
      validate: (v) => (String(v ?? '').replace(/[\s.,\u00a0\u202f']/g, '') === String(ev.count) ? null : t('pvTypeWrong', { n: ev.count })),
    });
    answer = r.ok ? r.value : null;
  }
  if (job.final) return; // ended while the sheet was open
  if (answer === null) {
    await api(`/api/jobs/${job.id}/cancel`, { method: 'POST', body: {} }).catch(() => {});
  } else {
    await api(`/api/jobs/${job.id}/answer`, { method: 'POST', body: { answer } }).catch(() => {});
  }
}

// ---- devices ------------------------------------------------------------------------------

function deviceSpec(d) {
  if (!d) return undefined;
  if (d.transport === 'usb') return d.serial ? `usb:${d.serial}` : 'usb';
  return `wifi:${d.id}`;
}
const selectedDevice = () => state.devices.find((d) => d.id === state.selectedId) ?? null;

async function refreshDevices() {
  if (state.scanning) return;
  state.scanning = true;
  renderDeviceList();
  const found = [];
  const final = await runCli('devices', { wait: 1500 }, {
    quiet: true,
    onEvent: (ev) => { if (ev.event === 'device') found.push(ev); },
  });
  state.scanning = false;
  if (final.ok) state.devices = found;
  else if (final.code === 'no_cli') state.devices = [];
  if (state.selectedId && !state.devices.some((d) => d.id === state.selectedId)) state.selectedId = null;
  if (!state.selectedId) {
    const first = state.devices.find((d) => d.reachable !== false);
    if (first) selectDevice(first.id);
  }
  $('empty-status').textContent = final.ok ? '' : errorMessage(final);
  renderDeviceList();
  renderMain();
}

function selectDevice(id) {
  if (state.selectedId === id) return;
  state.selectedId = id;
  state.status = null;
  state.statusError = null;
  state.cwd = '';
  state.entries = [];
  state.selected.clear();
  renderDeviceList();
  renderMain();
  loadStatus();
  if (state.tab === 'files') {
    loadFolder(state.pendingPath ?? '');
    state.pendingPath = null;
  }
  if (state.tab === 'updates') loadBuildInfo();
  mediaDeviceChanged();
  syncDeviceChanged();
  if (state.view !== 'computer') {
    if (state.tab === 'library') mediaTabOpened(state.tab);
    if (state.tab === 'sync') syncTabOpened();
  }
}

// The sidebar's Library: the computer's music, whatever HiBy is selected.
function showComputer(view) {
  state.view = 'computer';
  renderDeviceList();
  renderMain();
  computerOpened(view);
  saveLocation();
}

// Back to the selected HiBy's pages (from the computer's library).
function showDevice(tab) {
  state.view = 'device';
  renderDeviceList();
  if (tab) setTab(tab);
  else { renderMain(); saveLocation(); }
}

// "Sync Music" in the computer's library: the HiBy's Sync tab, then the sync of that kind.
function syncFromComputer(kind) {
  if (!selectedDevice()) return;
  showDevice('sync');
  syncKind(kind);
}

// Switches the device page to a tab and loads what it shows.
function setTab(tab) {
  state.tab = tab;
  state.view = 'device';
  saveLocation();
  renderMain();
  if (tab === 'files' && !state.entries.length) loadFolder(state.cwd);
  if (tab === 'updates') loadBuildInfo();
  if (tab === 'sync') syncTabOpened();
  mediaTabOpened(tab);
}

// The address keeps the tab and folder (#files/Music/ABBA), so a reload comes back to them.
// Each folder name is percent-encoded, because names such as "100% Hits" are valid.
function saveLocation() {
  const folder = state.tab === 'files' && state.cwd
    ? `/${state.cwd.split('/').map(encodeURIComponent).join('/')}` : '';
  const hash = state.view === 'computer' ? `#computer${mediaLocation()}` : `#${state.tab}${folder}${mediaLocation()}`;
  try { history.replaceState(null, '', hash); } catch { /* not important */ }
}

// A hand-edited or old address that does not decode leaves the app at the start.
function readLocation() {
  const raw = location.hash.slice(1);
  if (!raw) return;
  const i = raw.indexOf('/');
  let tab;
  let folder = null;
  try {
    tab = decodeURIComponent(i < 0 ? raw : raw.slice(0, i));
    if (i > 0) folder = raw.slice(i + 1).split('/').map(decodeURIComponent).join('/');
  } catch {
    return;
  }
  if (tab === 'music') tab = 'sync'; // the Music tab became Sync
  if (tab === 'computer') {
    state.view = 'computer';
    if (folder) readMediaLocation(folder, true);
    return;
  }
  if (document.querySelector(`#tabs button[data-tab="${CSS.escape(tab)}"]`)) state.tab = tab;
  if (tab === 'files' && folder && !checkPath(folder)) state.pendingPath = folder;
  if (tab === 'library' && folder) readMediaLocation(folder);
}

async function loadStatus(start = false) {
  const d = selectedDevice();
  if (!d || d.reachable === false) return;
  let daemonDown = false;
  const final = await runCli('status', { device: deviceSpec(d), start }, {
    quiet: true,
    onEvent: (ev) => { if (ev.event === 'warn' && ev.code === 'daemon_not_running') daemonDown = true; },
  });
  if (state.selectedId !== d.id) return;
  if (final.ok) {
    state.status = final;
    state.statusError = null;
  } else {
    state.status = null;
    state.statusError = { ...final, daemonDown };
  }
  renderMain();
}

// ---- files ----------------------------------------------------------------------------------

async function loadFolder(path) {
  const d = selectedDevice();
  if (!d) return;
  const entries = [];
  $('files-status').textContent = t('loadingFolder');
  const final = await runCli('ls', { device: deviceSpec(d), path, all: state.showHidden }, {
    quiet: true,
    onEvent: (ev) => { if (ev.event === 'entry') entries.push(ev); },
  });
  if (final.ok) {
    state.cwd = path;
    state.entries = entries.sort((a, b) =>
      (a.kind === 'dir' ? 0 : 1) - (b.kind === 'dir' ? 0 : 1) || a.name.localeCompare(b.name, getLang()));
    state.selected.clear();
    $('files-status').textContent = t('itemsCount', { n: entries.length });
    saveLocation();
  } else {
    $('files-status').textContent = errorMessage(final);
  }
  renderFiles();
}

function nameProblem(name) {
  if (name.includes('/')) return t('bad_forbidden_char');
  const reason = checkPath(name.normalize('NFC'));
  if (!reason) return null;
  const key = `bad_${reason}`;
  const msg = t(key);
  return msg === key ? t('bad_other') : msg;
}

async function newFolder() {
  const r = await sheet({
    title: t('newFolderTitle'), body: t('newFolderPrompt'), okText: t('create'),
    input: '', validate: (v) => nameProblem(v.trim()),
  });
  if (!r.ok) return;
  const p = state.cwd ? `${state.cwd}/${r.value.trim()}` : r.value.trim();
  const final = await runCli('mkdir', { device: deviceSpec(selectedDevice()), paths: [p] }, { title: t('creating') });
  if (!final.ok) $('files-status').textContent = errorMessage(final);
  await loadFolder(state.cwd);
}

async function renameSelected() {
  const [p] = [...state.selected];
  const entry = state.entries.find((e) => e.path === p);
  if (!entry) return;
  const r = await sheet({
    title: t('renameTitle'), body: t('renamePrompt'), okText: t('ok'),
    input: entry.name, validate: (v) => nameProblem(v.trim()),
  });
  if (!r.ok || r.value.trim() === entry.name) return;
  const dst = state.cwd ? `${state.cwd}/${r.value.trim()}` : r.value.trim();
  const final = await runCli('mv', { device: deviceSpec(selectedDevice()), src: p, dst }, { title: t('rename') });
  if (!final.ok) $('files-status').textContent = errorMessage(final);
  await loadFolder(state.cwd);
}

async function deleteSelected() {
  const items = state.entries.filter((e) => state.selected.has(e.path));
  if (!items.length) return;
  const anyDir = items.some((e) => e.kind === 'dir');
  const what = items.length === 1 ? `“${items[0].name}”` : t('itemsCount', { n: items.length });
  const r = await sheet({
    title: t('deleteTitle'),
    body: anyDir && items.length === 1 ? t('deleteFolderBody', { what }) : t('deleteBody', { what }),
    okText: t('deleteAction'), danger: true,
  });
  if (!r.ok) return;
  // The user agreed here, so the CLI does not need to ask again (--yes).
  const final = await runCli('rm', {
    device: deviceSpec(selectedDevice()), paths: items.map((e) => e.path), recursive: anyDir, yes: true,
  }, { title: t('deleting') });
  if (!final.ok) $('files-status').textContent = errorMessage(final);
  await loadFolder(state.cwd);
}

async function uploadFiles(files) {
  const d = selectedDevice();
  if (!d || !files.length || !state.cwd) return;
  let copied = 0;
  for (const file of files) {
    const name = file.name.normalize('NFC');
    const problem = nameProblem(name);
    if (problem) {
      $('files-status').textContent = `${name}: ${problem}`;
      continue;
    }
    const q = new URLSearchParams({ dir: state.cwd, name, device: deviceSpec(d), mode: 'skip-identical' });
    // The original modification time, so the copy on the HiBy keeps it (not the upload time).
    if (Number.isSafeInteger(file.lastModified) && file.lastModified > 0) q.set('mtime', String(file.lastModified));
    const final = await runCli('put', null, {
      title: t('copying', { name }), upload: `/api/upload?${q}`, file,
    });
    if (final.ok) copied += final.written ?? 0;
    else { $('files-status').textContent = `${name}: ${errorMessage(final)}`; break; }
  }
  if (copied) $('files-status').textContent = t('copied', { n: copied });
  await loadFolder(state.cwd);
}

// ---- pairing ----------------------------------------------------------------------------------

async function pairFlow() {
  const r = await sheet({ title: t('pairTitle'), body: t('pairStep1'), okText: t('pair') });
  if (!r.ok) return;
  const final = await runCli('connect', { device: 'wifi', pair: true }, { title: t('pairTitle') });
  if (final.ok) {
    $('empty-status').textContent = t('paired', { name: final.name });
    refreshDevices();
  } else if (final.code !== 'cancelled') {
    await sheet({ title: t('pairTitle'), body: errorMessage(final), okText: t('ok'), noCancel: true });
  }
}

// ---- updates: build ByOS on this computer and install it over the cable ------------------------
// Four byos-cli runs (docs/DESKTOP_APP.md 5.7): firmware-fetch (or a picked file), build,
// build-send --install. lib/build-flow.mjs turns their lines into the four steps.

const isUsb = (d) => !!d && d.transport === 'usb' && d.reachable !== false && state.upd.demo?.usb !== false;

function versionNewer(a, b) {
  const p = (v) => String(v ?? '').split(/[.+-]/).map((x) => Number.parseInt(x, 10) || 0);
  const x = p(a); const y = p(b);
  for (let i = 0; i < Math.max(x.length, y.length); i++) {
    if ((x[i] ?? 0) !== (y[i] ?? 0)) return (x[i] ?? 0) > (y[i] ?? 0);
  }
  return false;
}

async function loadBuildInfo() {
  if (state.upd.demo || state.upd.running) return;
  const final = await runCli('build-info', {}, { quiet: true });
  if (state.upd.running) return;
  state.upd.info = final.ok ? final : null;
  state.upd.infoError = final.ok ? null : final;
  // A finished install stays on screen; otherwise the steps start from what is already there.
  if (state.upd.flow.phase !== 'done') state.upd.flow = applyInfo(emptyFlow(), final);
  renderUpdates();
}

// Runs one byos-cli job for one step and follows it into the flow.
async function runStep(name, command, params, opts = {}) {
  const f = state.upd.flow;
  startStep(f, name);
  renderUpdates();
  const final = await runCli(command, params, {
    ...opts,
    onStarted: (job) => { state.upd.job = job; },
    onEvent: (ev) => {
      if (ev.event === 'progress' || ev.event === 'warn') {
        applyEvent(f, name, ev);
        renderUpdates();
      }
    },
  });
  state.upd.job = null;
  finishStep(f, name, final);
  renderUpdates();
  return final;
}

function resetFailed() {
  const f = state.upd.flow;
  if (f.phase === 'done') state.upd.flow = applyInfo(emptyFlow(), state.upd.info ?? {});
  for (const s of STEPS) if (state.upd.flow.steps[s].status === 'failed') state.upd.flow.steps[s].status = 'todo';
  state.upd.flow.error = null;
  state.upd.flow.errorStep = null;
}

// The whole flow, hands-free after the one confirmation: firmware, build, copy, install.
async function buildAndInstall() {
  const u = state.upd;
  if (u.running || u.demo) return;
  resetFailed();
  u.running = true;
  u.flow.phase = 'running';
  renderUpdates();
  try {
    const f = u.flow;
    if (f.steps.firmware.status !== 'done') {
      if (!(await runStep('firmware', 'firmware-fetch', {}, { title: t('updJobFirmware') })).ok) return;
    }
    if (f.steps.build.status !== 'done') {
      if (!(await runStep('build', 'build', {}, { title: t('updJobBuild') })).ok) return;
    }
    const d = selectedDevice();
    if (!isUsb(d)) {
      f.phase = 'idle';
      return;
    }
    const v = u.info?.hibyos_version ?? '';
    const ok = await sheet({ title: t('updConfirmTitle', { v }), body: t('updConfirmBody'), okText: t('updInstall') });
    if (!ok.ok) {
      f.phase = 'idle';
      return;
    }
    const final = await runStep('send', 'build-send', { device: deviceSpec(d), install: true, yes: true }, { title: t('updJobInstall', { v }) });
    if (final.ok) {
      refreshDevices();
      loadStatus();
    }
  } finally {
    u.running = false;
    u.job = null;
    renderUpdates();
  }
}

// A firmware file the user already has (for example from HiBy's page): byos-cli checks it.
async function useFirmwareFile(file) {
  const u = state.upd;
  if (u.running || u.demo || !file) return;
  resetFailed();
  u.running = true;
  try {
    await runStep('firmware', 'firmware-fetch', null, { title: t('updJobCheckFile'), upload: '/api/firmware', file });
    if (u.flow.phase !== 'failed') u.flow.phase = 'idle';
  } finally {
    u.running = false;
    renderUpdates();
  }
}

function cancelUpdate() {
  const job = state.upd.job;
  if (job?.id) api(`/api/jobs/${job.id}/cancel`, { method: 'POST', body: {} }).catch(() => {});
}

function stepTitle(name, info) {
  const v = info?.hibyos_version ?? '';
  switch (name) {
    case 'firmware': return t('updStepFirmware', { v: info?.firmware?.version ?? '' });
    case 'build': return t('updStepBuild', { v });
    case 'send': return t('updStepSend');
    default: return t('updStepInstall');
  }
}

function stepDetail(name, s, info, flow) {
  const d = selectedDevice();
  if (s.status === 'failed') {
    const e = flow.error;
    if (name === 'firmware' && e?.code === 'verify') return t('updFwWrong', { file: info?.firmware?.file ?? 'r3ii_2025.upt' });
    if (name === 'firmware' && e?.code === 'no_device') return t('updFwOffline');
    if (name === 'build') return `${t('updBuildFailed')} ${e?.message ?? ''}`.trim();
    if (name === 'install' && e?.btp?.name !== 'E_BATTERY' && e?.code === 'remote') return `${t('updRefused')} ${e?.message ?? ''}`.trim();
    return errorMessage(e);
  }
  const fw = info?.firmware;
  if (name === 'firmware') {
    if (s.status === 'active') {
      return s.phase === 'check' ? t('updFwChecking')
        : s.total ? t('updFwDownloading', { done: fmtBytes(s.done), total: fmtBytes(s.total) }) : t('updFwStarting');
    }
    if (s.status === 'done') return s.detail === 'file' ? t('updFwFromFile') : t('updFwReady');
    return t('updFwTodo', { size: fmtBytes(fw?.size) });
  }
  if (name === 'build') {
    if (s.status === 'active') return t(`stage_${s.phase ?? 'check'}`);
    if (s.status === 'done') return t('updBuildDone', { size: fmtBytes(flow.buildSize || null) || '' });
    return t('updBuildTodo');
  }
  if (name === 'send') {
    if (s.status === 'active') {
      return s.phase === 'put' && s.total ? t('updSendPut', { done: fmtBytes(s.done), total: fmtBytes(s.total) }) : t('updSendHash');
    }
    if (s.status === 'done') return flow.parked.length ? `${t('updSendDone')} ${t('updParked', { names: flow.parked.join(', ') })}` : t('updSendDone');
    return isUsb(d) ? t('updSendTodo') : t('updNeedsUsb');
  }
  if (s.status === 'active') return s.phase === 'reconnect' ? t('updInstallReconnect') : t('updInstallReboot');
  if (s.status === 'done') return t('updInstallDone', { v: flow.result?.version ?? info?.hibyos_version ?? '' });
  return t('updInstallTodo');
}

const SVG_NS = 'http://www.w3.org/2000/svg';
const MARKS = { done: 'M3.5 8.5 6.5 11.5 12.5 4.5', failed: 'M4.5 4.5l7 7M11.5 4.5l-7 7' };

function stepMark(s, i) {
  const m = el('span', { class: 'mark' });
  if (s.status === 'active') m.append(el('span', { class: 'spinner' }));
  else if (MARKS[s.status]) {
    const svg = document.createElementNS(SVG_NS, 'svg');
    svg.setAttribute('viewBox', '0 0 16 16');
    svg.setAttribute('aria-hidden', 'true');
    const path = document.createElementNS(SVG_NS, 'path');
    for (const [k, v] of Object.entries({ d: MARKS[s.status], fill: 'none', stroke: 'currentColor', 'stroke-width': '2', 'stroke-linecap': 'round', 'stroke-linejoin': 'round' })) path.setAttribute(k, v);
    svg.append(path);
    m.append(svg);
  } else m.textContent = String(i + 1);
  return m;
}

function renderUpdates() {
  const u = state.upd;
  const d = selectedDevice();
  const info = u.info;
  const f = u.flow;
  // after an install, the new version (the device list catches up on its next refresh)
  const installed = f.phase === 'done' && f.result?.version ? f.result.version : d?.byos_version ?? null;
  $('upd-title').textContent = installed ? t('updInstalled', { v: installed }) : t('updTitleNoDevice');
  const pill = $('upd-pill');
  if (info) {
    const newer = installed && versionNewer(info.hibyos_version, installed);
    $('upd-sub').textContent = newer
      ? t('updNewer', { v: info.hibyos_version, fw: info.firmware?.version ?? '' })
      : t('updSame', { v: info.hibyos_version, fw: info.firmware?.version ?? '' });
    pill.hidden = !newer && f.phase !== 'done';
    pill.textContent = f.phase === 'done' ? t('updPillDone') : t('updPillNew');
    pill.className = `pill${f.phase === 'done' ? ' ok' : ''}`;
  } else {
    $('upd-sub').textContent = u.infoError ? errorMessage(u.infoError) : t('updLoading');
    pill.hidden = true;
  }

  const list = $('upd-steps');
  list.replaceChildren();
  STEPS.forEach((name, i) => {
    const s = f.steps[name];
    const frac = fraction(s);
    const li = el('li', { class: `step ${s.status}` },
      stepMark(s, i),
      el('div', { class: 'step-body' },
        el('div', { class: 'step-title', text: stepTitle(name, info) }),
        el('div', { class: 'step-detail', text: stepDetail(name, s, info, f) }),
        s.status === 'active'
          ? el('div', { class: `step-bar${frac === null ? ' wait' : ''}` }, el('span', { style: frac === null ? '' : `width:${(frac * 100).toFixed(1)}%` }))
          : null));
    list.append(li);
  });

  const res = $('upd-result');
  res.hidden = !(f.phase === 'done' || (f.phase === 'failed' && f.error?.code !== 'cancelled'));
  res.className = `upd-result${f.phase === 'done' ? ' ok' : ' fail'}`;
  res.replaceChildren();
  if (f.phase === 'done') {
    res.append(el('strong', { text: t('updDoneTitle', { v: f.result?.version ?? info?.hibyos_version ?? '' }) }), el('span', { text: t('updDoneBody') }));
  } else if (!res.hidden) {
    res.append(el('strong', { text: t('updFailTitle') }), el('span', { text: errorMessage(f.error) }));
    if (f.error?.page) res.append(el('span', { class: 'muted', text: t('updPage', { page: f.error.page }) }));
  }

  const go = $('btn-upd-go');
  const usb = isUsb(d);
  go.hidden = u.running || f.phase === 'done';
  go.disabled = !info;
  if (f.phase === 'failed') go.textContent = t('retry');
  else if (!usb) go.textContent = f.steps.build.status === 'done' ? t('updConnectUsb') : t('updBuildOnly');
  else go.textContent = installed && info && installed === info.hibyos_version ? t('updReinstall') : t('updGo');
  if (!usb && f.steps.build.status === 'done') go.disabled = true;
  $('btn-upd-cancel').hidden = !u.running;
  // the user can always pick the official firmware by hand (as in the Android app), also to
  // replace one already checked
  $('btn-fw-file').hidden = !!u.running;
  $('btn-fw-file').textContent = t(f.steps.firmware.status === 'done' ? 'updPickOther' : 'updPickFile');
}

// Demo mode only (?demo=NAME): a fixed picture of one state, for screenshots and design review.
function demoUpdates(name) {
  const info = {
    ok: true, hibyos_version: '0.9.0', bundle: 'builtin',
    firmware: { version: '1.3', file: 'r3ii_2025.upt', size: 46759936, can_download: true },
    firmware_ready: false, build: null,
  };
  const f = emptyFlow();
  const doneTo = (n) => { for (const s of STEPS.slice(0, n)) Object.assign(f.steps[s], { status: 'done', detail: 'download' }); };
  f.sendTotal = 47845376 + 8063528;
  f.buildSize = 47845376;
  const usb = name !== 'wifi';
  switch (name) {
    case 'downloading': f.phase = 'running'; Object.assign(f.steps.firmware, { status: 'active', phase: 'download', done: 19 * 1024 ** 2, total: 46759936, unit: 'bytes' }); break;
    case 'building': f.phase = 'running'; doneTo(1); Object.assign(f.steps.build, { status: 'active', phase: 'verify_rootfs', done: 560, total: 1000, unit: 'permille' }); break;
    case 'sending': f.phase = 'running'; doneTo(2); Object.assign(f.steps.send, { status: 'active', phase: 'put', done: 31 * 1024 ** 2, total: f.sendTotal, unit: 'bytes' }); break;
    case 'installing': f.phase = 'running'; doneTo(3); Object.assign(f.steps.install, { status: 'active', phase: 'reboot' }); break;
    case 'done': f.phase = 'done'; doneTo(4); f.result = { version: '0.9.0', state: 'installed' }; break;
    case 'failed': f.phase = 'failed'; doneTo(2); f.steps.send.status = 'done'; f.steps.install.status = 'failed'; f.errorStep = 'install';
      f.error = { ok: false, code: 'remote', message: "the HiBy's battery is at 12 %", btp: { code: 67, name: 'E_BATTERY' } }; break;
    case 'wifi': doneTo(2); break;
    case 'ready': doneTo(1); break;
    default: break;
  }
  if (['building', 'sending', 'installing', 'done', 'failed', 'wifi', 'ready'].includes(name)) info.firmware_ready = true;
  state.upd = { info, flow: f, running: f.phase === 'running', job: null, demo: { name, usb } };
  state.tab = 'updates';
}

// ---- sheets -------------------------------------------------------------------------------------

let sheetBusy = Promise.resolve();

// A modal sheet. Options: title, body, okText, danger, input (initial text), validate(v),
// extra(container), collect() for custom values, noCancel. Resolves { ok, value }.
function sheet(o) {
  const run = () => new Promise((resolve) => {
    const dlg = $('sheet');
    $('sheet-title').textContent = o.title ?? '';
    $('sheet-body').textContent = o.body ?? '';
    const extra = $('sheet-extra');
    extra.replaceChildren();
    $('sheet-error').textContent = '';
    let input = null;
    if (o.input !== undefined) {
      input = el('input', { type: 'text', value: o.input, spellcheck: 'false', autocomplete: 'off' });
      extra.append(input);
    }
    o.extra?.(extra);
    const okBtn = $('sheet-ok');
    okBtn.textContent = o.okText ?? t('ok');
    okBtn.className = `btn primary${o.danger ? ' danger' : ''}`;
    $('sheet-cancel').textContent = t('cancel');
    $('sheet-cancel').hidden = !!o.noCancel;
    const onSubmit = (e) => {
      if (e.submitter?.value !== 'ok') return;
      const value = o.collect ? o.collect() : input?.value ?? null;
      const problem = o.validate?.(value);
      if (problem) {
        e.preventDefault();
        $('sheet-error').textContent = problem;
      }
    };
    $('sheet-form').addEventListener('submit', onSubmit);
    let mine = true; // this sheet is still the one on screen
    dlg.addEventListener('close', () => {
      mine = false;
      $('sheet-form').removeEventListener('submit', onSubmit);
      const ok = dlg.returnValue === 'ok';
      resolve({ ok, value: ok ? (o.collect ? o.collect() : input?.value ?? null) : null });
    }, { once: true });
    dlg.returnValue = '';
    dlg.showModal();
    o.abort?.then(() => { if (mine && dlg.open) dlg.close(''); });
    (input ?? extra.querySelector('input') ?? okBtn).focus();
    if (input) input.select();
  });
  sheetBusy = sheetBusy.then(run, run);
  return sheetBusy;
}

async function pairCodeSheet(ev, abort) {
  const boxes = [];
  const r = await sheet({
    abort,
    title: ev.device ?? t('pairTitle'),
    body: t('pairCode'),
    okText: t('pair'),
    extra: (c) => {
      const row = el('div', { class: 'code-boxes' });
      for (let i = 0; i < (ev.digits ?? 6); i++) {
        const b = el('input', { type: 'text', inputmode: 'numeric', maxlength: '1', 'aria-label': `Digit ${i + 1}` });
        b.addEventListener('input', () => {
          b.value = b.value.replace(/\D/g, '').slice(-1);
          if (b.value && boxes[i + 1]) boxes[i + 1].focus();
        });
        b.addEventListener('keydown', (e) => {
          if (e.key === 'Backspace' && !b.value && boxes[i - 1]) boxes[i - 1].focus();
        });
        b.addEventListener('paste', (e) => {
          const digits = (e.clipboardData?.getData('text') ?? '').replace(/\D/g, '');
          if (digits.length > 1) {
            e.preventDefault();
            boxes.forEach((x, j) => { x.value = digits[j] ?? ''; });
            boxes[Math.min(digits.length, boxes.length) - 1].focus();
          }
        });
        boxes.push(b);
        row.append(b);
      }
      c.append(row);
    },
    collect: () => boxes.map((b) => b.value).join(''),
    validate: (v) => (/^\d{6}$/.test(v) ? null : t('pairCode')),
  });
  return { ok: r.ok, code: r.value };
}

// ---- rendering ----------------------------------------------------------------------------------

function applyStaticText() {
  document.documentElement.lang = getLang();
  for (const n of document.querySelectorAll('[data-i18n]')) n.textContent = t(n.dataset.i18n);
  for (const n of document.querySelectorAll('[data-i18n-title]')) n.title = t(n.dataset.i18nTitle);
  for (const n of document.querySelectorAll('[data-later]')) {
    n.replaceChildren(el('p', { text: t(n.dataset.later) }), el('p', { class: 'small-text', text: t('later', { m: n.dataset.m }) }));
  }
  $('files-empty').textContent = t('emptyFolder');
}

function renderDeviceList() {
  const list = $('device-list');
  list.replaceChildren();
  if (!state.devices.length) {
    list.append(el('li', { class: 'side-item disabled' },
      state.scanning ? el('span', { class: 'spinner' }) : null,
      el('span', { text: state.scanning ? t('searching') : t('noDevices') })));
    return;
  }
  for (const d of state.devices) {
    const offline = d.reachable === false;
    const busy = state.jobs.some((j) => j.state === 'running') && d.id === state.selectedId;
    const li = el('li', {},
      el('button', {
        class: `side-item device${d.id === state.selectedId && state.view !== 'computer' ? ' selected' : ''}${offline ? ' offline' : ''}`,
        onclick: () => { selectDevice(d.id); showDevice(); },
        title: offline ? t('notConnected') : '',
      },
      el('span', { class: `ico ${d.transport === 'wifi' ? 'ico-wifi' : 'ico-usb'}` }),
      el('span', { class: 'dev-name', text: d.name }),
      busy ? el('span', { class: 'spinner' }) : offline ? el('span', { class: 'dev-meta', text: '—' }) : null));
    list.append(li);
  }
}

function renderMain() {
  const d = selectedDevice();
  const pc = state.view === 'computer';
  $('computer-view').hidden = !pc;
  $('empty-state').hidden = pc || !!d;
  $('device-view').hidden = pc || !d;
  if (pc) { renderMedia(); return; }
  if (!d) return;
  $('dev-name').textContent = d.name;
  const parts = [`${t('byosVersion')} ${d.byos_version ?? '?'}`, d.transport === 'wifi' ? t('wifi') : t('usb')];
  if (d.reachable === false) parts.push(t('notConnected'));
  $('dev-sub').textContent = parts.join(' · ');
  for (const b of document.querySelectorAll('#tabs button')) {
    b.setAttribute('aria-selected', String(b.dataset.tab === state.tab));
  }
  for (const p of document.querySelectorAll('.tab-panel')) p.hidden = p.dataset.panel !== state.tab;
  renderBanner();
  renderSummary();
  renderFiles();
  renderUpdates();
  renderMedia();
  renderSync();
}

function renderBanner() {
  const banner = $('banner');
  const action = $('banner-action');
  action.hidden = true;
  action.onclick = null;
  let text = null;
  const s = state.status;
  const sd = s?.sd?.sd_state ?? selectedDevice()?.sd_state;
  if (state.statusError) {
    text = state.statusError.daemonDown ? t('err_daemon_not_running') : errorMessage(state.statusError);
    if (state.statusError.daemonDown && selectedDevice()?.transport === 'usb') {
      action.hidden = false;
      action.textContent = t('startSync');
      action.onclick = () => loadStatus(true);
    }
  } else if (sd === 'usb_storage') text = t('bannerUsbStorage');
  else if (sd === 'no_card') text = t('bannerNoCard');
  else if (s?.update && s.update.state && s.update.state !== 'none') text = t('bannerUpdate');
  else if (s?.db?.staged) text = t('bannerDbStaged');
  banner.hidden = !text;
  $('banner-text').textContent = text ?? '';
}

function capacityBar(container, parts, total) {
  container.replaceChildren();
  for (const [cls, value] of parts) {
    const span = el('span', { class: cls });
    if (value > 0) span.style.minWidth = '4px';   // a few MB on a 128 GB card still shows
    container.append(span);
    requestAnimationFrame(() => { span.style.flexBasis = `${total ? (100 * value) / total : 0}%`; });
  }
}

function renderSummary() {
  const s = state.status;
  const d = selectedDevice();
  const sd = s?.sd;
  if (sd && sd.sd_total) {
    const usedBytes = sd.sd_total - sd.sd_free;
    capacityBar($('capacity'), [['used', usedBytes]], sd.sd_total);
    $('capacity-legend').replaceChildren(
      el('span', {}, el('i', { style: 'background:var(--accent)' }), t('used'), el('span', { class: 'v', text: fmtBytes(usedBytes) })),
      el('span', {}, el('i', { style: 'background:var(--cap-free)' }), t('freeLabel'), el('span', { class: 'v', text: fmtBytes(sd.sd_free) })),
      el('span', { class: 'muted', text: t('totalCap', { size: fmtBytes(sd.sd_total) }) }),
    );
  } else {
    capacityBar($('capacity'), [], 1);
    $('capacity-legend').replaceChildren();
  }
  if (sd && sd.data_total) {
    capacityBar($('internal'), [['used', sd.data_total - sd.data_free]], sd.data_total);
    $('internal-legend').textContent = `${t('free', { size: fmtBytes(sd.data_free) })} · ${fmtBytes(sd.data_total)}`;
  } else {
    capacityBar($('internal'), [], 1);
    $('internal-legend').textContent = '';
  }

  const facts = $('facts');
  facts.replaceChildren();
  const add = (k, v) => { if (v !== undefined && v !== null && v !== '') facts.append(el('dt', { text: k }), el('dd', { text: String(v) })); };
  if (d) {
    add(t('byosVersion'), d.byos_version);
    add(t('firmware'), d.firmware);
    if (d.serial) add(t('serial'), d.serial);
    add(t('deviceId'), d.id);
    add(t('connection'), `${d.transport === 'wifi' ? t('wifi') : t('usb')}${s?.encrypted ? ` (${t('encrypted')})` : ''}`);
  }
  if (s) {
    if (typeof s.battery === 'number') add(t('battery'), `${s.battery} %${s.charging ? ` (${t('charging')})` : ''}`);
    const sdName = { mounted: t('sdMounted'), usb_storage: t('sdUsbStorage'), no_card: t('sdNoCard'), read_only: t('sdReadOnly') };
    if (sd) add(t('card'), `${sdName[sd.sd_state] ?? sd.sd_state}${sd.sd_fs ? ` · ${sd.sd_fs}` : ''}`);
  }
}

function renderFiles() {
  const crumbs = $('crumbs');
  crumbs.replaceChildren();
  const parts = state.cwd ? state.cwd.split('/') : [];
  crumbs.append(el('button', { text: t('sdRoot'), onclick: () => loadFolder('') }));
  parts.forEach((p, i) => {
    const target = parts.slice(0, i + 1).join('/');
    crumbs.append(el('span', { class: 'sep', text: '›' }), el('button', { text: p, onclick: () => loadFolder(target) }));
  });
  $('btn-up').disabled = !state.cwd;
  $('btn-upload').disabled = !state.cwd;
  $('btn-upload').title = state.cwd ? '' : t('err_E_BAD_PATH');
  $('btn-delete').disabled = state.selected.size === 0;
  $('btn-rename').disabled = state.selected.size !== 1;

  const rows = $('file-rows');
  rows.replaceChildren();
  state.entries.forEach((e, i) => {
    const tr = el('tr', {
      class: `${e.kind === 'dir' ? 'dir' : 'file'}${state.selected.has(e.path) ? ' selected' : ''}`,
      style: `animation-delay:${Math.min(i, 20) * 12}ms`,
      onclick: (ev) => {
        if (!(ev.ctrlKey || ev.metaKey)) state.selected.clear();
        if (state.selected.has(e.path)) state.selected.delete(e.path); else state.selected.add(e.path);
        renderFiles();
      },
      ondblclick: () => { if (e.kind === 'dir') loadFolder(e.path); },
    },
    el('td', {}, el('span', { class: 'fname', text: e.name })),
    el('td', { class: 'num', text: e.kind === 'dir' ? '—' : fmtBytes(e.size) }),
    el('td', { text: kindOf(e) }),
    el('td', { text: fmtDate(e.mtime) }));
    rows.append(tr);
  });
  $('files-empty').hidden = state.entries.length > 0;
}

function renderLcd() {
  const lcd = $('lcd');
  const running = state.jobs.find((j) => j.state === 'running');
  const bar = $('lcd-bar');
  if (running) {
    lcd.classList.add('busy');
    $('lcd-title').textContent = running.progress?.op === 'pair_wait' ? t('pairAllow') : running.title;
    const p = running.progress;
    if (p && p.total > 0) {
      bar.hidden = false;
      $('lcd-fill').style.width = `${Math.min(100, (100 * p.done) / p.total)}%`;
    } else bar.hidden = true;
  } else {
    lcd.classList.remove('busy');
    bar.hidden = true;
    const d = selectedDevice();
    $('lcd-title').textContent = d ? d.name : t('idle');
    const last = state.jobs[0];
    $('lcd-sub').textContent = last ? `${last.title} — ${last.state === 'done' ? t('done') : t('failed')}` : '';
  }
  renderDeviceList();
}

function renderActivity() {
  const list = $('activity-list');
  list.replaceChildren();
  if (!state.jobs.length) {
    list.append(el('li', { class: 'muted', text: t('noActivity') }));
    return;
  }
  for (const j of state.jobs) {
    const stateText = j.state === 'running' ? '…' : j.state === 'done' ? t('done') : errorMessage(j.final);
    const li = el('li', {},
      el('div', { class: 'job-head' },
        el('span', { text: j.title }),
        el('span', { class: `job-state${j.state === 'failed' ? ' fail' : ''}`, text: stateText })));
    if (j.state === 'running' && j.id) {
      li.append(el('button', {
        class: 'btn', text: t('cancel'),
        onclick: () => api(`/api/jobs/${j.id}/cancel`, { method: 'POST', body: {} }).catch(() => {}),
      }));
    }
    if (j.state === 'failed' && j.final) {
      li.append(el('details', {}, el('summary', { text: t('details') }), el('pre', { text: JSON.stringify(j.final, null, 1) })));
    }
    list.append(li);
  }
}

// ---- wiring ------------------------------------------------------------------------------------------

function wire() {
  const sel = $('lang-select');
  for (const [code, label] of Object.entries(LANGS)) sel.append(el('option', { value: code, text: label }));
  sel.value = getLang();
  sel.addEventListener('change', () => {
    setLang(sel.value);
    try { localStorage.setItem('byos.lang', sel.value); } catch { /* storage may be blocked */ }
    applyStaticText();
    renderDeviceList();
    renderMain();
    renderLcd();
  });

  $('btn-refresh-devices').addEventListener('click', () => refreshDevices());
  $('btn-pair').addEventListener('click', () => pairFlow());
  $('lcd').addEventListener('click', () => { renderActivity(); $('activity').showModal(); });
  $('activity').addEventListener('click', (e) => { if (e.target === $('activity')) $('activity').close(); });

  for (const b of document.querySelectorAll('#tabs button')) {
    b.addEventListener('click', () => setTab(b.dataset.tab));
  }

  $('btn-upd-go').addEventListener('click', () => buildAndInstall());
  $('btn-upd-cancel').addEventListener('click', () => cancelUpdate());
  $('btn-fw-file').addEventListener('click', () => $('fw-input').click());
  $('fw-input').addEventListener('change', (e) => { useFirmwareFile(e.target.files?.[0]); e.target.value = ''; });

  $('btn-up').addEventListener('click', () => {
    const i = state.cwd.lastIndexOf('/');
    loadFolder(i < 0 ? '' : state.cwd.slice(0, i));
  });
  $('chk-hidden').addEventListener('change', (e) => { state.showHidden = e.target.checked; loadFolder(state.cwd); });
  $('btn-newfolder').addEventListener('click', () => newFolder());
  $('btn-rename').addEventListener('click', () => renameSelected());
  $('btn-delete').addEventListener('click', () => deleteSelected());
  $('btn-upload').addEventListener('click', () => $('file-input').click());
  $('file-input').addEventListener('change', (e) => { uploadFiles([...e.target.files]); e.target.value = ''; });

  const zone = $('drop-zone');
  zone.addEventListener('dragover', (e) => {
    if (!state.cwd || !e.dataTransfer?.types.includes('Files')) return;
    e.preventDefault();
    zone.classList.add('dragging');
  });
  zone.addEventListener('dragleave', (e) => { if (!zone.contains(e.relatedTarget)) zone.classList.remove('dragging'); });
  zone.addEventListener('drop', (e) => {
    e.preventDefault();
    zone.classList.remove('dragging');
    uploadFiles([...(e.dataTransfer?.files ?? [])]);
  });

  document.addEventListener('keydown', (e) => {
    if (state.tab !== 'files' || document.querySelector('dialog[open]')) return;
    if (e.key === 'Delete' && state.selected.size) deleteSelected();
    if (e.key === 'F2' && state.selected.size === 1) renameSelected();
    if (e.key === 'Backspace' && state.cwd) $('btn-up').click();
  });

  // Look for devices again every 5 s while the window is visible and nothing is running.
  setInterval(() => {
    if (document.hidden || state.jobs.some((j) => j.state === 'running') || document.querySelector('dialog[open]')) return;
    refreshDevices();
  }, 5000);
}

async function main() {
  let saved = null;
  try { saved = localStorage.getItem('byos.lang'); } catch { /* storage may be blocked */ }
  const forced = new URLSearchParams(location.search).get('lang');
  setLang(forced ?? saved ?? detectLang(navigator.languages));
  readLocation();
  applyStaticText();
  wire();
  const ctx = {
    t, el, getLang, api, runCli, sheet, errorMessage, selectedDevice, deviceSpec, fmtBytes, setTab, saveLocation, appState: state,
    showComputer, showDevice, syncFromComputer, libraryChanged,
  };
  initMedia(ctx);
  initSync(ctx, { icon, ICONS, toast, messageCard, ageText });
  try {
    state.info = await api('/api/info');
    const badge = $('mode-badge');
    if (state.info.mock) { badge.hidden = false; badge.textContent = t('mockBadge'); }
    if (state.info.error) { badge.hidden = false; badge.className = 'badge warn'; badge.textContent = t('cliMissing'); badge.title = state.info.error; }
    // Demo mode only: ?demo=NAME shows one fixed state of the Updates page (README, screenshots).
    const demo = new URLSearchParams(location.search).get('demo');
    if (state.info.mock && demo) {
      const params = new URLSearchParams(location.search);
      // ?frame=MS: every animation stopped MS after it starts, for a frame-by-frame review
      if (params.has('frame')) {
        const ms = Math.max(0, Number(params.get('frame')) || 0);
        const sheet = [...document.styleSheets].find((x) => (x.href ?? '').endsWith('/app.css'));
        // only the element under review (?framesel=CSS), the rest of the page as it really is by then
        const sel = params.get('framesel') || '*';
        const target = sel.split(',').map((x) => `${x}, ${x} *, ${x}::backdrop`).join(', ');
        sheet?.insertRule(`${target} { animation-play-state: paused !important; animation-delay: -${ms}ms !important; }`, sheet.cssRules.length);
      }
      const tab = demo.startsWith('sync-') ? await demoSync(demo, params) : await demoMedia(demo, params);
      if (tab === 'computer') state.view = 'computer';
      else if (tab) { state.tab = tab; state.view = 'device'; }
      else demoUpdates(demo);
    }
  } catch { /* shown by the device list */ }
  renderLcd();
  renderMain();
  if (state.view === 'computer') computerOpened();
  refreshDevices();
}

main();
