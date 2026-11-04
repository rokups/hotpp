" Copyright (c) 2026 the Hot++ project.
" This work is licensed under the terms of the MIT license.
" For a copy, see <https://opensource.org/licenses/MIT> or the accompanying LICENSE file.

if exists('b:did_ftplugin')
  finish
endif
let b:did_ftplugin = 1

setlocal commentstring=//\ %s
setlocal comments=s1:/*,mb:*,ex:*/,://
setlocal formatoptions-=t formatoptions+=croql
setlocal matchpairs+=<:>
let b:undo_ftplugin = 'setlocal commentstring< comments< formatoptions< matchpairs<'
