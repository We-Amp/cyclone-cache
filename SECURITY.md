# Security policy

## Reporting a vulnerability

If you discover a security vulnerability in Cyclone Cache, please report it
privately in one of these ways:

- **GitHub private vulnerability reporting:** open the
  [Security tab](https://github.com/We-Amp/cyclone-cache/security) of this
  repository and choose **Report a vulnerability**, or go straight to the
  [report form](https://github.com/We-Amp/cyclone-cache/security/advisories/new).
- **Email:** **security@modpagespeed.com**

Please do not open a public issue or pull request for a vulnerability. Both
are visible to everyone before a fix is available.

We will acknowledge your report and work with you to understand and address
the issue. We keep you informed as we work on it and tell you when
the fix is released, with the version that carries it.

We do not publish a PGP key for reports. GitHub private vulnerability
reporting is the encrypted channel; if you need another one, ask by email and
we will arrange it before you send details.

A good report has a description of the issue and its impact, the affected
version or commit, and the steps or input that reproduce it. We do not run a
bug bounty.

## Scope

The following component is in scope for security reports:

- The Cyclone Cache library (this repository): the cache core, the C and C++
  APIs, the plugin system, and the on-disk format parsers

Reports for the products that embed Cyclone Cache, the mod_pagespeed modules
and the optimizer worker, are covered by the security policies of their own
repositories, which are the same as this one.

Third-party dependencies are out of scope. If you find a vulnerability in a
dependency, please report it to the upstream maintainer and notify us so we can
assess impact.

## Fixes and announcements

Cyclone Cache reaches users as part of mod_pagespeed, so a security fix ships
in the next mod_pagespeed release that includes it, and that release's
announcement covers it.

- **Announcements.** Security fixes are announced in the security section of
  the release notes and in the CHANGELOG, with the impact class, the affected
  and fixed versions, and "update recommended". The same entries are in the
  [security updates table](https://modpagespeed.com/docs/release-notes/#security-updates).
  The credit goes in the same entry, if the reporter wants to be credited.
- **CVE ids.** When a CVE id has been assigned for a fixed issue, it is included
  in the entry. Many entries have none; the table says so.
- **Level of detail.** The announcements describe the impact and
  say who should update. They do not describe how to trigger an issue.
- **Machine-readable data.** Each mod_pagespeed release carries an SPDX SBOM
  and an OpenVEX document that cover Cyclone Cache as a component.

## Supported versions

Security fixes are made in the latest version of Cyclone Cache, as shipped in
the latest mod_pagespeed release. Upgrading to that release is the supported
path from any earlier one.

## Disclosure policy

We follow coordinated disclosure. Once a vulnerability is reported:

1. We confirm receipt.
2. We investigate and develop a fix.
3. We coordinate a release with the reporter. If a fix needs more time, we
   agree a date with the reporter.
4. We announce the fix in the release notes when it is released.
5. After the fix is released, the reporter may publish details. We ask that
   users get time to update first.

Findings from automated services follow the service's own disclosure
deadline.

If a vulnerability is being exploited, we do not wait: the fix is released as
soon as it is ready and announced with it.

If you would like to be credited for your report, let us know and we will
include your name (or alias) in the release notes
of the release that ships the fix.

## Contact

- **Private vulnerability reporting:**
  https://github.com/We-Amp/cyclone-cache/security/advisories/new
- **Email:** security@modpagespeed.com
- **Company:** We-Amp B.V., Castricum, The Netherlands
