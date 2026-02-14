cmake --preset debug
cmake --build --preset debug

cmake --preset relwithdebinfo
cmake --build --preset relwithdebinfo

cmake --preset release
cmake --build --preset release

cmake --preset asan
cmake --build --preset asan