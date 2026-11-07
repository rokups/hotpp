" Copyright (c) 2026 the Hot++ project.
" This work is licensed under the terms of the MIT license.
" For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

" Vim syntax file for Hot++ scripts (.hot).
" The "syn keyword" lines follow the keywords of the parser. A test
" (LanguageServer.EditorKeywordListsMatchTheParser) reads these lines. Keep every
" keyword on a "syn keyword" line.

if exists('b:current_syntax')
  finish
endif

" The first items have the lowest priority: a later item wins at the same place.
" Calls: a name followed by (
syn match hotFunctionCall "\<\h\w*\ze\s*("

" Operators
syn match hotOperator "=>\|?\.\|??\|[-+*/%&|^~!<>=?:]\+"

" Comments
syn keyword hotTodo contained TODO FIXME XXX NOTE
syn match   hotLineComment "//.*$" contains=hotTodo,@Spell
syn region  hotBlockComment start="/\*" end="\*/" contains=hotTodo,@Spell

" Keywords
syn keyword hotConditional if else switch case default
syn keyword hotRepeat while do for foreach in
syn keyword hotStatement return break continue static_assert
syn keyword hotException try catch finally throw await
syn keyword hotOperatorWord new delete is as
syn keyword hotStorage public private protected override virtual abstract async const get set ref out
syn keyword hotStructure class struct enum interface
syn keyword hotInclude using
syn keyword hotBoolean true false null
syn keyword hotSpecial this base
syn keyword hotType var void
syn keyword hotType bool sbyte int8 byte uint8 short int16 ushort uint16 int int32
syn keyword hotType uint uint32 long int64 ulong uint64 nint intptr nuint uintptr
syn keyword hotType float double char wchar char16
syn keyword hotBuiltin enumerate range

" Attributes at the start of a line: [weak], [native, handle("Actor")]
syn region hotAttributeList start="^\s*\[\ze\s*\h\w*\s*[,\](]" end="\]" contains=hotString,hotNumber,hotBoolean,hotAttributeName
syn match hotAttributeName "\<\h\w*\>" contained

" Numbers
syn match hotNumber "\<0[xX]\x\+[uUlL]*\>"
syn match hotNumber "\<\d\+\(\.\d*\)\=\([eE][-+]\=\d\+\)\=[fFuUlL]*\>"
syn match hotNumber "\.\d\+\([eE][-+]\=\d\+\)\=[fF]\=\>"

" Strings and characters. A string can hold ${expression} and an escape \$.
syn match  hotEscape contained "\\."
syn region hotInterp contained matchgroup=hotInterpDelim start="\${" end="}" contains=hotInterpBraces,hotNumber,hotConditional,hotOperatorWord,hotBoolean,hotSpecial,hotType,hotFunctionCall,hotString
syn region hotInterpBraces contained start="{" end="}" contains=hotInterpBraces,hotNumber,hotString
syn region hotString start=+"+ skip=+\\\\\|\\"+ end=+"\|$+ contains=hotEscape,hotInterp,@Spell
syn region hotRawString start=+R"(+ end=+)"+
syn region hotChar start=+'+ skip=+\\\\\|\\'+ end=+'\|$+ contains=hotEscape

" Declarations: the name after class, struct, interface and enum
syn match hotTypeName "\<\(class\|struct\|interface\|enum\)\s\+\zs\h\w*"


hi def link hotTodo Todo
hi def link hotLineComment Comment
hi def link hotBlockComment Comment
hi def link hotConditional Conditional
hi def link hotRepeat Repeat
hi def link hotStatement Statement
hi def link hotException Exception
hi def link hotOperatorWord Keyword
hi def link hotStorage StorageClass
hi def link hotStructure Structure
hi def link hotInclude Include
hi def link hotBoolean Boolean
hi def link hotSpecial Special
hi def link hotType Type
hi def link hotBuiltin Function
hi def link hotAttributeName PreProc
hi def link hotNumber Number
hi def link hotEscape SpecialChar
hi def link hotInterpDelim Delimiter
hi def link hotString String
hi def link hotRawString String
hi def link hotChar Character
hi def link hotTypeName Type
hi def link hotFunctionCall Function
hi def link hotOperator Operator

let b:current_syntax = 'hot'
