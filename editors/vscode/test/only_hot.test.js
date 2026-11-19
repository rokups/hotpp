// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Test of the middleware that keeps the features of the server on the "hot" documents
// (src/only_hot.js). It needs node only.
'use strict';

const assert = require('assert');
const { onlyHot, FEATURES, DOCUMENT_SELECTOR } = require('../src/only_hot.js');

const m = onlyHot();
for (const name of FEATURES) assert.strictEqual(typeof m[name], 'function', name);

// A "cpp" document gets no answer and the server is not called.
let calls = 0;
const next = (...args) => {
  calls += 1;
  return args;
};
assert.strictEqual(m.provideCompletionItem({ languageId: 'cpp' }, 'pos', 'ctx', 'tok', next), undefined);
assert.strictEqual(m.provideDocumentSemanticTokens({ languageId: 'cpp' }, 'tok', next), undefined);
assert.strictEqual(m.provideDocumentFormattingEdits({ languageId: 'cpp' }, 'opt', 'tok', next), undefined);
assert.strictEqual(calls, 0);

// A "hot" document goes to the server with the same arguments, without "next".
assert.deepStrictEqual(m.provideHover({ languageId: 'hot' }, 'pos', 'tok', next), [{ languageId: 'hot' }, 'pos', 'tok']);
assert.strictEqual(calls, 1);

// F12: a "hot" document reaches the server (a native symbol is found there). A "cpp" document
// does not: the clangd extension answers it.
assert.strictEqual(m.provideDefinition({ languageId: 'cpp' }, 'pos', 'tok', next), undefined);
assert.strictEqual(calls, 1);
assert.deepStrictEqual(m.provideDefinition({ languageId: 'hot' }, 'pos', 'tok', next), [{ languageId: 'hot' }, 'pos', 'tok']);
assert.strictEqual(calls, 2);

// The selector sends only "hot" documents and ".cpp" files to the server. A header is not sent.
assert.deepStrictEqual(DOCUMENT_SELECTOR, [
  { scheme: 'file', language: 'hot' },
  { scheme: 'file', pattern: '**/*.rml' },
  { scheme: 'file', language: 'cpp', pattern: '**/*.cpp' },
]);

// An "rml" document goes to the server like a "hot" document.
assert.deepStrictEqual(m.provideHover({ languageId: 'rml' }, 'pos', 'tok', next), [{ languageId: 'rml' }, 'pos', 'tok']);
assert.strictEqual(calls, 3);
// The other languages get no answer: for example "html" and "plaintext".
assert.strictEqual(m.provideHover({ languageId: 'html' }, 'pos', 'tok', next), undefined);
assert.strictEqual(m.provideHover({ languageId: 'plaintext' }, 'pos', 'tok', next), undefined);
assert.strictEqual(calls, 3);

const cppEntry = DOCUMENT_SELECTOR.find((s) => s.language === 'cpp');
assert.ok(cppEntry.pattern.endsWith('*.cpp'));
assert.ok(!DOCUMENT_SELECTOR.some((s) => s.language === 'cpp' && !s.pattern));
console.log('only_hot.test.js: ok');
