## Releasing MediaCurator

### Single source of truth

The app version lives in **one place**: the `project(VERSION ...)` call at the top of
`CMakeLists.txt`. Everything else derives from it:

| Consumer | How |
|----------|-----|
| In-app version (About/splash) | `include/core/Version.h.in` → generated `Version.h` → `MC_VERSION_STRING`, used in `src/main.cpp` |
| Windows installer / macOS bundle / Linux package | `PROJECT_VERSION*` read directly in `cmake/Packaging.cmake` |
| Git tag / GitHub Release | Set manually when you cut a release — must match `PROJECT_VERSION` |

Never hardcode a version string anywhere else. If you find one, replace it with
`MC_VERSION_STRING` (C++) or `${PROJECT_VERSION}` (CMake).

---

### Cutting a release

#### Step 1 — Bump the version

Edit `CMakeLists.txt`:

```cmake
project(MediaCurator
    VERSION 1.8.5
    ...
)
```

Use [semantic versioning](https://semver.org/): `MAJOR.MINOR.PATCH`.
- **PATCH** — bug fixes, no behavior/schema changes
- **MINOR** — new features, backwards-compatible
- **MAJOR** — breaking changes (e.g. DB schema migration with no upgrade path)

#### Step 2 — Commit and tag

The tag **must** match the version in `CMakeLists.txt`, prefixed with `v`. The commit
message **must** start with `Release ` (exact prefix) — CI uses that to skip a
redundant build on the plain `main` push, see note below:

```powershell
git add CMakeLists.txt
git commit -m "Release 1.8.5"
git push origin main

git tag v1.8.5
git push origin v1.8.5
```

> `build.yml`'s `build-windows`/`build-macos`/`build-linux` jobs skip themselves when
> the push is to `refs/heads/main` **and** the commit message starts with `Release `
> — that combination only ever means "the version-bump commit that's about to be
> tagged," so building it would just duplicate the build the tag push triggers next.
> The tag push (`refs/tags/vX.Y.Z`) is unaffected by this check and always builds.
> If you commit with a different message (or amend it), the `main` push will build
> normally — harmless, just redundant once you tag.

#### Step 3 — Let CI do the rest

Pushing the `v*` tag triggers `.github/workflows/build.yml`:

1. `build-windows`, `build-macos`, `build-linux` each configure, build, and package
   (NSIS `.exe`, `.dmg`, `.deb`) with the version baked in from `CMakeLists.txt`.
2. A `release` job (runs only on tag pushes) downloads all three installers and
   publishes a GitHub Release for the tag via `softprops/action-gh-release`, with
   release notes auto-generated from commits since the previous tag.

Watch progress under the repo's **Actions** tab. Once green, the Release appears
under **Releases** with all three installers attached.

#### Step 4 — Sanity check

Download at least one installer from the Release page and confirm the About/splash
screen shows the version you expect — this is the check that the single-source-of-truth
wiring didn't drift.

---

### Fixing a release after the fact

Don't move an existing tag to a new commit — treat pushed tags as immutable. If a
release build had a bug, bump the patch version and cut a new tag instead
(e.g. `v1.2.0` → `v1.1.1`).

If a release job fails after the platform builds already succeeded (e.g. a transient
`action-gh-release` error), re-run just the failed `release` job from the Actions tab
rather than re-pushing the tag.

---

### One-time repo setting

`release` needs write access to create the GitHub Release. If it fails with a
permissions error, check **Settings → Actions → General → Workflow permissions** and
ensure "Read and write permissions" is selected.
