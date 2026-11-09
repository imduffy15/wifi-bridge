<!-- SPDX-License-Identifier: Apache-2.0 -->
# Releasing

[release-please](https://github.com/googleapis/release-please) automates releases. Don't edit
`version.txt` or `CHANGELOG.md` by hand, and don't create tags or upload binaries manually.

## How a release works

1. **Conventional commits land on `main`.** Pull requests are squash-merged, and the pull request title
   becomes the commit message. The title must be a [Conventional Commit](https://www.conventionalcommits.org/):
   - `fix: ...` produces a patch release (`0.1.0` to `0.1.1`).
   - `feat: ...` produces a minor release (`0.1.1` to `0.2.0`).
   - `feat!: ...`, `fix!: ...`, or a `BREAKING CHANGE:` footer produces a major release (a minor release
     while the version is below 1.0).
   - `perf`, `revert`, `docs`, `build`, and `deps` commits appear in the changelog and count as patch-level
     changes. `refactor`, `ci`, `test`, `style`, and `chore` commits don't appear in the changelog.
2. **release-please opens a release pull request.** Every push to `main` runs
   `.github/workflows/release-please.yml`, which opens or updates a pull request titled
   `chore: release X.Y.Z`. The pull request bumps `version.txt` (which `CMakeLists.txt` reads into
   `PROJECT_VER`) and updates `CHANGELOG.md` and `.release-please-manifest.json`.
3. **To ship, merge the release pull request.** release-please then tags the merge commit `vX.Y.Z` and
   creates a GitHub release with the changelog section as its notes.
4. **The same workflow builds and attaches the firmware.** It checks out the tag, builds it in
   `espressif/idf:v5.4`, runs `tools/package.sh`, and uploads these files to the release:

   | File | Use |
   |---|---|
   | `wifi-bridge-X.Y.Z.ota.bin` | App image only. Use it for a firmware update from the bridge's page, or install it from the safeboot firmware. |
   | `wifi-bridge-X.Y.Z.safeboot.bin` | Safeboot firmware for boards with the safeboot layout. Install it from the running bridge. |
   | `wifi-bridge-X.Y.Z.factory.bin` | Bootloader, partition table, and app. Flash it at `0x0` over serial, or use the web installer. |
   | `wifi-bridge-X.Y.Z.elf` | Symbols for decoding crash backtraces. |
   | `SHA256SUMS` | Checksums of the packaged files. |

   The build fails if `version.txt`, the tag, and the version compiled into the firmware don't match.
5. **The web installer is redeployed.** The workflow downloads the factory and OTA images from the release,
   verifies them against `SHA256SUMS`, fills in `web-installer/manifest.json` with
   `tools/build-installer.sh`, and deploys the site to GitHub Pages.

The *safeboot firmware* is a small recovery firmware for boards with a single firmware slot. It installs
firmware updates while the main firmware isn't running.

To redeploy the installer without a new release (for example, after you change
`web-installer/index.html`), run the **Release** workflow manually from the **Actions** tab. With no input,
it deploys the latest release. You can also enter a tag such as `v0.1.0`. To build that tag and attach its
firmware to the release first (for a release created by hand), also select **build**. The page files come
from the branch that you run the workflow on, which must be `main` (see
[One-time repository setup](#one-time-repository-setup)).

## Versions before 1.0

Version 0.1.0 was published by hand, and release-please continues from it. The `bump-minor-pre-major`
setting keeps breaking changes on 0.x: they bump the minor version (`0.1.0` to `0.2.0`) instead of jumping
to 1.0.0. When the
project is ready for 1.0, remove that setting from `release-please-config.json`.

## One-time repository setup

1. **Actions permissions.** Go to **Settings > Actions > General > Workflow permissions**. Leave the default
   token read-only (each job requests what it needs), but select **Allow GitHub Actions to create and
   approve pull requests**. release-please needs this to open its pull request.
2. **CI on release pull requests (optional, recommended).** Pull requests opened with the built-in
   `GITHUB_TOKEN` don't trigger other workflows, so CI doesn't run on the release pull request. To fix
   this, create a fine-grained personal access token or a GitHub App token for this repository with
   *Contents: read and write* and *Pull requests: read and write*. Store it as the Actions secret
   `RELEASE_PLEASE_TOKEN`. When the secret is set, the workflow uses it automatically.
3. **GitHub Pages.** Go to **Settings > Pages > Build and deployment > Source** and choose
   **GitHub Actions**. This creates the `github-pages` environment, which by default accepts deployments
   only from `main`. Keep that setting.
4. **Squash merging.** Go to **Settings > General > Pull Requests**. Allow squash merging only, and set the
   default commit message to *Pull request title*, so every commit on `main` is a conventional commit.

### Set up the public installer repository

Use this while this repository is private. GitHub Pages on a private repository needs a paid plan, and the
site is public anyway. So the release workflow also publishes the installer page and the release binaries
(never the source) to a separate **public** repository, `<this repo>-installer`. To use a different
repository, set the Actions variable `INSTALLER_REPO`. The public repository serves the files with GitHub
Pages from its `main` branch.

To set it up:

1. Create the public repository and enable Pages: **Settings > Pages > Deploy from a branch > `main` /
   root**.
2. Create an SSH key pair. Add the public key to the installer repository as a deploy key **with write
   access**, and store the private key in this repository as the Actions secret `INSTALLER_DEPLOY_KEY`.

Each release, or a manual run of the **Release** workflow, then pushes the new site. Without the secret, the
workflow skips this step and shows a notice. Once this repository is public, you can enable Pages here
instead and stop using the installer repository.

### Pages on a private repository

- GitHub Pages for a **private** repository needs a paid plan (GitHub Pro, Team, or Enterprise). On the Free
  plan, deployment fails until the repository is public or the plan is upgraded.
- **The published site is public**, even when the repository is private. On Enterprise Cloud, you can
  restrict it to organization members. Anyone with the URL can download the factory and OTA images from it.
- The release notes link on the installer page points to the GitHub release. While the repository is
  private, visitors without access get a 404 error there. The `.elf` file is attached only to the release
  and isn't published on Pages.

## Build a release locally

To build and package the firmware and safeboot firmware the same way CI does, run:

```sh
./build.sh dist     # move sdkconfig.secrets away first: its presets would end up in the files
```

To preview the web installer with those files, run the following. Web Serial works on `http://localhost`.

```sh
tools/build-installer.sh dist "$(cat version.txt)" _site
python3 -m http.server -d _site 8000     # open http://localhost:8000 in Chrome or Edge
```

CI also runs
`node tools/check-inline-js.mjs main/web/index.html safeboot/main/index.html web-installer/index.html` and
`shellcheck tools/*.sh`. Run them before you push changes to the web pages or the scripts.

## Fix a failed release

- **The build or upload failed after the release was created.** Fix the problem on `main` with a `fix:`
  commit and ship the next patch release. Or rerun the failed jobs from the **Actions** tab:
  `gh release upload --clobber` overwrites any partial uploads.
- **A bad release is out.** Don't delete or move the tag. Ship a fix as a new patch release. If needed, mark
  the bad release as a pre-release or edit its notes to warn users.
