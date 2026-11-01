// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// Expands ${workspaceFolder} in the settings of the server. No dependency on the vscode
// module, so a plain node test can run it.
'use strict';

// Replaces each ${workspaceFolder} in "text" with "folder". When there is no folder, the text
// stays as it is.
function expandWorkspaceFolder(text, folder) {
  if (typeof text !== 'string' || !folder) return text;
  return text.split('${workspaceFolder}').join(folder);
}

function expandAll(list, folder) {
  return list.map((item) => expandWorkspaceFolder(item, folder));
}

module.exports = { expandWorkspaceFolder, expandAll };
