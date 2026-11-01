/* Machine Black Box - live monitor dashboard
   Talks to the ESP32 over the USB cable (Web Serial, Chrome/Edge) or over WiFi (MQTT over WebSocket).
   Everything shown comes from the ESP32; nothing is simulated. */
(() => {
  'use strict';

  const LIVE_WINDOW_S = 60;          // seconds shown on the live charts
  const OFFLINE_AFTER_MS = 3000;     // no data for this long = offline
  const MAX_RECORDS = 25;            // fault records kept in this browser
  const SERIAL_BAUD = 460800;

  const $ = id => document.getElementById(id);

  // ---------- browser storage (best effort; the page works without it) ----------
  const store = {
    get(key, fallback) {
      try { const v = localStorage.getItem(key); return v === null ? fallback : JSON.parse(v); } catch (e) { return fallback; }
    },
    set(key, value) {
      try { localStorage.setItem(key, JSON.stringify(value)); } catch (e) { /* storage unavailable */ }
    },
  };

  // ---------- state ----------
  const st = {
    mode: 'usb',
    linked: false,
    device: null,
    via: null,
    lastSeen: 0,
    lwt: null,
    lwtAt: 0,
    health: null,
    live: null,
    latestT: null,
    series: { vib: [], cur: [], temp: [] },
    marks: [],
    records: store.get('mbb.records', []),
    autoOpen: store.get('mbb.autoOpen', true),
    calSeconds: 20,
    prev: {},
  };
  if (!Array.isArray(st.records)) st.records = [];

  // ---------- helpers ----------
  const isNum = v => typeof v === 'number' && Number.isFinite(v);
  const fmt = (v, d) => (isNum(v) ? v.toFixed(d) : '—');
  const timeStr = ms => new Date(ms).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false });
  const reasonText = r => ({ vibration: 'vibration over threshold', current: 'current over threshold', temperature: 'temperature over threshold', manual: 'manual capture' }[r] || r);
  function fmtUptime(s) {
    if (!isNum(s)) return '—';
    const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = Math.floor(s % 60);
    return h ? h + ' h ' + m + ' min' : m ? m + ' min ' + sec + ' s' : sec + ' s';
  }

  function log(msg) {
    const li = document.createElement('li');
    const a = document.createElement('span');
    const b = document.createElement('span');
    a.textContent = timeStr(Date.now());
    b.textContent = msg;
    li.append(a, b);
    $('log').prepend(li);
    while ($('log').children.length > 100) $('log').lastChild.remove();
  }

  let toastTimer = null;
  function hideToast() { $('toast').hidden = true; }
  function toast(text, action) {
    const t = $('toast');
    t.textContent = '';
    const s = document.createElement('span');
    s.textContent = text;
    t.append(s);
    if (action) {
      const b = document.createElement('button');
      b.type = 'button';
      b.className = 'btn';
      b.textContent = action.label;
      b.addEventListener('click', () => { hideToast(); action.run(); });
      t.append(b);
    }
    t.hidden = false;
    clearTimeout(toastTimer);
    toastTimer = setTimeout(hideToast, 7000);
  }

  function download(name, text, type) {
    const url = URL.createObjectURL(new Blob([text], { type }));
    const a = document.createElement('a');
    a.href = url;
    a.download = name;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1500);
  }

  // ---------- theme ----------
  const cssVar = name => getComputedStyle(document.documentElement).getPropertyValue(name).trim();
  const palette = () => ({ ink: cssVar('--ink'), ink2: cssVar('--ink-2'), ink3: cssVar('--ink-3'), wash: cssVar('--wash'), bg: cssVar('--bg') });

  function applyTheme(theme) {
    document.documentElement.dataset.theme = theme;
    store.set('mbb.theme', theme);
    $('theme').textContent = theme === 'dark' ? 'Light' : 'Dark';
    allCharts().forEach(styleChart);
  }

  // ---------- charts ----------
  // Overlay lines (threshold, fault marker, after-fault shading, hover crosshair) drawn on the canvas.
  const overlayPlugin = {
    id: 'mbbOverlay',
    beforeDatasetsDraw(chart) {
      const L = chart.$lines, S = chart.$style;
      if (!L || !S || !L.shade) return;
      const { ctx, chartArea: a, scales: { x } } = chart;
      ctx.save();
      for (const s of L.shade) {
        const x1 = Math.max(a.left, x.getPixelForValue(s.from));
        const x2 = Math.min(a.right, x.getPixelForValue(s.to));
        if (x2 <= x1) continue;
        ctx.fillStyle = S.wash;
        ctx.fillRect(x1, a.top, x2 - x1, a.bottom - a.top);
        if (s.label) {
          ctx.font = S.font;
          ctx.fillStyle = S.ink2;
          ctx.textBaseline = 'bottom';
          ctx.fillText(s.label, x1 + 6, a.bottom - 4);
        }
      }
      ctx.restore();
    },
    afterDatasetsDraw(chart) {
      const L = chart.$lines, S = chart.$style;
      if (!S) return;
      const { ctx, chartArea: a, scales: { x, y } } = chart;
      ctx.save();
      ctx.font = S.font;
      const labels = [];                     // drawn after all lines, so no line runs through a label
      if (L) {
        for (const h of L.h || []) {
          if (!isNum(h.y)) continue;
          const py = y.getPixelForValue(h.y);
          if (py < a.top - 1 || py > a.bottom + 1) continue;
          ctx.strokeStyle = S.ink;
          ctx.lineWidth = 1.5;
          ctx.setLineDash([6, 4]);
          ctx.beginPath(); ctx.moveTo(a.left, py); ctx.lineTo(a.right, py); ctx.stroke();
          ctx.setLineDash([]);
          if (h.label) {
            const w = ctx.measureText(h.label).width;
            labels.push([h.label, a.right - w - 6, py - 19 < a.top ? py + 4 : py - 19]);
          }
        }
        for (const v of L.v || []) {
          const px = x.getPixelForValue(v.x);
          if (px < a.left || px > a.right) continue;
          ctx.strokeStyle = S.ink;
          ctx.lineWidth = 2;
          ctx.beginPath(); ctx.moveTo(px, a.top); ctx.lineTo(px, a.bottom); ctx.stroke();
          if (v.label) {
            const w = ctx.measureText(v.label).width;
            labels.push([v.label, px + 8 + w > a.right ? px - w - 8 : px + 8, a.top + 2]);
          }
        }
      }
      for (const [text, lx, ly] of labels) {
        const w = ctx.measureText(text).width;
        const px = Math.max(a.left + 4, Math.min(lx, a.right - w - 4));
        ctx.fillStyle = S.bg;
        ctx.fillRect(px - 3, ly - 1, w + 6, 16);
        ctx.fillStyle = S.ink;
        ctx.textBaseline = 'top';
        ctx.fillText(text, px, ly + 1);
      }
      const act = chart.tooltip && chart.tooltip.getActiveElements ? chart.tooltip.getActiveElements() : [];
      if (act.length) {
        const px = act[0].element.x;
        ctx.strokeStyle = S.ink2;
        ctx.lineWidth = 1;
        ctx.beginPath(); ctx.moveTo(px, a.top); ctx.lineTo(px, a.bottom); ctx.stroke();
      }
      ctx.restore();
    },
  };
  Chart.register(overlayPlugin);
  Chart.defaults.font.family = "'IBM Plex Mono', ui-monospace, Consolas, monospace";
  Chart.defaults.font.size = 12;

  function makeChart(canvas, cfg) {
    const chart = new Chart(canvas, {
      type: 'line',
      data: { datasets: [{ data: [], borderWidth: 2, pointRadius: 0, pointHoverRadius: 5, pointHoverBorderWidth: 2, tension: 0, spanGaps: false, fill: cfg.fill ? 'origin' : false }] },
      options: {
        animation: false,
        responsive: true,
        maintainAspectRatio: false,
        parsing: false,
        normalized: true,
        interaction: { mode: 'nearest', axis: 'x', intersect: false },
        layout: { padding: { top: 6, right: 6 } },
        scales: {
          x: {
            type: 'linear',
            afterBuildTicks: cfg.xTicks ? axis => { const t = cfg.xTicks(); if (t) axis.ticks = t.map(value => ({ value })); } : undefined,
            ticks: { maxRotation: 0, autoSkip: false, align: 'inner', padding: 6, callback: v => cfg.xLabel(v, false) },
            grid: { drawTicks: false },
            border: { display: false },
          },
          y: {
            beginAtZero: !!cfg.zero,
            grace: '8%',
            afterFit: axis => { axis.width = 58; },     // same width on every chart, so the time axes line up
            // keep a minimum span, so a steady reading is not stretched into noise or repeated tick labels
            afterDataLimits: cfg.minSpan ? scale => {
              if (!isNum(scale.min) || !isNum(scale.max)) return;
              if (scale.max - scale.min >= cfg.minSpan) return;
              if (cfg.zero) { scale.max = Math.max(scale.max, scale.min + cfg.minSpan); return; }
              const c = (scale.max + scale.min) / 2;
              scale.min = c - cfg.minSpan / 2;
              scale.max = c + cfg.minSpan / 2;
            } : undefined,
            ticks: { maxTicksLimit: 5, padding: 8, callback: v => Number(v).toFixed(cfg.tickDecimals) },
            grid: { drawTicks: false },
            border: { display: false },
          },
        },
        plugins: {
          legend: { display: false },
          tooltip: {
            displayColors: false,
            callbacks: {
              title: items => (items.length ? cfg.xLabel(items[0].parsed.x, true) : ''),
              label: item => fmt(item.parsed.y, cfg.decimals) + ' ' + cfg.unit,
            },
          },
        },
      },
    });
    chart.$cfg = cfg;
    chart.$lines = { h: [], v: [], shade: [] };
    styleChart(chart);
    return chart;
  }

  function styleChart(chart) {
    const p = palette();
    const ds = chart.data.datasets[0];
    ds.borderColor = p.ink;
    ds.backgroundColor = p.wash;
    ds.pointHoverBackgroundColor = p.ink;
    ds.pointHoverBorderColor = p.bg;
    const o = chart.options;
    for (const ax of [o.scales.x, o.scales.y]) {
      ax.ticks.color = p.ink2;
      ax.grid.color = p.ink3;
    }
    o.plugins.tooltip.backgroundColor = p.ink;
    o.plugins.tooltip.titleColor = p.bg;
    o.plugins.tooltip.bodyColor = p.bg;
    chart.$style = { ink: p.ink, ink2: p.ink2, wash: p.wash, bg: p.bg, font: "500 12px 'IBM Plex Mono', ui-monospace, Consolas, monospace" };
    chart.update('none');
  }

  // live charts: x = device time in seconds; labels are relative to "now"
  const liveTicks = () => (st.latestT === null ? null : [-60, -50, -40, -30, -20, -10, 0].map(d => st.latestT + d));
  const liveLabel = (v, long) => {
    if (st.latestT === null) return '';
    const d = v - st.latestT;
    if (Math.abs(d) < (long ? 0.05 : 0.5)) return 'now';
    return '−' + Math.abs(d).toFixed(long ? 1 : 0) + ' s';
  };
  const liveCharts = {
    vib:  makeChart($('c-vib'),  { unit: 'g',  decimals: 4, tickDecimals: 3, zero: true,  fill: true,  minSpan: 0.02, xTicks: liveTicks, xLabel: liveLabel }),
    cur:  makeChart($('c-cur'),  { unit: 'A',  decimals: 3, tickDecimals: 2, zero: true,  fill: false, minSpan: 0.5,  xTicks: liveTicks, xLabel: liveLabel }),
    temp: makeChart($('c-temp'), { unit: '°C', decimals: 2, tickDecimals: 1, zero: false, fill: false, minSpan: 2,    xTicks: liveTicks, xLabel: liveLabel }),
  };
  let viewerCharts = [];
  const allCharts = () => Object.values(liveCharts).concat(viewerCharts);

  // ---------- incoming data ----------
  function pushPoint(key, x, y) {
    const arr = st.series[key];
    if (arr.length && x <= arr[arr.length - 1].x) return;
    arr.push({ x, y: isNum(y) ? y : NaN });
    const cutoff = x - LIVE_WINDOW_S - 2;
    let i = 0;
    while (i < arr.length && arr[i].x < cutoff) i++;
    if (i) arr.splice(0, i);
  }

  function clearLive() {
    for (const k of Object.keys(st.series)) st.series[k].length = 0;
    st.marks.length = 0;
    st.latestT = null;
  }

  function seen(via) {
    st.lastSeen = Date.now();
    st.via = via;
  }

  function onLive(m, via) {
    if (!isNum(m.t)) return;
    const t = m.t / 1000;
    if (st.latestT !== null && t < st.latestT - 2) {          // the ESP32 restarted
      clearLive();
      log('ESP32 restarted');
    }
    st.latestT = t;
    pushPoint('vib', t, m.vib);
    pushPoint('cur', t, m.cur);
    pushPoint('temp', t, m.temp);
    st.live = m;
    seen(via);
  }

  function onTrigger(m) {
    if (isNum(m.t)) st.marks.push({ x: m.t / 1000, label: 'FAULT · ' + m.reason });
    log('Fault detected (' + reasonText(m.reason) + '), record #' + m.id + ': recording 10 s more');
    toast('Fault detected: ' + reasonText(m.reason) + '. Recording 10 s more…');
  }

  function onEvent(e, retained) {
    if (!e || !Array.isArray(e.vib) || !isNum(e.id)) return;
    const key = (e.device || 'esp32') + ':' + e.id;
    if (st.records.some(r => r.key === key)) return;
    const rec = { key, receivedAt: Date.now(), retained: !!retained, data: e };
    st.records.unshift(rec);
    if (st.records.length > MAX_RECORDS) st.records.length = MAX_RECORDS;
    store.set('mbb.records', st.records);
    renderRecords();
    if (retained) { log('Loaded last fault record #' + e.id + ' from the broker'); return; }
    log('Fault record #' + e.id + ' received (' + (e.pre + e.post) + ' values)');
    if (st.autoOpen && !$('viewer').open) openViewer(key);
    else toast('Fault record #' + e.id + ' saved.', { label: 'View', run: () => openViewer(key) });
  }

  function onCalibration(m) {
    if (m.state === 'running') {
      if (isNum(m.seconds)) st.calSeconds = m.seconds;
      log('Calibrating baseline for ' + st.calSeconds + ' s');
      toast('Calibrating for ' + st.calSeconds + ' s. Keep the machine running normally.');
    } else if (m.state === 'done') {
      const text = 'Baseline set: normal level ' + fmt(m.mean, 4) + ' g, vibration threshold ' + fmt(m.threshold, 4) + ' g';
      log(text);
      toast(text);
      $('i-vib').dataset.dirty = '';
    }
  }

  function handleMessage(m, via, retained) {
    if (!m || typeof m !== 'object') return;
    if (m.device && !st.device) st.device = m.device;
    switch (m.type) {
      case 'live':        onLive(m, via); break;
      case 'health':      st.health = m; seen(via); break;
      case 'trigger':     onTrigger(m); break;
      case 'event':       onEvent(m, retained); break;
      case 'calibration': onCalibration(m); break;
      case 'ack':
        log((m.ok ? '' : 'Not done: ') + m.msg);
        toast((m.ok ? '' : 'Not done: ') + m.msg);
        break;
      default: break;
    }
  }

  function handleLine(line, via) {
    line = line.trim();
    if (!line) return;
    if (line[0] === '#') { log('ESP32: ' + line.slice(1).trim()); return; }
    if (line[0] !== '{') return;
    let m;
    try { m = JSON.parse(line); } catch (e) { return; }
    handleMessage(m, via, false);
  }

  // ---------- device state ----------
  function espState() {
    if (!st.linked) return { key: 'idle', word: 'NOT CONNECTED', sub: 'Choose USB cable or WiFi, then press Connect.' };
    if (st.mode === 'wifi' && st.lwt === 'offline' && st.lwtAt >= st.lastSeen) {
      return { key: 'offline', word: 'ESP32 OFFLINE', sub: 'The broker reports the ESP32 disconnected (power or WiFi lost).' };
    }
    if (!st.lastSeen) return { key: 'waiting', word: 'WAITING', sub: 'Linked. Waiting for data from the ESP32…' };
    const age = Date.now() - st.lastSeen;
    if (age > OFFLINE_AFTER_MS) return { key: 'offline', word: 'ESP32 OFFLINE', sub: 'No data for ' + Math.round(age / 1000) + ' s.' };
    const h = st.health;
    const parts = [st.via === 'usb' ? 'via USB cable' : 'via WiFi'];
    if (h) {
      if (st.via !== 'usb' && h.wifi) parts.push(h.rssi + ' dBm');
      parts.push('up ' + fmtUptime(h.uptime_s));
      if (h.ip) parts.push(h.ip);
      parts.push('fw ' + h.fw);
    }
    return { key: 'online', word: 'ESP32 ONLINE', sub: parts.join(' · ') };
  }
  const isOnline = () => espState().key === 'online';

  function changed(key, value, msg) {
    if (st.prev[key] === value) return;
    st.prev[key] = value;
    if (msg) log(msg);
  }

  // ---------- rendering ----------
  function renderDevice(e) {
    $('dev-state').textContent = e.word;
    $('dev-sub').textContent = e.sub;
    $('dev-ind').classList.toggle('on', e.key === 'online');
    changed('esp', e.key, e.key === 'online' ? 'ESP32 online ' + (st.via === 'usb' ? 'over USB' : 'over WiFi')
                     : e.key === 'offline' ? 'ESP32 offline: ' + e.sub : null);
  }

  function renderTiles(online) {
    const L = online ? st.live : null;
    const h = st.health || {};
    const thr = h.thr || {};
    const preS = h.pre_s || 30, postS = h.post_s || 10;

    $('k-vib').textContent = L ? fmt(L.vib, 3) : '—';
    const ratio = L && isNum(L.vib) && thr.vib > 0 ? L.vib / thr.vib : 0;
    $('k-vib-meter').style.width = Math.min(ratio, 1) * 100 + '%';
    $('k-vib-sub').textContent = thr.vib > 0
      ? 'Trigger at ' + fmt(thr.vib, 3) + ' g' + (L && isNum(L.vib) ? ' · ' + Math.round(ratio * 100) + '% of threshold' : '')
      : 'Threshold not set';

    $('k-cur').textContent = L ? fmt(L.cur, 2) : '—';
    const turns = isNum(h.turns) ? h.turns : 1;
    $('k-cur-sub').textContent = (thr.cur > 0 ? 'Trigger at ' + fmt(thr.cur, 2) + ' A' : 'Current trigger off') + ' · ' + turns + (turns === 1 ? ' turn' : ' turns') + ' through clamp';

    $('k-temp').textContent = L ? fmt(L.temp, 1) : '—';
    $('k-temp-sub').textContent = thr.temp > 0 ? 'Trigger at ' + fmt(thr.temp, 1) + ' °C' : 'Temperature trigger off';

    let word = '—', inv = false, pct = 0, sub = 'Waiting for the ESP32';
    if (L) {
      if (L.state === 'recording') {
        word = 'RECORDING'; inv = true; pct = 1 - L.left / postS;
        sub = 'Fault detected · ' + fmt(L.left, 1) + ' s more after the fault';
      } else if (L.state === 'cooldown') {
        word = 'SAVED'; pct = 1;
        sub = 'Record sent · ready again in ' + fmt(L.left, 1) + ' s';
      } else if (L.state === 'calibrating') {
        word = 'CALIBRATING'; inv = true; pct = 1 - L.left / st.calSeconds;
        sub = 'Keep the machine running normally · ' + fmt(L.left, 0) + ' s left';
      } else {
        word = 'MONITORING'; pct = Math.min((L.fill || 0) / preS, 1);
        sub = L.fill >= preS ? 'Last ' + preS + ' s buffered, ready to record' : 'Filling buffer: ' + fmt(L.fill, 1) + ' of ' + preS + ' s';
      }
    }
    $('k-rec').textContent = word;
    $('k-rec').classList.toggle('inv', inv);
    $('k-rec-meter').style.width = Math.max(0, Math.min(pct, 1)) * 100 + '%';
    $('k-rec-sub').textContent = sub;
  }

  function renderSensors(online) {
    const s = online && st.health ? st.health.sensors : null;
    const row = (id, ok, text) => {
      $(id + '-i').classList.toggle('on', !!ok);
      $(id).textContent = text;
    };
    if (!s) {
      row('s-mpu', false, '—'); row('s-ads', false, '—'); row('s-ds', false, '—');
      return;
    }
    const m = s.mpu6050 || {}, a = s.ads1115 || {}, d = s.ds18b20 || {};
    row('s-mpu', m.ok, m.ok ? 'Detected at ' + m.addr + ' · WHO_AM_I ' + m.who_am_i + (m.who_am_i !== '0x68' ? ' (clone chip)' : '') : 'Not detected on GPIO21/22');
    let adsText = 'Not detected on GPIO32/33';
    if (a.ok) {
      adsText = 'Detected · clamp bias ' + fmt(a.mid_v, 3) + ' V';
      if (isNum(a.mid_v) && (a.mid_v < 1.5 || a.mid_v > 1.8)) adsText += ' (expected about 1.65 V: check R1/R2)';
    }
    row('s-ads', a.ok, adsText);
    row('s-ds', d.ok, d.ok ? 'Detected on GPIO4' : 'Not detected on GPIO4');
    changed('mpu', !!m.ok, 'MPU6050 ' + (m.ok ? 'detected' : 'not detected'));
    changed('ads', !!a.ok, 'ADS1115 ' + (a.ok ? 'detected' : 'not detected'));
    changed('ds', !!d.ok, 'DS18B20 ' + (d.ok ? 'detected' : 'not detected'));
  }

  function renderControls(online) {
    const busy = st.live && st.live.state && st.live.state !== 'monitor';
    $('b-capture').disabled = !online || busy;
    $('b-cal').disabled = !online || busy;
    for (const id of ['i-vib', 'i-cur', 'i-temp', 'i-turns', 'b-apply']) $(id).disabled = !online;
    const h = st.health;
    if (h && h.thr) {
      const fill = (id, v, d) => { const el = $(id); if (!el.dataset.dirty && document.activeElement !== el && isNum(v)) el.value = v.toFixed(d); };
      fill('i-vib', h.thr.vib, 3);
      fill('i-cur', h.thr.cur, 2);
      fill('i-temp', h.thr.temp, 1);
      fill('i-turns', h.turns, 0);
    }
    $('ctl-hint').textContent = !online ? 'Connect to the ESP32 to use the controls.'
      : busy ? 'Recorder busy (' + st.live.state + '). Controls return when it finishes.'
      : 'Calibrate with the machine running normally (20 s). It sets the vibration threshold.';
  }

  function renderLiveCharts() {
    if (st.latestT === null) return;
    const lo = st.latestT - LIVE_WINDOW_S, hi = st.latestT;
    const thr = (st.health && st.health.thr) || {};
    st.marks = st.marks.filter(m => m.x >= lo - 1);
    const units = { vib: ['g', 3], cur: ['A', 2], temp: ['°C', 1] };
    for (const key of Object.keys(liveCharts)) {
      const ch = liveCharts[key];
      ch.data.datasets[0].data = st.series[key];
      ch.options.scales.x.min = lo;
      ch.options.scales.x.max = hi;
      const t = thr[key];
      const h = t > 0 ? [{ y: t, label: 'trigger ' + t.toFixed(units[key][1]) + ' ' + units[key][0] }] : [];
      if (key === 'vib') ch.options.scales.y.suggestedMax = t > 0 ? t * 1.25 : undefined;
      ch.$lines = { h, v: st.marks.map(m => ({ x: m.x, label: m.label })), shade: [] };
      ch.update('none');
    }
  }

  function renderRecords() {
    const list = $('records');
    list.textContent = '';
    $('records-empty').hidden = st.records.length > 0;
    for (const r of st.records) {
      const e = r.data;
      const li = document.createElement('li');
      const b = document.createElement('button');
      b.type = 'button';
      const id = document.createElement('span'); id.className = 'rec-id'; id.textContent = '#' + e.id;
      const rs = document.createElement('span'); rs.className = 'rec-reason'; rs.textContent = e.reason;
      const tm = document.createElement('span'); tm.className = 'rec-time'; tm.textContent = r.retained ? 'from broker' : timeStr(r.receivedAt);
      const pk = document.createElement('span'); pk.className = 'rec-peak';
      const p = e.peak || {};
      pk.textContent = 'peak ' + fmt(p.vib, 3) + ' g · ' + fmt(p.cur, 2) + ' A · ' + fmt(p.temp, 1) + ' °C';
      b.append(id, rs, tm, pk);
      b.addEventListener('click', () => openViewer(r.key));
      li.append(b);
      list.append(li);
    }
  }

  function renderAll() {
    const e = espState();
    const online = e.key === 'online';
    renderDevice(e);
    renderTiles(online);
    renderSensors(online);
    renderControls(online);
    renderLiveCharts();
    $('clock').textContent = timeStr(Date.now());
  }

  // ---------- fault record viewer ----------
  let viewerKey = null;

  function recordSeries(e) {
    const n = e.vib.length, rate = e.rate_hz || 10, trig = (e.pre || n) - 1;
    const xs = [];
    for (let i = 0; i < n; i++) xs.push(Math.round(((i - trig) / rate) * 10) / 10);
    const mk = arr => xs.map((x, i) => ({ x, y: isNum(arr && arr[i]) ? arr[i] : NaN }));
    return { xs, trig, rate, vib: mk(e.vib), cur: mk(e.cur), temp: mk(e.temp) };
  }

  function openViewer(key) {
    const rec = st.records.find(r => r.key === key);
    if (!rec) return;
    viewerKey = key;
    const e = rec.data;
    const s = recordSeries(e);
    const preS = (e.pre / s.rate), postS = (e.post / s.rate);
    $('v-eyebrow').textContent = 'Trigger: ' + reasonText(e.reason);
    $('v-title').textContent = 'Fault record #' + e.id;
    $('v-sub').textContent = (rec.retained ? 'Loaded from the broker' : 'Captured at ' + timeStr(rec.receivedAt - postS * 1000))
      + ' · ' + preS.toFixed(0) + ' s before and ' + postS.toFixed(0) + ' s after · ' + s.rate + ' values per second';

    const dlg = $('viewer');
    hideToast();
    if (!dlg.open) dlg.showModal();
    destroyViewerCharts();

    const xLabel = (v, long) => {
      if (Math.abs(v) < 0.05) return long ? 'fault (0 s)' : 'fault';
      return (v > 0 ? '+' : '−') + Math.abs(v).toFixed(long ? 1 : 0) + ' s';
    };
    const lo = s.xs[0], hi = s.xs[s.xs.length - 1];
    const thr = e.thr || {};
    const defs = [
      ['v-vib',  'vib',  { unit: 'g',  decimals: 4, tickDecimals: 3, zero: true,  fill: true,  minSpan: 0.02 }, thr.vib,  3],
      ['v-cur',  'cur',  { unit: 'A',  decimals: 3, tickDecimals: 2, zero: true,  fill: false, minSpan: 0.5  }, thr.cur,  2],
      ['v-temp', 'temp', { unit: '°C', decimals: 2, tickDecimals: 1, zero: false, fill: false, minSpan: 1    }, thr.temp, 1],
    ];
    requestAnimationFrame(() => {
      for (const [canvasId, key, cfg, t, d] of defs) {
        const ch = makeChart($(canvasId), Object.assign({ xLabel }, cfg));
        ch.data.datasets[0].data = s[key];
        ch.options.scales.x.min = lo;
        ch.options.scales.x.max = hi;
        if (key === 'vib' && t > 0) ch.options.scales.y.suggestedMax = t * 1.25;
        ch.$lines = {
          h: t > 0 ? [{ y: t, label: 'trigger ' + t.toFixed(d) + ' ' + cfg.unit }] : [],
          v: [{ x: 0, label: key === 'vib' ? 'FAULT · ' + e.reason : 'FAULT' }],
          shade: [{ from: 0, to: hi, label: key === 'temp' ? 'after fault' : '' }],
        };
        ch.update('none');
        viewerCharts.push(ch);
      }
    });

    // summary table
    const tb = $('v-tbody');
    tb.textContent = '';
    const rows = [['Vibration', e.vib, 'g', 3], ['Current', e.cur, 'A', 2], ['Temperature', e.temp, '°C', 1]];
    for (const [name, arr, unit, d] of rows) {
      const before = (arr || []).slice(0, s.trig).filter(isNum);
      const avg = before.length ? before.reduce((a, b) => a + b, 0) / before.length : NaN;
      const atTrig = arr ? arr[s.trig] : NaN;
      const all = (arr || []).filter(isNum);
      const peak = all.length ? Math.max(...all) : NaN;
      const tr = document.createElement('tr');
      const cells = [name, fmt(avg, d) + (isNum(avg) ? ' ' + unit : ''), fmt(atTrig, d) + (isNum(atTrig) ? ' ' + unit : ''), fmt(peak, d) + (isNum(peak) ? ' ' + unit : '')];
      cells.forEach((c, i) => { const el = document.createElement(i ? 'td' : 'th'); el.textContent = c; tr.append(el); });
      tb.append(tr);
    }
    const thrBits = [];
    if (thr.vib > 0) thrBits.push('vibration ' + thr.vib.toFixed(3) + ' g');
    if (thr.cur > 0) thrBits.push('current ' + thr.cur.toFixed(2) + ' A');
    if (thr.temp > 0) thrBits.push('temperature ' + thr.temp.toFixed(1) + ' °C');
    $('v-note').textContent = 'Thresholds at the time: ' + (thrBits.length ? thrBits.join(', ') : 'none') + '. The shaded part is after the fault. Gaps mean a sensor was not reporting.';
  }

  function destroyViewerCharts() {
    viewerCharts.forEach(c => c.destroy());
    viewerCharts = [];
  }

  $('v-close').addEventListener('click', () => $('viewer').close());
  $('viewer').addEventListener('close', destroyViewerCharts);
  $('v-csv').addEventListener('click', () => {
    const rec = st.records.find(r => r.key === viewerKey);
    if (!rec) return;
    const e = rec.data, s = recordSeries(e);
    const cell = (arr, i, d) => (isNum(arr && arr[i]) ? arr[i].toFixed(d) : '');
    let csv = 't_s,vibration_g,current_a,temperature_c\n';
    for (let i = 0; i < s.xs.length; i++) csv += s.xs[i].toFixed(1) + ',' + cell(e.vib, i, 4) + ',' + cell(e.cur, i, 3) + ',' + cell(e.temp, i, 2) + '\n';
    download('mbb-fault-' + e.id + '-' + e.reason + '.csv', csv, 'text/csv');
  });
  $('v-json').addEventListener('click', () => {
    const rec = st.records.find(r => r.key === viewerKey);
    if (!rec) return;
    download('mbb-fault-' + rec.data.id + '-' + rec.data.reason + '.json', JSON.stringify(rec.data, null, 1), 'application/json');
  });

  // ---------- sending commands ----------
  let port = null, reader = null, pipeDone = null, client = null;

  async function sendCmd(text) {
    if (!isOnline()) { toast('The ESP32 is not online.'); return; }
    try {
      if (st.mode === 'usb' && port && port.writable) {
        const w = port.writable.getWriter();
        await w.write(new TextEncoder().encode(text + '\n'));
        w.releaseLock();
      } else if (st.mode === 'wifi' && client && st.device) {
        client.publish('mbb/' + st.device + '/cmd', text);
      } else {
        toast('No link to the ESP32.');
        return;
      }
      log('Sent: ' + text);
    } catch (e) {
      log('Could not send "' + text + '": ' + (e.message || e));
    }
  }
  const pause = ms => new Promise(r => setTimeout(r, ms));

  $('b-capture').addEventListener('click', () => sendCmd('capture'));
  $('b-cal').addEventListener('click', () => sendCmd('calibrate'));
  for (const id of ['i-vib', 'i-cur', 'i-temp', 'i-turns']) $(id).addEventListener('input', () => { $(id).dataset.dirty = '1'; });
  $('thr-form').addEventListener('submit', async ev => {
    ev.preventDefault();
    const h = st.health || {}, thr = h.thr || {};
    const cmds = [];
    const num = id => parseFloat($(id).value);
    if (isNum(num('i-vib')) && num('i-vib') !== thr.vib) cmds.push('set vib ' + num('i-vib'));
    if (isNum(num('i-cur')) && num('i-cur') !== thr.cur) cmds.push('set cur ' + num('i-cur'));
    if (isNum(num('i-temp')) && num('i-temp') !== thr.temp) cmds.push('set temp ' + num('i-temp'));
    if (isNum(num('i-turns')) && Math.round(num('i-turns')) !== h.turns) cmds.push('turns ' + Math.round(num('i-turns')));
    for (const id of ['i-vib', 'i-cur', 'i-temp', 'i-turns']) $(id).dataset.dirty = '';
    if (!cmds.length) { toast('No changes to apply.'); return; }
    for (const c of cmds) { await sendCmd(c); await pause(250); }
  });
  $('auto-open').checked = !!st.autoOpen;
  $('auto-open').addEventListener('change', () => { st.autoOpen = $('auto-open').checked; store.set('mbb.autoOpen', st.autoOpen); });

  // ---------- USB (Web Serial) ----------
  async function connectUsb() {
    if (!('serial' in navigator)) {
      setLink('USB needs Chrome or Edge');
      log('This browser has no Web Serial. Open the page in Chrome or Edge from http://localhost.');
      return;
    }
    try {
      port = await navigator.serial.requestPort();
      await port.open({ baudRate: SERIAL_BAUD, bufferSize: 65536 });
      try { await port.setSignals({ dataTerminalReady: false, requestToSend: false }); } catch (e) { /* not supported */ }
    } catch (e) {
      log(e && e.name === 'NotFoundError' ? 'No port chosen' : 'Could not open the port: ' + (e.message || e) + ' (close Arduino Serial Monitor first)');
      port = null;
      return;
    }
    st.linked = true;
    log('Opened USB serial port at ' + SERIAL_BAUD + ' baud');
    readUsb();
  }

  async function readUsb() {
    const decoder = new TextDecoderStream();
    pipeDone = port.readable.pipeTo(decoder.writable).catch(() => {});
    reader = decoder.readable.getReader();
    let buf = '';
    try {
      for (;;) {
        const { value, done } = await reader.read();
        if (done) break;
        buf += value;
        let n;
        while ((n = buf.indexOf('\n')) >= 0) {
          handleLine(buf.slice(0, n), 'usb');
          buf = buf.slice(n + 1);
        }
        if (buf.length > 200000) buf = '';
      }
    } catch (e) {
      log('USB read stopped: ' + (e.message || e));
    }
  }

  async function disconnectUsb() {
    try { if (reader) await reader.cancel(); } catch (e) { /* already closed */ }
    try { if (pipeDone) await pipeDone; } catch (e) { /* already closed */ }
    try { if (port) await port.close(); } catch (e) { /* already closed */ }
    reader = null; pipeDone = null; port = null;
  }

  if ('serial' in navigator) {
    navigator.serial.addEventListener('disconnect', ev => {
      if (port && ev.target === port) {
        log('USB cable unplugged');
        st.linked = false;
        disconnectUsb().then(() => { setButtons(); });
      }
    });
  }

  // ---------- WiFi (MQTT over WebSocket) ----------
  function connectWifi() {
    if (typeof mqtt === 'undefined') { log('lib/mqtt.min.js is missing.'); return; }
    const url = $('broker').value.trim();
    log('Connecting to ' + url);
    client = mqtt.connect(url, {
      clientId: 'mbb-dashboard-' + Math.random().toString(16).slice(2, 8),
      reconnectPeriod: 2000,
      connectTimeout: 4000,
      clean: true,
    });
    client.on('connect', () => {
      st.linked = true;
      log('Connected to the MQTT broker');
      client.subscribe('mbb/+/#', { qos: 0 });
    });
    client.on('message', (topic, payload, packet) => {
      const parts = topic.split('/');
      if (parts.length < 3 || parts[0] !== 'mbb') return;
      const dev = parts[1], kind = parts[2];
      if (st.device && dev !== st.device) return;
      if (!st.device) st.device = dev;
      if (kind === 'cmd') return;
      const text = payload.toString();
      if (kind === 'status') { st.lwt = text.trim(); st.lwtAt = Date.now(); return; }
      let m;
      try { m = JSON.parse(text); } catch (e) { return; }
      handleMessage(m, 'wifi', !!(packet && packet.retain));
    });
    client.on('offline', () => { if (st.linked) log('Lost the MQTT broker'); st.linked = false; });
    client.on('error', err => log('Broker error: ' + (err.message || err)));
  }

  function disconnectWifi() {
    if (client) { client.end(true); client = null; }
  }

  // ---------- connection controls ----------
  function setLink(text) { $('dev-sub').textContent = text; }

  function setButtons() {
    const active = st.mode === 'usb' ? !!port : !!client;
    $('connect').textContent = active ? 'Disconnect' : 'Connect';
    $('connect').classList.toggle('primary', !active);
    $('mode-usb').disabled = $('mode-wifi').disabled = active;
    $('broker').disabled = active;
  }

  function setMode(m) {
    st.mode = m;
    $('mode-usb').setAttribute('aria-pressed', String(m === 'usb'));
    $('mode-wifi').setAttribute('aria-pressed', String(m === 'wifi'));
    $('broker').hidden = m !== 'wifi';
    store.set('mbb.mode', m);
  }

  function resetSession() {
    st.linked = false; st.lastSeen = 0; st.health = null; st.live = null; st.lwt = null; st.device = null;
    clearLive();
  }

  let busy = false;
  $('connect').addEventListener('click', async () => {
    if (busy) return;
    busy = true;
    const active = st.mode === 'usb' ? !!port : !!client;
    if (active) {
      if (st.mode === 'usb') await disconnectUsb(); else disconnectWifi();
      resetSession();
      log('Disconnected');
    } else {
      resetSession();
      if (st.mode === 'usb') await connectUsb(); else connectWifi();
    }
    busy = false;
    setButtons();
    renderAll();
  });
  $('mode-usb').addEventListener('click', () => setMode('usb'));
  $('mode-wifi').addEventListener('click', () => setMode('wifi'));
  $('theme').addEventListener('click', () => applyTheme(document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark'));

  // ---------- start ----------
  const savedBroker = store.get('mbb.broker', null);
  if (savedBroker) $('broker').value = savedBroker;
  $('broker').addEventListener('change', () => store.set('mbb.broker', $('broker').value.trim()));
  setMode(store.get('mbb.mode', 'usb'));
  applyTheme(store.get('mbb.theme', window.matchMedia && window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light'));
  renderRecords();
  setButtons();
  renderAll();
  setInterval(renderAll, 250);
  if (document.fonts && document.fonts.ready) document.fonts.ready.then(() => allCharts().forEach(c => c.update('none')));
})();
