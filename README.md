This is an experimental vim fork with multi-threading capabilities

Instructions:

First make sure you have python development headers as right now only python
bindings can access the main message loop. On ubuntu enter
`sudo apt-get install python-dev` to install.

Then:

```sh
git clone git://github.com/tarruda/vim
cd vim
git checkout event-loop
make distclean
(cd src && make autoconf)
./configure --enable-pythoninterp
make
```

After compilation enter `./src/vim --version` and it should output
+messagequeue as an included feature.

A simple demonstration on how a plugin may use the message queue is included.
To run:

```sh
./src/vim -u snake.vim
```

or 

```sh
./src/vim -U snake.vim -g
```

There's no guarantee that this will ever get merged with mainline but I will
try to keep it in sync with the official repository.

Plugin authors can test if this feature is available through using
`has('messagequeue')`.
