# wobble-psx

A PlayStation 1 emulator, named after the console's famously wobbly
vertex graphics.

## Building

Requires CMake ≥ 3.24 and a C++20 compiler. SDL3 and Dear ImGui are
fetched and built automatically by CMake (nothing is installed to the
system). On Debian/Ubuntu the X11/Wayland dev headers SDL needs come
from `build-essential libx11-dev libxext-dev libwayland-dev
libxkbcommon-dev libgl1-mesa-dev`.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
./build/wobble
```

## References

- [psx-spx](https://psx-spx.consoledev.net/) — hardware documentation
