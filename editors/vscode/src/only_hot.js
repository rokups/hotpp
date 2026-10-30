// The middleware of the language client. The client also sends the ".cpp" documents to the
// server, to get the hotc errors of a ".cpp" script. The other features of a ".cpp" file belong
// to clangd. So each feature request of the client for a document that is not language "hot"
// returns no answer, and the server does not get the request. No dependency on the vscode
// module, so a plain node test can run it.
'use strict';

// The names of the middleware functions of the features of the server.
const FEATURES = [
  'provideCompletionItem',
  'provideHover',
  'provideSignatureHelp',
  'provideDefinition',
  'provideReferences',
  'provideDocumentSymbols',
  'provideDocumentFormattingEdits',
  'provideDocumentRangeFormattingEdits',
  'provideOnTypeFormattingEdits',
  'provideCodeActions',
  'provideRenameEdits',
  'prepareRename',
  'provideDocumentSemanticTokens',
  'provideDocumentSemanticTokensEdits',
  'provideDocumentRangeSemanticTokens',
  'provideDocumentHighlights',
  'provideDeclaration',
  'provideTypeDefinition',
  'provideImplementation',
  'provideFoldingRanges',
  'provideSelectionRanges',
  'provideDocumentLinks',
  'provideCodeLenses',
  'provideInlayHints',
  'provideLinkedEditingRange',
];

// Makes the middleware object. In each function the first argument is the document and the last
// argument is the function "next" that calls the server.
function onlyHot() {
  const middleware = {};
  for (const name of FEATURES) {
    middleware[name] = (...args) => {
      const next = args[args.length - 1];
      const document = args[0];
      if (document && document.languageId !== 'hot') return undefined;
      return next(...args.slice(0, -1));
    };
  }
  return middleware;
}

module.exports = { onlyHot, FEATURES };
