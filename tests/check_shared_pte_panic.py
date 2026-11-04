#!/usr/bin/env python3
"""Execute a dedicated invalid-PTE fixture; normal return is a failure."""
import resource
import subprocess
import sys
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
p = subprocess.run([sys.argv[1]], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
if p.returncode == 0 or (sys.argv[2].encode() if len(sys.argv) > 2 else b'present user PTE lacks user bit') not in p.stdout:
    sys.stdout.buffer.write(p.stdout)
    raise SystemExit('mandatory PTE panic missing')
