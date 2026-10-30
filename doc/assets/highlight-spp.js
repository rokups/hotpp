/*
 * S++ language definition for highlight.js (version 11).
 * The word lists come from editors/vscode/syntaxes/spp.tmLanguage.json.
 * Keep the two files in sync when the language changes.
 */
(function (hljs) {
  'use strict';
  // The page stays a plain code block when the CDN script does not load.
  if (!hljs) return;

  var KEYWORDS = {
    $pattern: /[A-Za-z_]\w*/,
    keyword:
      'if else while do for foreach in return break continue switch case default ' +
      'try catch finally throw ' +
      'public private protected override virtual const get set ' +
      'class struct enum var void new delete is as using',
    type:
      'bool sbyte int8 byte uint8 short int16 ushort uint16 int int32 uint uint32 ' +
      'long int64 ulong uint64 nint intptr nuint uintptr float double char wchar char16',
    literal: 'true false null',
    'variable.language': 'this base',
    built_in: 'enumerate range'
  };

  // Words that can precede "(" but are not function names.
  var NOT_A_CALL = (
    'if|else|while|do|for|foreach|in|return|break|continue|switch|case|default|' +
    'try|catch|finally|throw|public|private|protected|override|virtual|const|get|set|' +
    'class|struct|enum|var|void|new|delete|is|as|using|this|base|true|false|null|' +
    'enumerate|range|' +
    'bool|sbyte|int8|byte|uint8|short|int16|ushort|uint16|int|int32|uint|uint32|' +
    'long|int64|ulong|uint64|nint|intptr|nuint|uintptr|float|double|char|wchar|char16'
  );

  var IDENT = '[A-Za-z_]\\w*';

  var NUMBER = {
    scope: 'number',
    relevance: 0,
    variants: [
      { begin: /\b0[xX][0-9a-fA-F]+(?:[uU]?[lL]{0,2})\b/ },
      { begin: /(?:\b\d+\.\d*|\B\.\d+|\b\d+(?=[eE]))(?:[eE][+-]?\d+)?[fF]?\b|\b\d+[fF]\b/ },
      { begin: /\b\d+(?:[uU]?[lL]{0,2})\b/ }
    ]
  };

  var FUNCTION_CALL = {
    scope: 'title.function',
    begin: new RegExp('\\b(?!(?:' + NOT_A_CALL + ')\\b)' + IDENT + '(?=\\s*\\()'),
    relevance: 0
  };

  var ESCAPE = { scope: 'char.escape', begin: /\\./, relevance: 0 };

  var INTERPOLATION = {
    scope: 'subst',
    begin: /\$\{/,
    end: /\}/,
    keywords: KEYWORDS,
    contains: [NUMBER, FUNCTION_CALL]
  };

  var STRING = {
    scope: 'string',
    begin: '"',
    end: '"|$',
    contains: [ESCAPE, INTERPOLATION]
  };

  var CHAR = {
    scope: 'string',
    begin: "'",
    end: "'|$",
    contains: [ESCAPE]
  };

  // An attribute list such as [alias("total")] at the start of a line.
  var ATTRIBUTE = {
    scope: 'meta',
    begin: /^\s*\[(?=\s*[A-Za-z_]\w*\s*[,\](])/,
    end: /\]/,
    contains: [
      hljs.C_BLOCK_COMMENT_MODE,
      STRING,
      NUMBER,
      { scope: 'meta', begin: new RegExp('\\b' + IDENT + '\\b(?=\\s*[,\\](])'), relevance: 0 },
      {
        begin: /\(/,
        end: /\)/,
        contains: [
          STRING,
          NUMBER,
          { scope: 'literal', begin: /\b(?:true|false|null)\b/ },
          { scope: 'params', begin: new RegExp('\\b' + IDENT + '\\b'), relevance: 0 }
        ]
      }
    ]
  };

  // using <header>; using "file";
  var USING = {
    begin: /\busing\b/,
    end: /;/,
    returnEnd: true,
    keywords: { $pattern: /[A-Za-z_]\w*/, keyword: 'using' },
    contains: [
      { scope: 'string', begin: /<[^>\n]*>/ },
      { scope: 'string', begin: /"[^"\n]*"/ }
    ]
  };

  // class Name : Base {
  var TYPE_DECLARATION = [
    {
      begin: [/\b(?:class|struct|enum)/, /\s+/, new RegExp(IDENT), /\s*:\s*/, /[^{;\n]+?(?=\s*\{)/],
      beginScope: { 1: 'keyword', 3: 'title.class', 5: 'title.class.inherited' }
    },
    {
      begin: [/\b(?:class|struct|enum)/, /\s+/, new RegExp(IDENT)],
      beginScope: { 1: 'keyword', 3: 'title.class' }
    }
  ];

  // is Type, as Type
  var TYPE_TEST = {
    begin: [
      /\b(?:is|as)\b/,
      /\s+/,
      /(?!(?:null|true|false)\b)[A-Za-z_][\w.]*(?:<[^<>\n]*>)?[*&]?/
    ],
    beginScope: { 1: 'keyword', 3: 'type' }
  };

  function spp() {
    return {
      name: 'S++',
      aliases: ['spp'],
      keywords: KEYWORDS,
      contains: [
        hljs.C_LINE_COMMENT_MODE,
        hljs.C_BLOCK_COMMENT_MODE,
        ATTRIBUTE,
        USING,
        TYPE_DECLARATION[0],
        TYPE_DECLARATION[1],
        TYPE_TEST,
        STRING,
        CHAR,
        NUMBER,
        FUNCTION_CALL
      ]
    };
  }

  hljs.registerLanguage('spp', spp);

  // Rouge writes each fenced block as div.language-X.highlighter-rouge > ... > pre > code.
  // Rouge does not know S++. For known languages it adds its own spans.
  // Reset the code to plain text, then let highlight.js style it.
  var blocks = document.querySelectorAll('div.highlighter-rouge pre code');
  Array.prototype.forEach.call(blocks, function (code) {
    var box = code.closest('div.highlighter-rouge');
    var match = /(?:^|\s)language-([\w+-]+)/.exec(box.className);
    var lang = match ? match[1] : '';
    if (lang && hljs.getLanguage(lang)) {
      code.textContent = code.textContent;
      code.classList.add('language-' + lang);
    } else {
      code.classList.add('nohighlight');
    }
  });
  // A fence without a tag has no div.highlighter-rouge. Keep it plain.
  Array.prototype.forEach.call(document.querySelectorAll('pre > code'), function (code) {
    if (!code.closest('div.highlighter-rouge')) code.classList.add('nohighlight');
  });

  hljs.configure({ ignoreUnescapedHTML: true });
  hljs.highlightAll();
})(window.hljs);
