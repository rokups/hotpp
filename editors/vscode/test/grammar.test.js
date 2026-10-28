// Test of the TextMate grammar of S++. Run: node test/grammar.test.js [--update]
//
// Always run (plain node, no package):
//  - package.json, language-configuration.json and the grammar are valid JSON.
//  - Every keyword and every builtin type name of the language is in the grammar.
// Run when vscode-textmate and vscode-oniguruma are installed (npm install in this folder):
//  - Every file of example/scripts/*.spp tokenizes. No token has an "invalid" scope.
//    No rule stays open at the end of a file.
//  - test/sample.spp matches the snapshot test/sample.snap.
// Without the two packages the second part is skipped and a notice says so.
'use strict';

const fs = require('fs');
const path = require('path');

const root = path.resolve(__dirname, '..');
const repo = path.resolve(root, '..', '..');
const update = process.argv.includes('--update');
let failures = 0;
const fail = (msg) => { failures++; console.error('FAIL: ' + msg); };

function readJson(file) {
  try {
    return JSON.parse(fs.readFileSync(file, 'utf8'));
  } catch (e) {
    fail(`${path.relative(repo, file)}: ${e.message}`);
    return null;
  }
}

// ---- part 1: JSON and keyword coverage
const pkg = readJson(path.join(root, 'package.json'));
readJson(path.join(root, 'language-configuration.json'));
const grammarFile = path.join(root, 'syntaxes', 'spp.tmLanguage.json');
const grammar = readJson(grammarFile);
if (!grammar) process.exit(1);
if (pkg) {
  const c = pkg.contributes || {};
  if (!(c.languages || []).some((l) => l.id === 'spp' && l.extensions.includes('.spp'))) fail('language spp');
  if (!(c.grammars || []).some((g) => g.scopeName === grammar.scopeName)) fail('grammar scope');
  if (!c.configuration || !c.configuration.properties['spp.server.path']) fail('setting spp.server.path');
}

const keywords = ['var', 'let', 'foreach', 'in', 'using', 'override', 'virtual', 'new', 'public', 'private',
  'protected', 'get', 'set', 'finally', 'ref', 'out', 'void', 'is', 'as', 'try', 'catch', 'throw', 'class',
  'struct', 'enum', 'const', 'if', 'else', 'while', 'do', 'for', 'return', 'break', 'continue', 'switch',
  'case', 'default', 'this', 'super', 'null', 'true', 'false', 'delete'];

// The builtin names come from the translator header: {"script", "alias" or nullptr, "cpp"}.
const header = fs.readFileSync(path.join(repo, 'src', 'translator', 'builtin_types.h'), 'utf8');
const types = [];
for (const m of header.matchAll(/\{"(\w+)",\s*(?:"(\w+)"|nullptr),\s*"\w+"\}/g)) {
  types.push(m[1]);
  if (m[2]) types.push(m[2]);
}
if (types.length < 15) fail(`only ${types.length} builtin types found in builtin_types.h`);

// A word is covered when it is an alternative of a "match" regex of the grammar.
const matches = [];
(function walk(node) {
  if (Array.isArray(node)) return node.forEach(walk);
  if (node && typeof node === 'object') {
    if (typeof node.match === 'string') matches.push(node.match);
    Object.values(node).forEach(walk);
  }
})(grammar);
const words = new Set(matches.flatMap((r) => r.replace(/\\b|\(\?[:=!]|[()]/g, '|').split('|')));
const covered = (w) => words.has(w);
for (const w of keywords) if (!covered(w)) fail(`keyword not in the grammar: ${w}`);
for (const w of types) if (!covered(w)) fail(`builtin type not in the grammar: ${w}`);

// ---- part 2: tokenizer
let textmate, oniguruma;
try {
  textmate = require('vscode-textmate');
  oniguruma = require('vscode-oniguruma');
} catch (e) {
  console.log('NOTICE: vscode-textmate or vscode-oniguruma is missing. The tokenizer test is skipped.');
  console.log('NOTICE: run "npm install" in editors/vscode to turn it on.');
  finish();
}

async function main() {
  const wasm = fs.readFileSync(require.resolve('vscode-oniguruma/release/onig.wasm')).buffer;
  await oniguruma.loadWASM(wasm);
  const registry = new textmate.Registry({
    onigLib: Promise.resolve({
      createOnigScanner: (p) => new oniguruma.OnigScanner(p),
      createOnigString: (s) => new oniguruma.OnigString(s),
    }),
    loadGrammar: async (scope) => (scope === grammar.scopeName ? textmate.parseRawGrammar(
      fs.readFileSync(grammarFile, 'utf8'), grammarFile) : null),
  });
  const g = await registry.loadGrammar(grammar.scopeName);
  if (!g) { fail('the grammar does not load'); return; }

  // Gives the lines "text" => scopes, and checks the end state.
  function tokenize(text, name, withScopes) {
    let state = textmate.INITIAL;
    const out = [];
    text.split('\n').forEach((line, i) => {
      const r = g.tokenizeLine(line, state);
      for (const t of r.tokens) {
        const scopes = t.scopes.slice(1);
        if (scopes.some((s) => s.startsWith('invalid'))) fail(`${name}:${i + 1}: invalid scope`);
        if (withScopes && line.slice(t.startIndex, t.endIndex).trim() !== '')
          out.push(`${i + 1}: ${JSON.stringify(line.slice(t.startIndex, t.endIndex))} ${scopes.join(' ')}`);
      }
      state = r.ruleStack;
    });
    // Only a block comment may reach the end of a file. A file must not end inside one.
    if (state.depth > 1) fail(`${name}: a rule is still open at the end of the file`);
    return out;
  }

  const dir = path.join(repo, 'example', 'scripts');
  const files = fs.readdirSync(dir).filter((f) => f.endsWith('.spp')).sort();
  if (files.length === 0) fail('no example scripts found');
  for (const f of files) tokenize(fs.readFileSync(path.join(dir, f), 'utf8'), f, false);
  console.log(`tokenized ${files.length} example scripts`);

  const sample = fs.readFileSync(path.join(root, 'test', 'sample.spp'), 'utf8');
  const got = tokenize(sample, 'sample.spp', true).join('\n') + '\n';
  const snap = path.join(root, 'test', 'sample.snap');
  if (update || !fs.existsSync(snap)) {
    fs.writeFileSync(snap, got);
    console.log('snapshot written: test/sample.snap');
  } else if (fs.readFileSync(snap, 'utf8') !== got) {
    fail('test/sample.spp differs from test/sample.snap. Run with --update to accept.');
  }
}

function finish() {
  if (failures) {
    console.error(`${failures} failure(s)`);
    process.exit(1);
  }
  console.log('grammar test passed');
  process.exit(0);
}

main().then(finish, (e) => { console.error(e); process.exit(1); });
