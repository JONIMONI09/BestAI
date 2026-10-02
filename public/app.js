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
    idToWord: {},     // token id -> word (the reverse lookup; see loadVocab)
    view: 'words',    // or 'tokens'
    lastTokens: [],
    inflight: null,   // AbortController of the running request, if any
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

  function textNode(text) {
    return document.createTextNode(text);
  }

  function addMessage(role, bodyNode, meta) {
    const wrap = el('div', `msg ${role}`);
    wrap.appendChild(bodyNode);
    if (meta) wrap.appendChild(el('span', 'meta', meta));
    $('messages').appendChild(wrap);
    $('messages').scrollTop = $('messages').scrollHeight;
    return wrap;
  }

  /**
   * Render one token per chip into `container`.
   *
   * The vocabulary is a word -> id map, so looking a token id up in
   * Object.values() only ever finds the id itself: "words" mode used to
   * print numbers. The reverse map is built once in loadVocab().
   */
  function renderTokens(container, tokens) {
    const inWords = state.view === 'words';
    for (const tok of tokens) {
      const word = state.idToWord[tok];
      const known = word !== undefined;
      /* An unknown token is shown muted, not as if it were a word. */
      const chip = el('span', inWords && known ? 'tok' : (inWords ? 'tok unknown' : 'tok'), inWords && known ? word : String(tok));
      chip.title = known ? `token ${tok} → ${word}` : `token ${tok} (not in vocabulary)`;
      container.appendChild(chip);
    }
  }

  /* Every bot message remembers its tokens so the Words/Tokens toggle can
   * re-render it. Without this the toggle only changed the button state
   * and the already-rendered messages kept the old representation. */
  const botMessages = [];

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
    if (model.uploaded) box.appendChild(el('span', 'chip', 'uploaded'));
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

    if (tokens.length === 0) {
      $('chart-summary').textContent = 'No token stream yet.';
      return;
    }
    const max = Math.max(...tokens);
    const min = Math.min(...tokens);
    /* Text alternative: the canvas pixels mean nothing to a screen reader,
     * the three numbers do. */
    $('chart-summary').textContent =
      `Token chart: ${tokens.length} tokens, minimum ${min}, maximum ${max}.`;

    const barW = w / tokens.length;
    for (let i = 0; i < tokens.length; i += 1) {
      const bh = (Math.max(tokens[i], 0) / max) * (h - 20);
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
      /* The number sits ON the colour, so the colour has to stay dark
       * enough for the text to be readable: lightness 28%..46% against
       * #04222f. */
      cell.style.background = `hsl(${190 - t * 190} 85% ${28 + t * 18}%)`;
      cell.title = `state value ${v}`;
      cell.setAttribute('role', 'listitem');
      cell.setAttribute('aria-label', `state value ${v}`);
      strip.appendChild(cell);
    }
    $('state-summary').textContent =
      `State vector: ${stateVector.length} values between ${Math.min(...stateVector)} and ${Math.max(...stateVector)}.`;
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
    /* The reverse map is built HERE, once per vocabulary load, not per
     * rendered token. */
    state.idToWord = Object.fromEntries(
      Object.entries(state.vocab).map(([word, id]) => [Number(id), word])
    );
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
      state.idToWord = Object.fromEntries(Object.entries(map).map(([w, id]) => [Number(id), w]));
      $('vocab-hint').textContent = `${res.entries} entries saved`;
      log(`vocabulary saved for ${key}: ${res.entries} entries`);
    } catch (e) {
      $('vocab-hint').textContent = e.message;
    }
  }

  /* ── Model upload ─────────────────────────────────────────── */

  async function uploadModel(file) {
    const name = $('upload-name').value.trim();
    if (!name.toLowerCase().endsWith('.hydra')) {
      $('upload-hint').textContent = 'the name must end in .hydra';
      return false;
    }
    $('upload-hint').textContent = 'validating…';
    $('upload-open').disabled = true;
    try {
      const res = await fetch(`/api/models/upload?name=${encodeURIComponent(name)}`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/octet-stream' },
        body: file,
      });
      const data = await res.json().catch(() => ({}));
      if (!res.ok) throw new Error(data.error || `HTTP ${res.status}`);
      $('upload-hint').textContent = `stored ${data.model.path}`;
      await loadModels(data.model.path);
      return true;
    } catch (e) {
      $('upload-hint').textContent = e.message;
      return false;
    } finally {
      $('upload-open').disabled = false;
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

  /* One request at a time. Two concurrent /api/infer calls could interleave
   * their responses and the second one would overwrite the first one's
   * token stream. */
  function setBusy(busy) {
    $('send').disabled = busy;
    $('train-run').disabled = busy;
    $('cancel').hidden = !busy;
    $('input').setAttribute('aria-busy', busy ? 'true' : 'false');
  }

  async function send(text) {
    const startToken = Number($('opt-token').value) || 0;
    const steps = Number($('opt-steps').value) || 16;
    const { tokens, unknown } = text ? encodeWords(text) : { tokens: [], unknown: [] };
    /* The WHOLE prompt goes to the engine, not just its first token. */
    const prompt = tokens.length > 0 ? tokens : [startToken];

    addMessage('user', textNode(text || `(empty → prompt ${prompt.join(', ')})`));
    if (unknown.length > 0) {
      $('input-hint').textContent = `not in vocabulary: ${unknown.join(', ')}`;
      $('input-hint').classList.add('warn');
      addMessage('sys', textNode(`Words without a token id were skipped: ${unknown.join(', ')}`));
    } else {
      $('input-hint').textContent = '';
      $('input-hint').classList.remove('warn');
    }

    const controller = new AbortController();
    state.inflight = controller;
    setBusy(true);
    setStatus('running engine…', 'busy');
    try {
      const t0 = performance.now();
      const res = await api('/api/infer', {
        method: 'POST',
        body: JSON.stringify({ path: state.model, prompt, steps }),
        signal: controller.signal,
      });
      const wall = performance.now() - t0;
      state.lastTokens = res.tokens || [];

      const body = el('div');
      renderTokens(body, state.lastTokens);
      const engineMs = Number(res.elapsed_ms);
      const shown = Number.isFinite(engineMs) ? engineMs : wall;
      const meta = `${res.tokens.length} tokens · engine ${shown.toFixed(1)} ms · wall ${wall.toFixed(0)} ms · model ${state.model}`;
      const wrap = addMessage('bot', body, meta);
      botMessages.push({ element: body, tokens: state.lastTokens, wrapper: wrap });

      $('m-total').textContent = shown.toFixed(1);
      $('m-per-token').textContent = (shown / Math.max(1, res.tokens.length)).toFixed(2);
      $('m-tps').textContent = (res.tokens.length / Math.max(0.001, shown / 1000)).toFixed(1);
      drawChart(state.lastTokens);
      if (Array.isArray(res.state)) renderState(res.state);
      log(`${state.model} prompt=${JSON.stringify(prompt)} steps=${steps} → ${JSON.stringify(state.lastTokens)}`);
      setStatus('ready', 'ready');
    } catch (e) {
      if (e.name === 'AbortError') {
        /* The client stopped waiting. The engine process is left alone:
         * killing it server-side would also kill any other request. */
        addMessage('sys', textNode('Request cancelled — the engine kept running, its answer was discarded.'));
        log('request cancelled by the user');
        setStatus('ready', 'ready');
      } else {
        addMessage('sys', textNode(`Engine error: ${e.message}`));
        log(`engine error: ${e.message}`);
        setStatus('error', 'error');
      }
    } finally {
      state.inflight = null;
      setBusy(false);
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
    setBusy(true);
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
    } finally {
      setBusy(false);
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

  /* ── Tabs ─────────────────────────────────────────────────── */

  function selectTab(id) {
    for (const name of ['chat', 'engine', 'train']) {
      const tab = $(`tab-${name}`);
      const view = $(`view-${name}`);
      const active = name === id;
      tab.classList.toggle('is-active', active);
      tab.setAttribute('aria-selected', active ? 'true' : 'false');
      view.hidden = !active;
    }
  }

  /* ── Settings drawer ──────────────────────────────────────── */

  const FOCUSABLE = 'a[href], button:not([disabled]), input:not([disabled]), select:not([disabled]), textarea:not([disabled]), [tabindex]:not([tabindex="-1"])';

  function openSettings(open) {
    const drawer = $('settings');
    const scrim = $('scrim');
    drawer.classList.toggle('open', open);
    drawer.setAttribute('aria-hidden', open ? 'false' : 'true');
    scrim.hidden = !open;
    $('toggle-settings').setAttribute('aria-expanded', open ? 'true' : 'false');
    if (open) {
      /* aria-hidden alone hides nothing from a keyboard user: focus could
       * still walk into the drawer from behind. Focus moves in, and Tab is
       * trapped while it is open. */
      $('settings-close').focus();
      drawer.onkeydown = (ev) => {
        if (ev.key === 'Escape') {
          ev.preventDefault();
          openSettings(false);
          return;
        }
        if (ev.key !== 'Tab') return;
        const items = Array.from(drawer.querySelectorAll(FOCUSABLE))
          .filter((n) => n.offsetParent !== null);
        if (items.length === 0) return;
        const first = items[0];
        const last = items[items.length - 1];
        if (ev.shiftKey && document.activeElement === first) {
          ev.preventDefault();
          last.focus();
        } else if (!ev.shiftKey && document.activeElement === last) {
          ev.preventDefault();
          first.focus();
        }
      };
    } else {
      drawer.onkeydown = null;
      /* Focus has to go back where it came from, otherwise it falls to the
       * document body and the next Tab starts at the top of the page. */
      $('toggle-settings').focus();
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

    $('cancel').addEventListener('click', () => {
      if (state.inflight) state.inflight.abort();
    });

    $('model-select').addEventListener('change', (ev) => selectModel(ev.target.value));
    $('opt-humanity').addEventListener('input', updateAxiom);
    $('vocab-save').addEventListener('click', saveVocab);
    $('train-run').addEventListener('click', train);

    $('view-words').addEventListener('click', () => setView('words'));
    $('view-tokens').addEventListener('click', () => setView('tokens'));

    $('tab-chat').addEventListener('click', () => selectTab('chat'));
    $('tab-engine').addEventListener('click', () => selectTab('engine'));
    $('tab-train').addEventListener('click', () => selectTab('train'));
    $('tabs').addEventListener('keydown', (ev) => {
      if (ev.key !== 'ArrowRight' && ev.key !== 'ArrowLeft') return;
      const order = ['chat', 'engine', 'train'];
      const current = order.indexOf(document.activeElement.id.replace('tab-', ''));
      if (current < 0) return;
      ev.preventDefault();
      const next = order[(current + (ev.key === 'ArrowRight' ? 1 : order.length - 1)) % order.length];
      $(`tab-${next}`).focus();
      selectTab(next);
    });

    /* Upload */
    $('upload-open').addEventListener('click', () => {
      $('upload-hint').textContent = '';
      $('upload-input').value = '';
      $('upload-dialog').showModal();
    });
    $('upload-input').addEventListener('change', (ev) => {
      const file = ev.target.files && ev.target.files[0];
      if (!file) return;
      $('upload-name').value = file.name.replace(/\.hydra$/i, '');
      $('upload-hint').textContent = `${file.name} · ${file.size} bytes — press Upload`;
    });
    $('upload-confirm').addEventListener('click', async (ev) => {
      const file = $('upload-input').files && $('upload-input').files[0];
      if (!file) {
        $('upload-hint').textContent = 'choose a file first';
        ev.preventDefault();
        return;
      }
      const ok = await uploadModel(file);
      if (ok) $('upload-dialog').close();
      else ev.preventDefault();
    });

    const drawer = $('settings');
    const scrim = $('scrim');
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
    $('view-words').setAttribute('aria-pressed', view === 'words' ? 'true' : 'false');
    $('view-tokens').setAttribute('aria-pressed', view === 'tokens' ? 'true' : 'false');
    /* Re-render what is already on screen; before this the toggle only
     * changed the buttons and the messages kept their old representation. */
    for (const msg of botMessages) {
      msg.element.textContent = '';
      renderTokens(msg.element, msg.tokens);
    }
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();