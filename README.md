# tgit2

Clone Git repositories from Tcl using libgit2.

```tcl
package require tgit2 0.1.0
tgit2::clone -depth 1 https://github.com/jerily/ttrek-registry.git ./registry
```

`tgit2::clone ?-depth count? url directory` checks out the remote's default branch.
The destination must be absent or empty. Omit `-depth` for full history.
Shallow clones require a server transport that supports them.

Build with CMake, Tcl 8.6 or 9.x development files, and libgit2 1.9 with HTTPS support.

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build
```
