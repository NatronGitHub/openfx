#!/bin/bash
# Sets MACOSX_DEPLOYMENT_TARGET to the runner's macOS for the next steps of a CI job. CI builds
# aren't shipped; the release bundles target macOS 14.0 (VFX Reference Platform CY2027) and are
# built on a macOS 14 machine. In a job that installs MacPorts, run it after the install: the
# ports are MacPorts' binary archives for the runner's macOS.
set -eo pipefail
echo "MACOSX_DEPLOYMENT_TARGET=$(sw_vers -productVersion | cut -d. -f1).0" >> "$GITHUB_ENV"
