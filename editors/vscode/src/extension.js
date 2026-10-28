// Thin client of the S++ language server. Plain JS: no build step.
// The extension fails soft. A missing binary or a missing package gives one warning.
'use strict';

const vscode = require('vscode');

let client;
let warned = false;

function warn(message) {
  if (warned) return;
  warned = true;
  vscode.window.showWarningMessage(`S++: ${message} Highlighting still works.`);
}

function activate(context) {
  const config = vscode.workspace.getConfiguration('spp');
  if (!config.get('server.enable', true)) return;

  let lc;
  try {
    lc = require('vscode-languageclient/node');
  } catch (e) {
    warn('the package vscode-languageclient is missing. Run "npm install" in the extension folder.');
    return;
  }

  const command = config.get('server.path', 'spp-ls');
  const args = config.get('server.args', ['--stdio']);
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
