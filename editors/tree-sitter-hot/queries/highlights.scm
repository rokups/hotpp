; Highlights of Hot++ scripts. The word lists below follow the parser keywords.
; The test LanguageServer.EditorKeywordListsMatchTheParser reads the quoted words.

(comment) @comment

[
  (string_content)
  (string)
  (char)
  (raw_string)
] @string

(escape_sequence) @string.escape

(interpolation
  "${" @punctuation.special
  "}" @punctuation.special)

(number) @number

; Keywords
[
  "if" "else" "switch" "case" "default"
  "while" "do" "for" "foreach" "in"
  "return" "break" "continue"
  "try" "catch" "finally" "throw" "await"
  "new" "delete" "is" "as"
] @keyword

[
  "public" "private" "protected" "override" "virtual" "abstract" "async" "const"
  "get" "set" "ref" "out" "var" "void"
] @keyword.modifier

[
  "class" "struct" "enum" "interface"
] @keyword.type

"using" @keyword.import

["true" "false" "null"] @constant.builtin

["this" "base"] @variable.builtin

; Builtin types
[
  "bool" "sbyte" "int8" "byte" "uint8" "short" "int16" "ushort" "uint16" "int" "int32"
  "uint" "uint32" "long" "int64" "ulong" "uint64" "nint" "intptr" "nuint" "uintptr"
  "float" "double" "char" "wchar" "char16"
] @type.builtin

; Declarations
(class_declaration name: (identifier) @type)
(enum_declaration name: (identifier) @type)
(enumerator) @constant
(function_declaration name: (identifier) @function)
(field_declaration name: (identifier) @property)
(type (identifier) @type)
(attribute name: (identifier) @attribute)
(parameter name: (identifier) @variable.parameter)

; Calls. The names enumerate and range are helpers of the language.
(call function: (identifier) @function.call)
((call function: (identifier) @function.builtin)
  (#any-of? @function.builtin "enumerate" "range"))

(operator) @operator
(punctuation) @punctuation.delimiter
