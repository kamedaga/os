#!/bin/sh
# Prefer the GTK3 backend validated with PachaOS's Xfce session; allow explicit
# overrides for headless conversion and backend diagnostics.
: "${SAL_USE_VCLPLUGIN:=gtk3}"
export SAL_USE_VCLPLUGIN
case "$0" in */lowriter) set -- --writer "$@" ;; esac
exec /usr/bin/libreoffice "$@"
