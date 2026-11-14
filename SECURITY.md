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

We will acknowledge your report within 72 hours and work with you to understand
and address the issue.

## Scope

The following component is in scope for security reports:

- The Cyclone Cache library (this repository): the cache core, the C and C++
  APIs, the plugin system, and the on-disk format parsers

Reports for the products that embed Cyclone Cache — mod_pagespeed 2.1 and the
PageSpeed optimizer — are covered by the security policies of their own
repositories.

Third-party dependencies are out of scope. If you find a vulnerability in a
dependency, please report it to the upstream maintainer and notify us so we can
assess impact.

## Fixes and advisories

Cyclone Cache reaches users as part of mod_pagespeed, so a security fix ships
in the next mod_pagespeed release that includes it. That release comes with a
published security advisory that lists the impact and the affected and fixed
versions, and credits the reporter if they want to be credited.

## Supported versions

Only the latest version of Cyclone Cache, as shipped in the latest
mod_pagespeed release, receives security updates. Users on older versions
should upgrade to receive fixes.

## Disclosure policy

We follow a 90-day coordinated disclosure timeline. Once a vulnerability is
reported:

1. We confirm receipt within 72 hours.
2. We investigate and develop a fix.
3. We coordinate a release within 90 days of the initial report.
4. We publish a security advisory when the fix is released.
5. After the fix is released, the reporter may publish details.

If you would like to be credited for your report, let us know and we will
include your name (or alias) in the security advisory and the release notes
of the release that ships the fix.

## Contact

- **Private vulnerability reporting:**
  https://github.com/We-Amp/cyclone-cache/security/advisories/new
- **Email:** security@modpagespeed.com
- **Company:** We-Amp B.V., Castricum, The Netherlands
