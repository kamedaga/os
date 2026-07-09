#!/bin/sh

if /cmd/lpr_pthread_static.elf; then
    echo LPR_PTHREAD_STATIC=OK
else
    echo LPR_PTHREAD_STATIC=BAD
    exit 1
fi

if /cmd/lpr_pthread_dynamic.elf; then
    echo LPR_PTHREAD_DYNAMIC=OK
else
    echo LPR_PTHREAD_DYNAMIC=BAD
    exit 1
fi

echo LPR_PTHREAD_DONE
