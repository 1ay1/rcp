# Publishing the RCP SDKs

The four reference SDKs are **published to their registries** (v1.0.0). This
guide has the release flow for cutting the next version.

| SDK | Registry | Package | Install |
|-----|----------|---------|---------|
| Python | [PyPI](https://pypi.org/project/rcp-protocol/) | `rcp-protocol` | `pip install rcp-protocol` (imports as `rcp`) |
| Node.js | [npm](https://www.npmjs.com/package/rcp-protocol) | `rcp-protocol` | `npm install rcp-protocol` |
| Rust | [crates.io](https://crates.io/crates/rcp-protocol) | `rcp-protocol` | `cargo add rcp-protocol` (crate `rcp`) |
| C++ | vcpkg / CMake | `rcp` | `find_package(rcp CONFIG REQUIRED)` |

> **Name availability:** `rcp-protocol` on PyPI/npm/crates.io and `rcp` on vcpkg
> are assumed free. If any is taken, pick a fallback (e.g. `rcp-sdk`) and update
> the package name in the corresponding manifest before publishing.

## One-time setup

### PyPI (Trusted Publishing — no token)

1. Create the project on PyPI (or reserve the name by publishing once manually).
2. In the PyPI project → **Settings → Publishing**, add a **GitHub Actions**
   trusted publisher:
   - Owner: `1ay1`  ·  Repo: `rcp`  ·  Workflow: `release.yml`  ·  Environment: `release`
3. No secret needed — the `release.yml` `pypi` job authenticates via OIDC.

### npm

1. `npm login` (or create an **automation** access token in npm → Access Tokens).
2. Add it as a repo secret: **Settings → Secrets and variables → Actions →**
   `NPM_TOKEN`.

### crates.io

1. `cargo login` then create an API token at crates.io → Account Settings → API
   Tokens (scope: publish-new + publish-update).
2. Add it as a repo secret: `CARGO_REGISTRY_TOKEN`.

### GitHub environment

Create an environment named **`release`** (Settings → Environments) — optionally
with required reviewers so a human approves each publish.

## Releasing (automated)

Bump the version in all three manifests to the same value, commit, then tag:

```sh
# sdk/python/pyproject.toml  → version = "1.0.1"
# sdk/node/package.json      → "version": "1.0.1"
# sdk/rust/Cargo.toml        → version = "1.0.1"

git commit -am "Release SDKs 1.0.1"
git tag v1.0.1
git push origin main --tags
```

The `Release SDKs` workflow fans out and publishes to PyPI, npm, and crates.io.
Each registry job is independent.

## Releasing (manual, first time / fallback)

If you'd rather publish by hand the first time to reserve the names:

```sh
# Python
cd sdk/python
python -m pip install --upgrade build twine
python -m build
python -m twine check dist/*
python -m twine upload dist/*            # prompts for PyPI credentials

# Node
cd sdk/node
npm publish --access public              # after `npm login`

# Rust
cd sdk/rust
cargo publish                            # after `cargo login`
```

All three were verified locally: `python -m build` + `twine check` PASS,
`npm pack --dry-run` includes LICENSE + README (11 files), and
`cargo package` compiles the packaged crate clean.

## C++ — vcpkg

The C++ SDK is header-only C++23 and installs via CMake today:

```sh
cmake -S sdk/cpp -B build -DCMAKE_INSTALL_PREFIX=/usr/local
cmake --install build
# consumers: find_package(rcp CONFIG REQUIRED); target_link_libraries(app PRIVATE rcp::rcp)
```

`sdk/cpp/vcpkg.json` is the port manifest. To land it in the public vcpkg
registry, submit a port (a `portfile.cmake` that points at a tagged release
tarball) to microsoft/vcpkg, or host a private overlay/registry. This is the one
SDK whose "publish" step is a PR to an external repo rather than a `push`.

## After the first publish

Update the docs quickstart (`docs/get-started/quickstart.mdx`) and the SDK pages
to show the registry install commands instead of the source-install caveats, and
remove the "unpublished — install from source" notes.
