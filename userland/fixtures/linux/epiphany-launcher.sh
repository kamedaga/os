#!/bin/sh
# Porting environment: isolation is not implemented. Explicitly authorized for
# Epiphany testing; do not export this setting to the desktop session globally.
export WEBKIT_DISABLE_SANDBOX_THIS_IS_DANGEROUS=1
exec /usr/bin/epiphany "$@"
