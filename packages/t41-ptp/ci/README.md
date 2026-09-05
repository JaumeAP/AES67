# Board build

`platformio.ini` here builds an example for the Teensy 4.1, against the
library in this repository and the two dependencies under `libraries/`.

`src/main.cpp` is written from `examples/PTPNode/PTPNode.ino` with the
prototypes an `.ino` gets from the Arduino preprocessor and a `.cpp` does
not. The `board` target in `test/Makefile` does that and runs
`pio run -d ci`, and the workflow calls the same target:

    make -C test board

It is the only copy of the recipe; nothing it writes is versioned, and
`make -C test clean` removes `src/main.cpp` and `.pio/`.
