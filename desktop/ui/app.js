// ByOS desktop scaffold UI. Plain ES modules, no framework. Every device operation is one
// byos-cli run through the local runner (server.mjs): POST /api/jobs, then server-sent events.
// Names from the device are only ever inserted with textContent.

import { t, setLang, getLang, detectLang, LANGS, errorMessage } from '/i18n.js';
import { checkPath } from '/btp-path.js';

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
  status: null,
  statusError: null,
  cwd: '',
  entries: [],
  selected: new Set(),
  showHidden: false,
  jobs: [],
  scanning: false,
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
}

// The address keeps the tab and folder (#files/Music/ABBA), so a reload comes back to them.
// Each folder name is percent-encoded, because names such as "100% Hits" are valid.
function saveLocation() {
  const folder = state.tab === 'files' && state.cwd
    ? `/${state.cwd.split('/').map(encodeURIComponent).join('/')}` : '';
  const hash = `#${state.tab}${folder}`;
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
  if (document.querySelector(`#tabs button[data-tab="${CSS.escape(tab)}"]`)) state.tab = tab;
  if (tab === 'files' && folder && !checkPath(folder)) state.pendingPath = folder;
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
        class: `side-item device${d.id === state.selectedId ? ' selected' : ''}${offline ? ' offline' : ''}`,
        onclick: () => selectDevice(d.id),
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
  $('empty-state').hidden = !!d;
  $('device-view').hidden = !d;
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
      el('span', { class: 'muted', text: fmtBytes(sd.sd_total) }),
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
    b.addEventListener('click', () => {
      state.tab = b.dataset.tab;
      saveLocation();
      renderMain();
      if (state.tab === 'files' && !state.entries.length) loadFolder(state.cwd);
    });
  }

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
  try {
    state.info = await api('/api/info');
    const badge = $('mode-badge');
    if (state.info.mock) { badge.hidden = false; badge.textContent = t('mockBadge'); }
    if (state.info.error) { badge.hidden = false; badge.className = 'badge warn'; badge.textContent = t('cliMissing'); badge.title = state.info.error; }
  } catch { /* shown by the device list */ }
  renderLcd();
  renderMain();
  refreshDevices();
}

main();
