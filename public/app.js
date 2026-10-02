/* Hydra-Stone Console — Frontend-Logik */
'use strict';

/* Alles in einer IIFE: vorher landeten sechs Funktionen und die
 * Konstante $() im globalen Scope und kollidierten mit jedem anderen
 * Skript auf der Seite (ESLint no-implicit-globals). */
(function () {

const $ = (id) => document.getElementById(id);
const state = { running: false };

/* ---------- Modell-Info ---------- */
async function loadModelInfo() {
  try {
    const r = await fetch('/api/model');
    const j = await r.json();
    $('m-path').textContent = j.path ? j.path.split('/').pop() : '–';
    $('m-dim').textContent = j.dim;
    $('m-vocab').textContent = j.vocab;
    $('m-layers').textContent = j.layers;
  } catch {
    $('m-path').textContent = 'Fehler beim Laden';
  }
}

/* ---------- Inferenz ---------- */
async function runInference() {
  if (state.running) return;
  state.running = true;
  $('run').disabled = true;
  $('run').textContent = '… läuft';

  const token = parseInt($('token').value, 10) || 0;
  const steps = Math.max(1, Math.min(256, parseInt($('steps').value, 10) || 16));

  try {
    const r = await fetch('/api/infer', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ token, steps }),
    });
    const j = await r.json();
    if (j.error) throw new Error(j.error);

    drawTokens(j.tokens, j.vocab);
    drawState(j.state);
    updateStats(j);

    const lines = [
      `[Hydra] Modell: dim=${j.dim} vocab=${j.vocab} layers=${j.layers}`,
      `[Hydra] Start-Token: ${j.start_token}, Steps: ${j.steps}`,
      `[Hydra] Tokens: ${j.tokens.join(' ')}`,
      `[Hydra] Timing: ${j.elapsed_ms.toFixed(3)} ms gesamt, ` +
        `${(j.elapsed_ms / j.steps).toFixed(4)} ms/Token`,
    ];
    $('log').textContent = lines.join('\n');
  } catch (e) {
    $('log').textContent = `[Fehler] ${e.message}\n\nHinweis: hydra-run kompiliert? → make`;
  } finally {
    state.running = false;
    $('run').disabled = false;
    $('run').textContent = '▶ Inferenz starten';
  }
}

function updateStats(j) {
  $('s-elapsed').textContent = j.elapsed_ms.toFixed(2);
  $('s-per-token').textContent = (j.elapsed_ms / j.steps).toFixed(4);
  const tps = j.elapsed_ms > 0 ? (j.steps / j.elapsed_ms) * 1000 : 0;
  $('s-throughput').textContent = tps >= 1000
    ? (tps / 1000).toFixed(1) + 'k' : tps.toFixed(0);
}

/* ---------- Visualisierung: Token-Stream ---------- */
function drawTokens(tokens, vocab) {
  const c = $('token-canvas');
  const ctx = c.getContext('2d');
  const w = c.width, h = c.height;
  ctx.clearRect(0, 0, w, h);

  /* Hintergrund-Raster */
  ctx.strokeStyle = 'rgba(52,211,153,0.06)';
  ctx.lineWidth = 1;
  for (let y = 0; y < h; y += 28) {
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
  }

  const n = tokens.length;
  const bw = w / n;

  for (let i = 0; i < n; ++i) {
    const v = tokens[i] / vocab;                 /* normiert 0..1 */
    const bh = Math.max(2, v * (h - 24));
    const x = i * bw;

    /* Balken */
    const hue = 155 + v * 40;                    /* grün -> teal */
    ctx.fillStyle = `hsla(${hue}, 70%, 55%, 0.85)`;
    ctx.fillRect(x + bw * 0.15, h - bh - 14, Math.max(1, bw * 0.7), bh);

    /* Verlaufslinie */
    if (i > 0) {
      const pv = tokens[i - 1] / vocab;
      ctx.strokeStyle = 'rgba(52,211,153,0.5)';
      ctx.beginPath();
      ctx.moveTo((i - 1) * bw + bw / 2, h - pv * (h - 24) - 14);
      ctx.lineTo(x + bw / 2, h - bh - 14);
      ctx.stroke();
    }
  }

  /* Achse */
  ctx.fillStyle = 'rgba(107,122,138,0.7)';
  ctx.font = '10px monospace';
  ctx.fillText('0', 2, h - 2);
  ctx.fillText(String(vocab), w - 34, h - 2);
}

/* ---------- Visualisierung: State-Vektor ---------- */
function drawState(vec) {
  const grid = $('state-grid');
  grid.innerHTML = '';
  for (const v of vec) {
    const cell = document.createElement('div');
    cell.className = 'state-cell';
    const norm = (v + 127) / 254;                 /* -127..127 -> 0..1 */
    const alpha = 0.08 + norm * 0.72;
    cell.style.background = `rgba(52,211,153,${alpha.toFixed(2)})`;
    cell.style.color = norm > 0.55 ? '#05130d' : 'var(--text-dim)';
    cell.textContent = v;
    grid.appendChild(cell);
  }
}

/* ---------- Axiom-Slider ---------- */
function updateAxiom() {
  const h = parseFloat($('humanity').value);
  $('h-value').textContent = h.toFixed(2);
  const verdict = $('h-verdict');
  if (h <= 0) {
    verdict.textContent = 'BLOCKIERT (U = −∞)';
    verdict.className = 'verdict block';
  } else {
    verdict.textContent = 'ERLAUBT';
    verdict.className = 'verdict allow';
  }
}

/* ---------- Wiring ---------- */
$('run').addEventListener('click', runInference);
$('humanity').addEventListener('input', updateAxiom);
document.addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && document.activeElement.tagName === 'INPUT') runInference();
});

loadModelInfo();
updateAxiom();
})();
