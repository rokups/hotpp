// Test of the ${workspaceFolder} expansion (src/expand.js). It needs node only.
'use strict';

const assert = require('assert');
const { expandWorkspaceFolder, expandAll } = require('../src/expand.js');

assert.strictEqual(expandWorkspaceFolder('${workspaceFolder}/spp-ls', '/w'), '/w/spp-ls');
assert.strictEqual(expandWorkspaceFolder('a${workspaceFolder}b${workspaceFolder}', '/w'), 'a/wb/w');
assert.strictEqual(expandWorkspaceFolder('spp-ls', '/w'), 'spp-ls');
// No folder: the text stays.
assert.strictEqual(expandWorkspaceFolder('${workspaceFolder}/x', undefined), '${workspaceFolder}/x');
// A value that is not a string stays.
assert.strictEqual(expandWorkspaceFolder(5, '/w'), 5);
assert.deepStrictEqual(
  expandAll(['--stdio', '-I', '${workspaceFolder}/include'], '/w'),
  ['--stdio', '-I', '/w/include']
);
console.log('expand.test.js: ok');
