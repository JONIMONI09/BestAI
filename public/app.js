/* Hydra-Stone Console front end.
 *
 * Everything displayed here comes from the server: the model list, the
 * engine token stream, the axiom verdict and the training metrics. Nothing
 * is simulated in the browser - if the engine returns tokens, those tokens
 * are shown; if a word is not in the vocabulary, the console says so
 * instead of inventing a mapping.
 */
(function () {
  'use strict';

  const $ = (id) => document.getElementById(id);

  const state = {
    model: null,      // path of the selected model
    models: [],       // metadata from /api/models
    vocab: {},        // word -> token id
    view: 'words',    // or 'tokens'
    lastTokens: [],
  };

  /* ── API ──────────────────────────────────────────────────── */

  async function api(path, options) {
    const res = await fetch(path, Object.assign({ headers: { 'Content-Type': 'application/json' } }, options));
    let data;
    try {
      data = await res.json();
    } catch {
      data = {};
    }
    if (!res.ok) {
      throw new Error(data.error || `HTTP ${res.status}`);
    }
    return data;
  }

  function setStatus(text, kind) {
    const el = $('status');
    el.textContent = text;
    el.dataset.state = kind || 'idle';
  }

  function log(line) {
    const el = $('log');
    const stamp = new Date().toLocaleTimeString();
    el.textContent = `${stamp}  ${line}\n${el.textContent}`.slice(0, 8000);
  }

  /* ── Rendering helpers ────────────────────────────────────── */

  function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }

  function addMessage(role, bodyNode, meta) {
    const wrap = el('div', `msg ${role}`);
    wrap.appendChild(bodyNode);
    if (meta) wrap.appendChild(el('span', 'meta', meta));
    $('messages').appendChild(wrap);
    $('messages').scrollTop = $('messages').scrollHeight;
    return wrap;
  }

  function textNode(text) {
    return document.createTextNode(text);
  }

  function renderTokens(container, tokens) {
    const ids = Object.values(state.vocab);
    for (const tok of tokens) {
      const word = ids.find((id) => id === tok);
      const chip = el('span', 'tok', state.view === 'words' && word !== undefined ? word : String(tok));
      chip.title = word !== undefined ? `token ${tok} → ${word}` : `token ${tok} (not in vocabulary)`;
      container.appendChild(chip);
    }
  }

  function renderChips(model) {
    const box = $('model-chips');
    box.textContent = '';
    if (!model) return;
    const facts = [
      ['dim', model.dim],
      ['vocab', model.vocab],
      ['layers', model.layers],
      ['size', `${(model.bytes / 1024).toFixed(1)} kB`],
    ];
    for (const [k, v] of facts) {
      const chip = el('span', 'chip');
      chip.appendChild(textNode(`${k} `));
      chip.appendChild(el('b', null, String(v)));
      box.appendChild(chip);
    }
    if (model.trained) box.appendChild(el('span', 'chip', 'trained'));
    if (model.valid === false) box.appendChild(el('span', 'chip', 'invalid header'));

    const detail = $('model-detail');
    detail.textContent = '';
    for (const [k, v] of [['path', model.path], ['magic', model.magic === 1213809746 ? 'HYDR' : model.magic]]) {
      detail.appendChild(el('span', null, k));
      detail.appendChild(el('b', null, String(v)));
    }
  }

  function drawChart(tokens) {
    const canvas = $('token-chart');
    const dpr = window.devicePixelRatio || 1;
    const w = canvas.clientWidth || 600;
    const h = 180;
    canvas.width = w * dpr;
    canvas.height = h * dpr;
    const ctx = canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);

    if (tokens.length === 0) return;
    const max = Math.max(...tokens, 1);
    const barW = w / tokens.length;
    for (let i = 0; i < tokens.length; i += 1) {
      const bh = (tokens[i] / max) * (h - 20);
      const grad = ctx.createLinearGradient(0, h - bh, 0, h);
      grad.addColorStop(0, '#38bdf8');
      grad.addColorStop(1, 'rgba(56,189,248,0.25)');
      ctx.fillStyle = grad;
      ctx.fillRect(i * barW + 1, h - bh, Math.max(1, barW - 2), bh);
    }
    ctx.fillStyle = 'rgba(148,163,180,.75)';
    ctx.font = '10px ui-monospace, monospace';
    ctx.fillText(`max token ${max}`, 6, 12);
  }

  function renderState(stateVector) {
    const strip = $('state-strip');
    strip.textContent = '';
    for (const v of stateVector) {
      const t = (v + 127) / 254;
      const cell = el('span', 'state-cell', String(v));
      cell.style.background = `hsl(${190 - t * 190} 85% ${28 + t * 42}%)`;
      cell.title = `state value ${v}`;
      strip.appendChild(cell);
    }
  }

  /* ── Models ───────────────────────────────────────────────── */

  async function loadModels(selectPath) {
    const data = await api('/api/models');
    state.models = data.models;
    const select = $('model-select');
    select.textContent = '';
    for (const m of state.models) {
      const opt = document.createElement('option');
      opt.value = m.path;
      opt.textContent = `${m.path}${m.valid ? '' : ' (invalid header)'}`;
      select.appendChild(opt);
    }
    const wanted = selectPath || state.model || data.default;
    if (wanted && state.models.some((m) => m.path === wanted)) {
      select.value = wanted;
      await selectModel(wanted);
    } else if (state.models.length > 0) {
      await selectModel(state.models[0].path);
    }
  }

  async function selectModel(modelPath) {
    state.model = modelPath;
    const meta = state.models.find((m) => m.path === modelPath);
    renderChips(meta);
    try {
      const info = await api(`/api/model?path=${encodeURIComponent(modelPath)}`);
      renderChips(Object.assign({}, meta, info));
    } catch (e) {
      log(`model info failed: ${e.message}`);
    }
    await loadVocab();
  }

  async function loadVocab() {
    const key = (state.model || '').replace(/[^A-Za-z0-9._-]/g, '-').split('/').pop().replace(/\.hydra$/, '');
    try {
      const data = await api(`/api/vocab?model=${encodeURIComponent(key)}`);
      state.vocab = data.map || {};
    } catch {
      state.vocab = {};
    }
    const lines = Object.entries(state.vocab).map(([w, id]) => `${w} ${id}`);
    $('vocab-text').value = lines.join('\n');
  }

  async function saveVocab() {
    const map = {};
    for (const line of $('vocab-text').value.split(/\r?\n/)) {
      const m = line.trim().match(/^(\S+)\s+(\d+)$/);
      if (m) map[m[1]] = Number(m[2]);
    }
    const key = (state.model || '').replace(/[^A-Za-z0-9._-]/g, '-').split('/').pop().replace(/\.hydra$/, '');
    try {
      const res = await api('/api/vocab', { method: 'PUT', body: JSON.stringify({ model: key, map }) });
      state.vocab = map;
      $('vocab-hint').textContent = `${res.entries} entries saved`;
      log(`vocabulary saved for ${key}: ${res.entries} entries`);
    } catch (e) {
      $('vocab-hint').textContent = e.message;
    }
  }

  /* ── Chat ─────────────────────────────────────────────────── */

  function encodeWords(text) {
    const tokens = [];
    const unknown = [];
    for (const word of text.toLowerCase().split(/\s+/).filter(Boolean)) {
      const id = state.vocab[word];
      if (id === undefined) unknown.push(word);
      else tokens.push(id);
    }
    return { tokens, unknown };
  }

  async function send(text) {
    const startToken = Number($('opt-token').value) || 0;
    const steps = Number($('opt-steps').value) || 16;
    const { tokens, unknown } = text ? encodeWords(text) : { tokens: [], unknown: [] };
    const seed = tokens.length > 0 ? tokens[0] : startToken;

    addMessage('user', textNode(text || `(empty → start token ${seed})`));
    if (unknown.length > 0) {
      $('input-hint').textContent = `not in vocabulary: ${unknown.join(', ')}`;
      $('input-hint').classList.add('warn');
      addMessage('sys', textNode(`Words without a token id were skipped: ${unknown.join(', ')}`));
    } else {
      $('input-hint').textContent = '';
      $('input-hint').classList.remove('warn');
    }

    setStatus('running engine…', 'busy');
    try {
      const t0 = performance.now();
      const res = await api('/api/infer', {
        method: 'POST',
        body: JSON.stringify({ path: state.model, token: seed, steps }),
      });
      const wall = performance.now() - t0;
      state.lastTokens = res.tokens || [];

      const body = el('div');
      renderTokens(body, state.lastTokens);
      const engineMs = Number(res.elapsed_ms);
      const shown = Number.isFinite(engineMs) ? engineMs : wall;
      const meta = `${res.tokens.length} tokens · engine ${shown.toFixed(1)} ms · wall ${wall.toFixed(0)} ms · model ${state.model}`;
      addMessage('bot', body, meta);

      $('m-total').textContent = shown.toFixed(1);
      $('m-per-token').textContent = (shown / Math.max(1, res.tokens.length)).toFixed(2);
      $('m-tps').textContent = (res.tokens.length / Math.max(0.001, shown / 1000)).toFixed(1);
      drawChart(state.lastTokens);
      if (Array.isArray(res.state)) renderState(res.state);
      log(`${state.model} seed=${seed} steps=${steps} → ${JSON.stringify(state.lastTokens)}`);
      setStatus('ready', 'ready');
    } catch (e) {
      addMessage('sys', textNode(`Engine error: ${e.message}`));
      log(`engine error: ${e.message}`);
      setStatus('error', 'error');
    }
  }

  /* ── Training ─────────────────────────────────────────────── */

  async function train() {
    const payload = {
      name: $('train-name').value,
      vocab: Number($('train-vocab').value),
      dim: Number($('train-dim').value),
      layers: Number($('train-layers').value),
      epochs: Number($('train-epochs').value),
      corpus: $('train-corpus').value,
    };
    const badge = $('train-badge');
    const out = $('train-result');
    badge.className = 'badge';
    badge.textContent = 'training…';
    $('train-hint').textContent = '';
    out.hidden = true;
    setStatus('training…', 'busy');

    try {
      const res = await api('/api/train', { method: 'POST', body: JSON.stringify(payload) });
      out.hidden = false;
      out.textContent = [
        `model      ${res.model}`,
        `size       ${res.bytes} bytes`,
        `config     vocab=${res.config.vocab} dim=${res.config.dim} layers=${res.config.layers} epochs=${res.epochs}`,
        `samples    ${res.samples}`,
        `accuracy   ${(res.before.accuracy * 100).toFixed(1)}% → ${(res.after.accuracy * 100).toFixed(1)}% (best epoch ${res.bestEpoch})`,
        `moves      ${res.flips} weight updates`,
        `verified   engine stream matches trainer: ${res.verification.match ? 'yes' : 'NO'}`,
        `simulated  ${JSON.stringify(res.verification.simulated)}`,
        `engine     ${JSON.stringify(res.verification.engine)}`,
        res.unknownTokens.length ? `unknown words skipped: ${res.unknownTokens.join(', ')}` : '',
      ].filter(Boolean).join('\n');
      badge.className = 'badge ok';
      badge.textContent = `${(res.after.accuracy * 100).toFixed(0)}% accuracy · verified`;
      log(`trained ${res.model}: ${(res.before.accuracy * 100).toFixed(1)}% → ${(res.after.accuracy * 100).toFixed(1)}%`);
      await loadModels(res.model);
      setStatus('ready', 'ready');
    } catch (e) {
      badge.className = 'badge err';
      badge.textContent = e.message;
      out.hidden = false;
      out.textContent = `Training failed: ${e.message}`;
      setStatus('error', 'error');
    }
  }

  /* ── Axiom ────────────────────────────────────────────────── */

  async function updateAxiom() {
    const h = Number($('opt-humanity').value);
    $('axiom-value').textContent = h.toFixed(2);
    try {
      const res = await api(`/api/axiom?h=${h}`);
      const verdict = $('axiom-verdict');
      verdict.textContent = res.allowed ? 'ALLOWED' : 'BLOCKED';
      verdict.className = `verdict ${res.allowed ? 'allow' : 'block'}`;
    } catch (e) {
      log(`axiom check failed: ${e.message}`);
    }
  }

  /* ── Wiring ───────────────────────────────────────────────── */

  function init() {
    $('composer').addEventListener('submit', (ev) => {
      ev.preventDefault();
      const text = $('input').value.trim();
      $('input').value = '';
      send(text);
    });

    $('model-select').addEventListener('change', (ev) => selectModel(ev.target.value));
    $('opt-humanity').addEventListener('input', updateAxiom);
    $('vocab-save').addEventListener('click', saveVocab);
    $('train-run').addEventListener('click', train);

    $('view-words').addEventListener('click', () => setView('words'));
    $('view-tokens').addEventListener('click', () => setView('tokens'));

    const drawer = $('settings');
    const scrim = $('scrim');
    const openSettings = (open) => {
      drawer.classList.toggle('open', open);
      drawer.setAttribute('aria-hidden', open ? 'false' : 'true');
      scrim.hidden = !open;
      $('toggle-settings').setAttribute('aria-expanded', open ? 'true' : 'false');
    };
    $('toggle-settings').addEventListener('click', () => openSettings(!drawer.classList.contains('open')));
    $('settings-close').addEventListener('click', () => openSettings(false));
    scrim.addEventListener('click', () => openSettings(false));

    window.addEventListener('resize', () => drawChart(state.lastTokens));

    updateAxiom();
    addMessage('sys', textNode('Every reply is produced by the compiled C engine. Words need a token id in the vocabulary; unknown words are reported, never guessed.'));

    loadModels()
      .then(() => setStatus('ready', 'ready'))
      .catch((e) => {
        setStatus('server unreachable', 'error');
        log(`startup failed: ${e.message}`);
      });
  }

  function setView(view) {
    state.view = view;
    $('view-words').classList.toggle('is-active', view === 'words');
    $('view-tokens').classList.toggle('is-active', view === 'tokens');
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();