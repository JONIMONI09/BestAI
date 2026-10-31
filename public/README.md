# `public/` — the Hydra-Stone Console (plain HTML/CSS/JS)

The web UI served by `server.js` (zero npm runtime dependencies; ESLint is
dev-only, run with `npm ci && npm run lint`).

| File | What it is |
|---|---|
| `index.html` | The single page: Chat / Models / settings, the token log, import + cancel controls |
| `app.js` | All client logic — fetches against `/api/*` (models, import, infer, cancel, config, errors). It contains **no model math**: inference runs in the compiled C engine as a child of `server.js`, so the UI cannot drift from the engine |
| `style.css` | Styling; WCAG AA contrast pairs are checked by `tools/contrast_check.js` |

Gates that touch this folder: ESLint, `tools/contrast_check.js`,
`tools/android_header_check.sh` (the console and the Android app must apply
the same header rules and quote the same reason), and the live-HTTP API
suite `tools/server_test.js` (57 tests).
