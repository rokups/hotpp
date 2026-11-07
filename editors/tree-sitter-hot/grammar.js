// Copyright (c) 2026 the Hot++ project.
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

// A loose tree-sitter grammar of Hot++ scripts (.hot). It is for highlighting only.
// It does not check the language. It reads the text as a flat list of items and finds
// the declarations, the calls, the strings and the comments.
//
// The two lists below follow the parser. A test (LanguageServer.EditorKeywordListsMatchTheParser)
// compares them with the keywords of the parser. Keep one word on each string: the test reads them.

const KEYWORDS = [
  'if', 'else', 'while', 'do', 'for', 'foreach', 'in', 'return', 'break', 'continue',
  'switch', 'case', 'default', 'await', 'try', 'catch', 'finally', 'throw',
  'public', 'private', 'protected', 'override', 'virtual', 'abstract', 'async', 'const',
  'get', 'set', 'ref', 'out', 'var', 'void', 'new', 'delete', 'is', 'as', 'this', 'base',
  'true', 'false', 'null', 'class', 'struct', 'enum', 'interface', 'using', 'static_assert',
];

const TYPES = [
  'bool', 'sbyte', 'int8', 'byte', 'uint8', 'short', 'int16', 'ushort', 'uint16', 'int', 'int32',
  'uint', 'uint32', 'long', 'int64', 'ulong', 'uint64', 'nint', 'intptr', 'nuint', 'uintptr',
  'float', 'double', 'char', 'wchar', 'char16',
];

// The words that the declaration rules read.
const DECLARATION_WORDS = ['class', 'struct', 'enum', 'interface'];

const sep1 = (rule, separator) => seq(rule, repeat(seq(separator, rule)));

module.exports = grammar({
  name: 'hot',

  word: ($) => $.identifier,

  extras: ($) => [/\s/, $.comment],

  conflicts: ($) => [
    [$.type, $._item],
  ],

  rules: {
    source_file: ($) => repeat($._member),

    // A member: a declaration, an attribute line or any other item.
    _member: ($) => choice(
      $.attribute,
      $.function_declaration,
      $.field_declaration,
      $.parameter,
      $._item,
    ),

    _item: ($) => choice(
      $.class_declaration,
      $.enum_declaration,
      $.call,
      $.block,
      $.string,
      $.raw_string,
      $.char,
      $.number,
      $.identifier,
      $._keyword,
      $._primitive,
      $.operator,
      $.punctuation,
    ),

    _keyword: ($) => choice(...KEYWORDS.filter((w) => !DECLARATION_WORDS.includes(w))),
    _primitive: ($) => choice(...TYPES),

    // class Name : Base, Other { ... }  (also struct and interface)
    class_declaration: ($) => prec.right(seq(
      choice('class', 'struct', 'interface'),
      optional(field('name', $.identifier)),
      optional(seq(':', sep1($.type, ','))),
      optional($.class_body),
    )),
    class_body: ($) => seq('{', repeat($._member), '}'),

    // enum Name { A, B = 2 }
    enum_declaration: ($) => prec.right(seq(
      'enum',
      field('name', $.identifier),
      optional(seq(':', $.type)),
      optional($.enum_body),
    )),
    enum_body: ($) => seq('{', repeat(choice($.enumerator, $.number, $.operator, $.punctuation, $.comment)), '}'),
    enumerator: ($) => alias($.identifier, 'enumerator'),

    // [weak] and [native, handle("Actor")] at the start of a member.
    attribute: ($) => prec(5, seq(
      '[',
      sep1(seq(field('name', $.identifier), optional(seq('(', repeat(choice($.string, $.number, $.identifier, ',')), ')'))), ','),
      ']',
    )),

    // void Name(  and  int Name(  and  Type Name(
    function_declaration: ($) => prec.dynamic(3, prec(3, seq(
      field('type', choice($.type, 'void', 'var')),
      field('name', $.identifier),
      '(',
    ))),

    // Type Name = , Type Name ;  and  Type Name {  (a property)
    field_declaration: ($) => prec.dynamic(2, prec(2, seq(
      field('type', $.type),
      field('name', $.identifier),
      choice('=', ';', '{'),
    ))),

    // Type Name , and Type Name )  (a parameter)
    parameter: ($) => prec.dynamic(2, prec(2, seq(
      field('type', $.type),
      field('name', $.identifier),
      choice(',', ')'),
    ))),

    type: ($) => prec.right(seq(
      choice(seq($.identifier, repeat(seq('.', $.identifier))), $._primitive),
      optional($.type_arguments),
    )),
    type_arguments: ($) => seq('<', repeat1(choice($.type, ',', $.number, '[', ']')), '>'),

    call: ($) => prec(1, seq(field('function', $.identifier), '(')),

    block: ($) => seq('{', repeat($._item), '}'),

    string: ($) => seq(
      '"',
      repeat(choice($.string_content, $.escape_sequence, $.interpolation, $._dollar)),
      token.immediate('"'),
    ),
    string_content: () => token.immediate(prec(1, /[^"\\$\n]+/)),
    escape_sequence: () => token.immediate(/\\./),
    _dollar: () => token.immediate('$'),
    interpolation: ($) => seq(token.immediate('${'), repeat($._item), '}'),

    // R"(text)" without a delimiter.
    raw_string: () => token(seq('R"(', repeat(choice(/[^)]/, /\)[^"]/)), ')"')),

    char: () => token(/'([^'\\\n]|\\.)*'/),

    number: () => token(choice(
      /0[xX][0-9a-fA-F]+[uUlL]*/,
      /\d+(\.\d*)?([eE][+-]?\d+)?[fFuUlL]*/,
      /\.\d+([eE][+-]?\d+)?[fF]?/,
    )),

    identifier: () => /[A-Za-z_]\w*/,

    comment: () => token(prec(2, choice(
      seq('//', /.*/),
      seq('/*', /[^*]*\*+([^/*][^*]*\*+)*/, '/'),
    ))),

    // The signs < and > are separate tokens: type arguments use them too.
    operator: () => choice(/[-+*%&|^~!=?:]+/, '<', '>', '/', '/='),
    punctuation: () => choice('(', ')', '[', ']', ',', ';', '.', '@', '#', '$', '\\', '`'),
  },
});
