if has('event_loop')

set nocompatible
set nocursorline

python << EOF
import sys
sys.path.append('.')
import snake
import vim
buf = vim.current.buffer
empty = ' ' * 80
buf[0] = empty
for n in xrange(20):
    buf.append(empty)
EOF
 
function! Update()
python << EOF
snake.update()
EOF
endfunction

function! End()
unmap <buffer> k
unmap <buffer> j
unmap <buffer> h
unmap <buffer> l
unmap <buffer> <esc>
unmap <buffer> <space>
python << EOF
snake.end()
EOF
endfunction

function! KeyPress(k)
python << EOF
snake.keypress(vim.eval('a:k'))
EOF
endfunction

nnoremap <silent> <buffer> k :call KeyPress('up')<cr>
nnoremap <silent> <buffer> j :call KeyPress('down')<cr>
nnoremap <silent> <buffer> h :call KeyPress('left')<cr>
nnoremap <silent> <buffer> l :call KeyPress('right')<cr>
nnoremap <silent> <buffer> <esc> :call KeyPress('esc')<cr>
nnoremap <silent> <buffer> <space> :call KeyPress('space')<cr>

au User update-screen call Update()
au User end-game call End()

python << EOF
snake.start()
EOF
endif

