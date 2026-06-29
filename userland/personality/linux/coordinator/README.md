# Linux Coordinator

The coordinator owns shared Linux state only.

It must not become a universal syscall server.

Coordinator-owned state includes:

- pid/tid maps
- thread groups
- parent/child state
- wait queues
- signal queues
- shared fd table metadata
