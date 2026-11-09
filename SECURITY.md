<!-- SPDX-License-Identifier: Apache-2.0 -->
# Security

## Report a vulnerability

Report vulnerabilities privately with **Report a vulnerability** on the
[Security tab](../../security/advisories/new). Don't open a public issue. Include the firmware version and
the steps to reproduce the problem. You'll get a reply within a week.

Fixes go into the latest release only.

## Deploy a bridge safely

- The bridge's page has no password until you set one. Until then, anyone on the same network can change
  its settings.
- The page uses plain HTTP, so others on the same network can read a password that you set there.
- The setup network is open unless you give it a password with `ApPassword`.
