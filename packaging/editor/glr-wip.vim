" glr-wip.vim - publish vim's unsaved buffer to gl-repl's `--watch` sidecar.
"
" `./gl-repl --watch scene.glr` follows the file on every `:w`. Drop this in
" `~/.vim/plugin/` (or `:source` it) and it follows every *keystroke* instead:
" the buffer is written to `scene.glr.wip` on each change and each cursor move,
" and gl-repl mirrors it live - geometry, caret position, and the half-typed
" row parked in its own input line where the edit guides can draw it.
"
" Nothing here writes the scene file. `:w` still does that, and it is still the
" only thing gl-repl treats as saved. The sidecar is deleted when the buffer
" unloads or vim exits, so finding one at startup means vim died holding
" unsaved work - gl-repl offers it back rather than applying it.
"
" Two details that are not free choices:
"
"   The write must be atomic. Writing `scene.glr.wip` in place lets gl-repl
"   observe it half-written between the truncate and the last byte. So each
"   publication goes to a sibling temp in the same directory and is renamed
"   over the target - a same-directory rename(2), which replaces atomically.
"
"   The cursor line is last, and it is the only line gl-repl excludes from the
"   content hash. That is what makes moving around in vim cost gl-repl a stat
"   and a hash instead of a full document reimport, so the exclusion has to
"   stay true: nothing may follow `// @cursor <line> <col>`.
"
" Configuration, both optional:
"
"   let g:glr_wip_patterns = ['*.glr', '*.c']   " which files to publish
"   let g:loaded_glr_wip = 1                    " before sourcing: disable
"
" The three functions are global rather than script-local so `:call
" GlrWipPublish()` works from a mapping or by hand. That is also the only
" spelling that survives the autocmds below: they are built with `:execute`, and
" `<SID>` inside an `:execute` string is stored literally instead of being
" resolved to the script number, so a script-local callback registers fine and
" then fails to exist when the event fires.

if exists('g:loaded_glr_wip')
  finish
endif
let g:loaded_glr_wip = 1

if !exists('g:glr_wip_patterns')
  " Both authored formats. `.glr` is the usual one; exported `.c` is what
  " Ctrl+S writes when the scene has no .glr home, and --watch scene.c
  " follows that file. Without `*.c` here the sidecar is never published
  " and live cursor follow is silently just `:w`.
  let g:glr_wip_patterns = ['*.glr', '*.c']
endif

" The sidecar for a file, or '' for a buffer with no name on disk (a scratch
" buffer has nothing gl-repl could be bound to).
"
" gl-repl binds through realpath(); :p is only absolute. Opening a scene
" through a symlink would otherwise publish <link>.wip while gl-repl watches
" <resolved-target>.wip and live follow would never see it.
function! GlrWipSidecar(name) abort
  let l:path = fnamemodify(a:name, ':p')
  if empty(l:path) || isdirectory(l:path)
    return ''
  endif
  let l:resolved = resolve(l:path)
  if empty(l:resolved)
    let l:resolved = l:path
  endif
  return l:resolved . '.wip'
endfunction

function! GlrWipPublish() abort
  let l:target = GlrWipSidecar(expand('%'))
  if empty(l:target)
    return
  endif

  let l:lines = getline(1, '$')
  " line('.') is 1-based; col('.') is a 1-based *byte* offset, which is what
  " gl-repl converts from. Appended last, and with nothing after it.
  call add(l:lines, printf('// @cursor %d %d', line('.'), col('.')))

  " Same directory as the target, so the rename below stays within one
  " filesystem and is therefore atomic. The pid keeps two vims editing the
  " same file from colliding on the temp name.
  let l:tmp = printf('%s.%d.tmp', l:target, getpid())
  if writefile(l:lines, l:tmp) != 0
    call delete(l:tmp)
    return
  endif
  if rename(l:tmp, l:target) != 0
    call delete(l:tmp)
  endif
endfunction

" `<afile>` rather than `%`: on BufUnload the current buffer is not necessarily
" the one going away.
function! GlrWipClear(name) abort
  let l:target = GlrWipSidecar(a:name)
  if !empty(l:target)
    call delete(l:target)
  endif
endfunction

" VimLeave fires once, with no file of its own, so a pattern would never match
" and every open buffer's sidecar would survive the session that made it.
function! GlrWipClearAll() abort
  for l:buf in getbufinfo({'buflisted': 1})
    for l:pat in g:glr_wip_patterns
      if l:buf.name =~ glob2regpat(l:pat)
        call GlrWipClear(l:buf.name)
        break
      endif
    endfor
  endfor
endfunction

augroup glr_wip
  autocmd!
  for s:glr_wip_pat in g:glr_wip_patterns
    " The `I` variants are the point: they fire while still in insert mode,
    " which is where the half-typed row gl-repl parks comes from.
    execute 'autocmd TextChanged,TextChangedI,CursorMoved,CursorMovedI '
          \ . s:glr_wip_pat . ' call GlrWipPublish()'
    " Publish on open too, so gl-repl starts following without waiting for the
    " first keystroke.
    execute 'autocmd BufReadPost ' . s:glr_wip_pat . ' call GlrWipPublish()'
    execute 'autocmd BufUnload ' . s:glr_wip_pat
          \ . " call GlrWipClear(expand('<afile>'))"
  endfor
  unlet! s:glr_wip_pat
  autocmd VimLeave * call GlrWipClearAll()
augroup END
