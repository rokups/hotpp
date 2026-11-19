// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Test of the TextMate grammar of Hot++. Run: node test/grammar.test.js [--update]
//
// Always run (plain node, no package):
//  - package.json, language-configuration.json and the grammar are valid JSON.
//  - Every keyword and every builtin type name of the language is in the grammar.
// Run when vscode-textmate and vscode-oniguruma are installed (npm install in this folder):
//  - Every file of tests/scripts/*.hot tokenizes. No token has an "invalid" scope.
//    No rule stays open at the end of a file.
//  - test/sample.hot matches the snapshot test/sample.snap.
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
const grammarFile = path.join(root, 'syntaxes', 'hot.tmLanguage.json');
const grammar = readJson(grammarFile);
if (!grammar) process.exit(1);
if (pkg) {
  const c = pkg.contributes || {};
  if (!(c.languages || []).some((l) => l.id === 'hot' && l.extensions.includes('.hot'))) fail('language hot');
  if (!(c.grammars || []).some((g) => g.scopeName === grammar.scopeName)) fail('grammar scope');
  if (!(c.languages || []).some((l) => l.id === 'rml' && l.extensions.includes('.rml'))) fail('language rml');
  const rmlReg = (c.grammars || []).find((g) => g.scopeName === 'text.html.rml');
  if (!rmlReg || rmlReg.embeddedLanguages['source.hot'] !== 'hot') fail('grammar rml');
  if (!(pkg.activationEvents || []).includes('onLanguage:rml')) fail('activation event rml');
  if (!c.configuration || !c.configuration.properties['hot.server.path']) fail('setting hot.server.path');
}

// The keywords come from the language server list in src/ls/semantic.cpp. A C++ test checks that
// this list equals the keywords of the parser. The attribute names (for example "weak") are
// keywords of the server too, but the grammar colors only "weak" of them.
const semantic = fs.readFileSync(path.join(repo, 'src', 'ls', 'semantic.cpp'), 'utf8');
const block = /std::set<std::string> r = \{([^}]*)\};/.exec(semantic);
if (!block) fail('the keyword list is not found in src/ls/semantic.cpp');
const keywords = [...(block ? block[1].matchAll(/"(\w+)"/g) : [])].map((m) => m[1]);
if (keywords.length < 40) fail(`only ${keywords.length} keywords found in src/ls/semantic.cpp`);
keywords.push('weak');

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
// The words of the removed syntax must not be keywords of the grammar.
for (const w of ['function', 'let', 'import', 'super'])
  if (covered(w)) fail(`removed word is in the grammar: ${w}`);

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

const grammarFiles = {
  [grammar.scopeName]: grammarFile,
  'text.html.rml': path.join(root, 'syntaxes', 'rml.tmLanguage.json'),
  // A small stand-in for the HTML grammar of VS Code. It has its own script rule.
  'text.html.basic': path.join(root, 'test', 'html_stub.tmLanguage.json'),
};

async function main() {
  const wasm = fs.readFileSync(require.resolve('vscode-oniguruma/release/onig.wasm')).buffer;
  await oniguruma.loadWASM(wasm);
  const registry = new textmate.Registry({
    onigLib: Promise.resolve({
      createOnigScanner: (p) => new oniguruma.OnigScanner(p),
      createOnigString: (s) => new oniguruma.OnigString(s),
    }),
    loadGrammar: async (scope) => {
      const file = grammarFiles[scope];
      return file ? textmate.parseRawGrammar(fs.readFileSync(file, 'utf8'), file) : null;
    },
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

  const dir = path.join(repo, 'tests', 'scripts');
  const files = fs.readdirSync(dir).filter((f) => f.endsWith('.hot')).sort();
  if (files.length === 0) fail('no example scripts found');
  for (const f of files) tokenize(fs.readFileSync(path.join(dir, f), 'utf8'), f, false);
  console.log(`tokenized ${files.length} example scripts`);

  const sample = fs.readFileSync(path.join(root, 'test', 'sample.hot'), 'utf8');
  const got = tokenize(sample, 'sample.hot', true).join('\n') + '\n';
  const snap = path.join(root, 'test', 'sample.snap');
  if (update || !fs.existsSync(snap)) {
    fs.writeFileSync(snap, got);
    console.log('snapshot written: test/sample.snap');
  } else if (fs.readFileSync(snap, 'utf8') !== got) {
    fail('test/sample.hot differs from test/sample.snap. Run with --update to accept.');
  }

  // ---- the rml grammar
  const rml = await registry.loadGrammar('text.html.rml');
  if (!rml) { fail('the rml grammar does not load'); return; }
  function rmlLines(text) {
    let state = textmate.INITIAL;
    return text.split('\n').map((line) => {
      const r = rml.tokenizeLine(line, state);
      state = r.ruleStack;
      return r.tokens.map((t) => ({ text: line.slice(t.startIndex, t.endIndex), scopes: t.scopes }));
    });
  }
  const find = (tokens, text) => tokens.find((t) => t.text === text);
  const has = (t, prefix) => !!t && t.scopes.some((s) => s.startsWith(prefix));

  // The body of a script block is Hot++. It wins over the script rule of the HTML grammar.
  let l = rmlLines('<div id="a">\n<script type="x">\nint x = 1;\n</script>\n<p>t</p>');
  if (!has(find(l[0], 'div'), 'entity.name.tag.html')) fail('rml: markup outside has no html scope');
  if (!has(find(l[2], 'int'), 'source.hot')) fail('rml: script body has no source.hot scope');
  if (has(find(l[2], 'int'), 'source.js')) fail('rml: script body has the scope of the HTML script rule');
  if (!has(find(l[2], 'int'), 'keyword') && !has(find(l[2], 'int'), 'storage'))
    fail('rml: "int" in a script body has no keyword or type scope');
  if (!has(find(l[1], 'script'), 'entity.name.tag.html')) fail('rml: script tag name');
  if (!has(find(l[3], 'script'), 'entity.name.tag.html')) fail('rml: script end tag name');
  if (has(find(l[4], 'p'), 'source.hot')) fail('rml: markup after a script block has a hot scope');
  if (!has(find(l[4], '>t'), 'text.html.rml')) fail('rml: top scope');

  // An uppercase tag works too.
  l = rmlLines('<SCRIPT>\nint x = 1;\n</SCRIPT>');
  if (!has(find(l[1], 'int'), 'source.hot')) fail('rml: uppercase SCRIPT body has no source.hot scope');

  // A script tag with "src" has no body. The text after it is markup.
  l = rmlLines('<script src="x.hot"></script>\n<b>int</b>');
  if (l[0].some((t) => has(t, 'source.hot'))) fail('rml: script with src has a hot scope');
  if (has(find(l[1], 'int'), 'source.hot')) fail('rml: the text after a script with src has a hot scope');
  if (!has(find(l[1], 'b'), 'entity.name.tag.html')) fail('rml: the tag after a script with src has no html scope');

  // The end tag ends the block inside a string and inside a block comment (as the mask does).
  l = rmlLines('<script>\nstring s = "</script>";\n</script>\n<b>x</b>');
  if (!has(find(l[1], '</'), 'punctuation.definition.tag') && !l[1].some((t) => has(t, 'entity.name.tag.html')))
    fail('rml: the end tag in a string is not a tag');
  if (!has(find(l[3], 'b'), 'entity.name.tag.html') || l[3].some((t) => has(t, 'source.hot')))
    fail('rml: the block does not end at the end tag in a string');
  l = rmlLines('<script>\n/* a </script>\n<i>t</i>');
  if (!has(find(l[2], 'i'), 'entity.name.tag.html') || l[2].some((t) => has(t, 'comment')))
    fail('rml: the block does not end at the end tag in a comment');
  l = rmlLines("<script>\nint x; // </script>\n<i>t</i>");
  if (!has(find(l[2], 'i'), 'entity.name.tag.html') || l[2].some((t) => has(t, 'source.hot')))
    fail('rml: the block does not end at the end tag in a line comment');

  // A self-closing script tag has no body.
  l = rmlLines('<script src="x.hot" />\n<b>int</b>');
  if (l.some((line) => line.some((t) => has(t, 'source.hot')))) fail('rml: a self-closing script has a hot scope');
  if (!has(find(l[1], 'b'), 'entity.name.tag.html')) fail('rml: the tag after a self-closing script has no html scope');

  // A script tag in an HTML comment is not code.
  l = rmlLines('<!-- <script>\nint x;\n</script> -->\n<i>t</i>');
  if (l.slice(0, 3).some((line) => line.some((t) => has(t, 'source.hot')))) fail('rml: a comment has a hot scope');
  if (!has(find(l[1], 'int x;'), 'comment.block.html')) fail('rml: the comment scope is lost');
  if (!has(find(l[3], 'i'), 'entity.name.tag.html')) fail('rml: the markup after a comment has no html scope');
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
