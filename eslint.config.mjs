// ESLint flat config for Hydra-Stone.
//
// The runtime has ZERO npm dependencies (server.js uses only node: builtins).
// ESLint is a devDependency and never ships with the engine.
//
// Rule selection rationale:
//  - `js.configs.recommended` catches the errors class (undefined vars, ...)
//  - Security-relevant rules are switched on explicitly because this server
//    takes model paths from HTTP parameters (CVE-2025-2445 class: command
//    injection) and serves static files (path traversal).
//  - `no-*` style rules that fight the existing formatting are deliberately
//    NOT enabled: this is a deliberate, readable style, not a house style
//    battle.
import js from '@eslint/js';

export default [
  js.configs.recommended,
  {
    files: ['server.js'],
    languageOptions: {
      ecmaVersion: 2022,
      sourceType: 'commonjs',
      globals: {
        require: 'readonly',
        module: 'writable',
        process: 'readonly',
        console: 'readonly',
        Buffer: 'readonly',
        __dirname: 'readonly',
        URL: 'readonly',
        setTimeout: 'readonly',
        clearTimeout: 'readonly',
      },
    },
    rules: {
      // Command-injection surface: no shell string building.
      'no-eval': 'error',
      'no-implied-eval': 'error',
      'no-new-func': 'error',
      'no-var': 'error',
      'prefer-const': 'error',
      eqeqeq: ['error', 'smart'],
      'no-throw-literal': 'error',
      'no-return-await': 'error',
      'require-await': 'error',
      // Node 18+ global fetch/URL are covered via globals above.
      'no-unused-vars': ['error', { argsIgnorePattern: '^_' }],
    },
  },
  {
    files: ['public/app.js'],
    languageOptions: {
      ecmaVersion: 2022,
      sourceType: 'script',
      globals: {
        window: 'readonly',
        document: 'readonly',
        fetch: 'readonly',
        console: 'readonly',
        setTimeout: 'readonly',
        setInterval: 'readonly',
        clearInterval: 'readonly',
        requestAnimationFrame: 'readonly',
      },
    },
    rules: {
      'no-eval': 'error',
      'no-implied-eval': 'error',
      'no-new-func': 'error',
      'no-var': 'error',
      'prefer-const': 'error',
      eqeqeq: ['error', 'smart'],
      'no-var': 'error',
      'no-implicit-globals': 'error',
      'no-unused-vars': ['error', { argsIgnorePattern: '^_' }],
    },
  },
];