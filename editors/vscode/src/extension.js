// Thin client of the S++ language server. Plain JS: no build step.
// The extension fails soft. A missing binary or a missing package gives one warning.
'use strict';

const vscode = require('vscode');
const { expandWorkspaceFolder, expandAll } = require('./expand.js');
const { onlySpp } = require('./only_spp.js');

let client;
let watchers = [];  // the file watchers of the client
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

// Start the server with the current settings. It returns false if the server is off or the
// client package is missing.
function startServer() {
  const config = vscode.workspace.getConfiguration('spp');
  if (!config.get('server.enable', true)) return false;

  let lc;
  try {
    lc = require('vscode-languageclient/node');
  } catch (e) {
    warn('the package vscode-languageclient is missing. Run "npm install" in the extension folder.');
    return false;
  }

  const folders = vscode.workspace.workspaceFolders;
  const folder = folders && folders.length ? folders[0].uri.fsPath : undefined;
  const command = expandWorkspaceFolder(config.get('server.path', 'spp-ls'), folder);
  const args = expandAll(config.get('server.args', ['--stdio']), folder);
  watchers = [
    vscode.workspace.createFileSystemWatcher('**/*.hot'),
    vscode.workspace.createFileSystemWatcher('**/.cache/spp/hosts/*'),
  ];
  const options = {
    // The server gets the ".cpp" documents too. It publishes the sppc errors for a ".cpp" script,
    // which a host lists, and it ignores any other ".cpp" file. The middleware keeps all the
    // other features (completion, hover, tokens, formatting) on the "spp" documents: clangd owns
    // them for a ".cpp" file.
    documentSelector: [
      { scheme: 'file', language: 'spp' },
      { scheme: 'file', language: 'cpp' },
    ],
    middleware: onlySpp(),
    // The client sends workspace/didChangeWatchedFiles for these files. The code disposes the
    // watchers in deactivate(), so a restart does not leave old watchers.
    synchronize: { fileEvents: watchers },
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
    return false;
  }
  return true;
}

// The command "spp.restartServer". It stops the server and starts it again. It reads the
// settings again, so a new "spp.server.path" or "spp.server.args" takes effect.
async function restartServer() {
  warned = false;
  await deactivate();
  if (startServer()) vscode.window.showInformationMessage('S++: the language server restarted.');
  else vscode.window.showWarningMessage('S++: the language server is off or does not start.');
}

function activate(context) {
  context.subscriptions.push(vscode.commands.registerCommand('spp.showScriptPosition', showScriptPosition));
  context.subscriptions.push(vscode.commands.registerCommand('spp.restartServer', restartServer));
  startServer();
  context.subscriptions.push({ dispose: () => deactivate() });
}

function deactivate() {
  const c = client;
  client = undefined;
  for (const w of watchers) w.dispose();
  watchers = [];
  return c ? c.stop().catch(() => undefined) : undefined;
}

module.exports = { activate, deactivate };
