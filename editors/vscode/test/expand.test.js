// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Test of the ${workspaceFolder} expansion (src/expand.js). It needs node only.
'use strict';

const assert = require('assert');
const { expandWorkspaceFolder, expandAll } = require('../src/expand.js');

assert.strictEqual(expandWorkspaceFolder('${workspaceFolder}/hot-ls', '/w'), '/w/hot-ls');
assert.strictEqual(expandWorkspaceFolder('a${workspaceFolder}b${workspaceFolder}', '/w'), 'a/wb/w');
assert.strictEqual(expandWorkspaceFolder('hot-ls', '/w'), 'hot-ls');
// No folder: the text stays.
assert.strictEqual(expandWorkspaceFolder('${workspaceFolder}/x', undefined), '${workspaceFolder}/x');
// A value that is not a string stays.
assert.strictEqual(expandWorkspaceFolder(5, '/w'), 5);
assert.deepStrictEqual(
  expandAll(['--stdio', '-I', '${workspaceFolder}/include'], '/w'),
  ['--stdio', '-I', '/w/include']
);
console.log('expand.test.js: ok');
