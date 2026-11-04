// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Test of the tree-sitter grammar. Run: node test/check.js <work dir> [tree-sitter path]
// It generates the parser in the work dir, parses test/sample.hot and every file of
// tests/scripts/*.hot, and compiles queries/highlights.scm. A parse with an ERROR or MISSING
// node fails. The tool tree-sitter must be in PATH. Without it, the test skips with a message.
'use strict';

const fs = require('fs');
const os = require('os');
const path = require('path');
const { spawnSync } = require('child_process');

const root = path.resolve(__dirname, '..');
const repo = path.resolve(root, '..', '..');
const work = path.resolve(process.argv[2] || fs.mkdtempSync(path.join(os.tmpdir(), 'ts-hot-')));

const SKIP = 77;
const tool = process.argv[3] || 'tree-sitter';
// On Windows the tool can be a .cmd file. A .cmd file needs a shell, and the shell needs quotes.
const shell = process.platform === 'win32';
const quote = (a) => (shell && /[\s&()^%!]/.test(a) ? `"${a}"` : a);
const spawn = (args, opts) => spawnSync(quote(tool), args.map(quote), { ...opts, shell, encoding: 'utf8' });

const probe = spawn(['--version'], {});
if (probe.error || probe.status !== 0) {
  console.log('NOTICE: tree-sitter is not found. The parser test is skipped.');
  process.exit(SKIP);
}

fs.rmSync(work, { recursive: true, force: true });
fs.mkdirSync(path.join(work, 'queries'), { recursive: true });
for (const f of ['grammar.js', 'tree-sitter.json', 'package.json']) fs.copyFileSync(path.join(root, f), path.join(work, f));
fs.copyFileSync(path.join(root, 'queries', 'highlights.scm'), path.join(work, 'queries', 'highlights.scm'));

// The cache of the tool stays in the work dir.
const env = { ...process.env, XDG_CACHE_HOME: path.join(work, 'cache'), HOME: process.env.HOME };
const run = (args) => spawn(args, { cwd: work, env });

let failures = 0;
const fail = (msg) => { failures++; console.error('FAIL: ' + msg); };

const gen = run(['generate']);
if (gen.status !== 0) {
  fail('tree-sitter generate:\n' + gen.stderr);
  process.exit(1);
}

const files = [path.join(root, 'test', 'sample.hot')];
const scripts = path.join(repo, 'tests', 'scripts');
for (const f of fs.readdirSync(scripts).sort()) if (f.endsWith('.hot')) files.push(path.join(scripts, f));
for (const f of files) {
  const r = run(['parse', f]);
  const out = (r.stdout || '') + (r.stderr || '');
  if (r.status !== 0 || /\(ERROR|\(MISSING|MISSING "/.test(out)) fail(`parse error in ${path.relative(repo, f)}`);
}
console.log(`parsed ${files.length} files`);

const q = run(['query', path.join(work, 'queries', 'highlights.scm'), files[0]]);
if (q.status !== 0) fail('highlights.scm does not compile:\n' + q.stderr);
else if (!/capture: \d+ - keyword,/.test(q.stdout)) fail('highlights.scm gives no keyword capture');

if (failures) process.exit(1);
console.log('tree-sitter grammar: ok');
