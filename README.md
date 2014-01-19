An experimental vim fork with multi-threading capabilities.

Vim code still only executes in the main thread, but a thread-safe queue is
provided that  other threads can use to notify vim main loop about events.

Events published to the queue will be executed as ['User'
autocommands](http://vimdoc.sourceforge.net/htmldoc/autocmd.html#autocmd-events)
with the filename matching the event name and v:event_arg as a string
argument(empty string if no arguments were passed to the event).

Here's a simple example:

```vim
" This only works if vim is compiled with --enable-eventloop
if has('event_loop')

python << EOF
import vim
from threading import Thread
from time import sleep

def run():
    while True:
        sleep(1)
        vim.trigger('my-custom-event', 'this message came from another thread!')

t = Thread(target=run)
t.daemon = True # Only daemon threads will be killed when vim exits
t.start()
EOF

function! Notify()
python << EOF
vim.current.buffer.append(vim.eval('v:event_arg'))
EOF
endfunction

au User my-custom-event call Notify()

endif
```

Instructions:

First make sure you have python development headers as right now only through
python scripting a plugin can access the event loop(I plan to add vimscript
functions that make use of the event loop once its more stable).

On ubuntu enter `sudo apt-get install python-dev` to install the necessary
headers. Then clone/compile:

```sh
git clone git://github.com/tarruda/vim
cd vim
make distclean
(cd src && make autoconf)
./configure --enable-pythoninterp --enable-eventloop --with-features=huge
make
```

After compilation enter `./src/vim --version` and it should output
+event_loop as an included feature.

A more complex demonstration is included in the branch:

```sh
./src/vim -u snake.vim
```

or 

```sh
./src/vim -U snake.vim -g
```

There's no guarantee that this will ever get merged with mainline but I will
do my best to keep it in sync with the official repository.

Plugin authors can test if this feature with `has('event_loop')` as shown
in the example above.
