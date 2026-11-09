<!-- SPDX-License-Identifier: Apache-2.0 -->
<!--
Pull request title: use a Conventional Commit, for example:
  feat: add VLAN tagging on the Ethernet port
  fix(eth): recover when the PHY link flaps
  feat!: change the default management address     (breaking change)
Pull requests are squash-merged, and the title becomes the commit message. release-please reads it to
choose the next version and write CHANGELOG.md. See docs/RELEASING.md.
Types: feat, fix, perf, docs, build, deps, refactor, ci, test, style, chore, revert.
-->

## What and why

## How you tested it

<!-- Boards, wired device, and Wi-Fi network; serial log excerpts if relevant. -->

## Checklist

- [ ] The pull request title is a Conventional Commit (`type(scope): summary`, with `!` for breaking changes).
- [ ] I built it with `idf.py build` (ESP-IDF v5.4) and tested it on hardware, or I explained why not.
- [ ] The README or docs describe any user-visible changes.
