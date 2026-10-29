// Thin client of the S++ language server. Plain JS: no build step.
// The extension fails soft. A missing binary or a missing package gives one warning.
'use strict';

const vscode = require('vscode');
const { expandWorkspaceFolder, expandAll } = require('./expand.js');

let client;
let warned = false;

function warn(message) {
  if (warned) return;
  warned = true;
  vscode.window.showWarningMessage(`S++: ${message} Highlighting still works.`);
}

// The command "spp.showScriptPosition". It asks the server for the script place of the cursor
// in a generated .cpp file, and shows that place.
async function showScriptPosition() {
  const editor = vscode.window.activeTextEditor;
  if (!editor) return;
  if (!client) {
    vscode.window.showWarningMessage('S++: the language server is not running.');
    return;
  }
  let location;
  try {
    location = await client.sendRequest('spp/toScript', {
      textDocument: { uri: editor.document.uri.toString() },
      position: { line: editor.selection.active.line, character: editor.selection.active.character },
    });
  } catch (e) {
    vscode.window.showWarningMessage(`S++: the request spp/toScript fails: ${e.message}`);
    return;
  }
  if (!location || !location.uri) {
    vscode.window.showInformationMessage('S++: this place has no script position.');
    return;
  }
  const doc = await vscode.workspace.openTextDocument(vscode.Uri.parse(location.uri));
  const start = new vscode.Position(location.range.start.line, location.range.start.character);
  const end = new vscode.Position(location.range.end.line, location.range.end.character);
  await vscode.window.showTextDocument(doc, { selection: new vscode.Range(start, end) });
}

function activate(context) {
  context.subscriptions.push(vscode.commands.registerCommand('spp.showScriptPosition', showScriptPosition));
  const config = vscode.workspace.getConfiguration('spp');
  if (!config.get('server.enable', true)) return;

  let lc;
  try {
    lc = require('vscode-languageclient/node');
  } catch (e) {
    warn('the package vscode-languageclient is missing. Run "npm install" in the extension folder.');
    return;
  }

  const folders = vscode.workspace.workspaceFolders;
  const folder = folders && folders.length ? folders[0].uri.fsPath : undefined;
  const command = expandWorkspaceFolder(config.get('server.path', 'spp-ls'), folder);
  const args = expandAll(config.get('server.args', ['--stdio']), folder);
  const options = {
    documentSelector: [{ scheme: 'file', language: 'spp' }],
    // Do not restart a server that does not start.
    errorHandler: {
      error: () => {
        warn(`the server "${command}" fails. Set "spp.server.path".`);
        return { action: lc.ErrorAction.Shutdown };
      },
      closed: () => {
        warn(`the server "${command}" stopped. Set "spp.server.path".`);
        return { action: lc.CloseAction.DoNotRestart };
      },
    },
  };

  try {
    client = new lc.LanguageClient('spp', 'S++ Language Server', { command, args }, options);
    client.start().catch(() => warn(`the server "${command}" does not start. Set "spp.server.path".`));
  } catch (e) {
    warn(`the server "${command}" does not start. Set "spp.server.path".`);
  }
  context.subscriptions.push({ dispose: () => deactivate() });
}

function deactivate() {
  const c = client;
  client = undefined;
  return c ? c.stop().catch(() => undefined) : undefined;
}

module.exports = { activate, deactivate };
