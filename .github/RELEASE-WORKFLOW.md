# Release Workflow

Two repositories are involved:

- `mxmlnkn/librapidarchive` (also called `librapidarchive`) contains the C++ library, the rapidgzip changelog,
  and the Python packaging metadata for both `indexed_bzip2` and `rapidgzip`.
- `mxmlnkn/rapidgzip` contains the rapidgzip release CI. It includes `librapidarchive` as the
  `librapidarchive` submodule. Its top-level `CHANGELOG.md` is only a pointer to the changelog in
  `librapidarchive`.

The Python packaging files in `rapidgzip/python/rapidgzip` are symbolic links into the submodule. This
includes the changelog. Version bumps and changelog entries therefore always happen in `librapidarchive`,
and the rapidgzip repository only records the new submodule commit in its release commit.

## Releasing rapidgzip

1. Bump the version in `librapidarchive` and push it first, so that the submodule commit exists on GitHub:

   - `python/rapidgzip/pyproject.toml`
   - `src/rapidgzip/rapidgzip.hpp` (`RAPIDGZIP_VERSION_MAJOR`, `RAPIDGZIP_VERSION_MINOR`, `RAPIDGZIP_VERSION_PATCH`)
   - `python/rapidgzip/CHANGELOG.md` (new section at the top)

   ```bash
   git commit -m "[version] Bump rapidgzip version to X.Y.Z"
   git push origin
   ```

2. In the `rapidgzip` repository, update the submodule:

   ```bash
   cd librapidarchive
   git fetch origin
   git checkout <librapidarchive version-bump commit>
   cd ..
   git add librapidarchive
   ```

3. Commit the submodule update:

   ```bash
   git commit -m "[version] Bump version to X.Y.Z"
   ```

4. Create an annotated tag whose message is the changelog entry. The binary release workflow uses the tag
   message as the GitHub release body:

   ```bash
   git tag -a rapidgzip-vX.Y.Z -F changelog-entry.txt
   ```

5. Push the branch and the tag:

   ```bash
   git push origin main
   git push origin rapidgzip-vX.Y.Z
   ```

6. The tag starts these workflows in the `rapidgzip` repository:

   - `check-version.yml` checks that the C++ header version, `pyproject.toml` version, top changelog version
     in `python/rapidgzip/CHANGELOG.md`, and tag version all match.
   - `wheels-rapidgzip.yml` reacts to `rapidgzip-v*.*.*` tags, publishes the sdist and wheels to PyPI, and
     waits for the version check.
   - `conda-rapidgzip.yml` publishes the conda package.
   - `publish-binaries.yml` creates a GitHub release with static binaries.

   The AUR package is not updated anymore. `python-rapidgzip` is in the official Arch repository `extra`:
   <https://archlinux.org/packages/extra/x86_64/python-rapidgzip/>

## Releasing indexed_bzip2

1. Bump the version in these files:

   - `python/indexed_bzip2/pyproject.toml`
   - `python/indexed_bzip2/indexed_bzip2.pyx` (`__version__`)
   - `python/indexed_bzip2/conda/meta.yaml` (`package.version`)
   - `src/tools/ibzip2.cpp` (the `--version` string)

2. Add the release notes to `python/indexed_bzip2/CHANGELOG.md`. The root `CHANGELOG.md` is a symbolic link
   to this file.

3. Commit, create an annotated tag with the changelog entry as its message, and push:

   ```bash
   git commit -m "[version] Bump indexed_bzip2 version to X.Y.Z"
   git tag -a vX.Y.Z -F /tmp/changelog-entry.txt
   git push origin master
   git push origin vX.Y.Z
   ```

4. The tag starts these workflows in `librapidarchive`:

   - `wheels.yml` publishes the sdist and wheels to PyPI.
   - `conda.yml` publishes the conda package.

   `build-binaries.yml` only runs on `master` and manual dispatch and uploads workflow artifacts. It does not
   create a GitHub release.
