// Test of the middleware that keeps the features of the server on the "hot" documents
// (src/only_spp.js). It needs node only.
'use strict';

const assert = require('assert');
const { onlySpp, FEATURES } = require('../src/only_spp.js');

const m = onlySpp();
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
console.log('only_spp.test.js: ok');
