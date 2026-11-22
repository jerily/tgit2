# tgit2

Manage Git repositories and linked worktrees from Tcl using libgit2.

```tcl
package require tgit2 0.2.0
tgit2::clone -depth 1 https://github.com/jerily/ttrek-registry.git ./registry
```

## Commands

| Command | Result |
| --- | --- |
| `tgit2::clone ?-depth count? url directory` | Checks out the default branch. The destination must be absent or empty. Omit `-depth` for full history. Shallow clones require a compatible server. |
| `tgit2::init directory` | Creates a bare repository with an empty initial commit on `main` and returns its full object ID. |
| `tgit2::worktree add repo name directory commitId` | Creates a linked checkout and its own branch at the specified commit. |
| `tgit2::worktree list repo` | Returns the names of linked worktrees. |
| `tgit2::worktree remove repo name` | Deletes a worktree created by `add`, including its uncommitted edits, registration, and private branch. |
| `tgit2::worktree prune repo name` | Removes the registration and private branch of a missing worktree. |
| `tgit2::status worktree` | Returns changed paths mapped to libgit2 `git_status_t` bitmasks. Includes untracked files. |
| `tgit2::diff worktree` | Returns the patch from HEAD through current worktree contents, including untracked files. |
| `tgit2::stage worktree paths` | Stages a Tcl list of literal worktree-relative paths. Missing tracked files are staged for deletion. |
| `tgit2::commit worktree paths message` | Stages the requested paths and commits the index. Returns the full commit ID. An unchanged tree returns its existing commit. |
| `tgit2::ref get repo refName` | Resolves a reference such as `HEAD` or `refs/heads/main` to a full object ID. |
| `tgit2::ref update repo refName expectedId newId` | Replaces the direct reference when its current ID matches the expected ID. |
| `tgit2::export_bare repo commitId directory` | Writes a standalone bare repository with a single branch named `main` and the selected commit's reachable history. Returns the tip ID. |
| `tgit2::verify repo commitId` | Checks that Git can traverse the commit history and find its referenced objects. Returns the requested commit ID on success. |

Use native filesystem paths and full hexadecimal commit IDs. The destinations for
`init`, `worktree add`, and `export_bare` must be new directories with existing
parents. A worktree name is one branch-name component. Each checkout owns a
branch under `refs/heads/tgit2-worktrees/`. Export or retain a reference to a
commit before removing its checkout. Commits use the identity
`tgit2 <tgit2@localhost>`.

## Build from source

Use CMake, Tcl 8.6 or 9.x development files, and libgit2 1.9.7 with HTTPS support.
Set `prefix` to the dependency installation containing the target Tcl headers,
stub library, shell and libgit2 CMake package. Build once for each Tcl major ABI.

```sh
prefix=/absolute/path/to/dependency/prefix
cmake -S . -B build -DCMAKE_PREFIX_PATH="$prefix" \
    -DTCL_INCLUDE_PATH="$prefix/include" -DTCL_LIBRARY_DIR="$prefix/lib" \
    -DCMAKE_INSTALL_PREFIX="$prefix"
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build
```

A separate libgit2 installation can be selected with `-Dlibgit2_DIR` pointing to
its directory containing `libgit2Config.cmake`. The tests also use the Git CLI to
check interoperability.
