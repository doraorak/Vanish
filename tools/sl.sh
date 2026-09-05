#!/bin/bash
# Runs lldb commands (stdin) against a throwaway host that has SkyLight dlopen'd.
# Deliberately NOT WindowServer: attaching lldb there stops the compositor.
S="$(cd "$(dirname "$0")" && pwd)"
pgrep -x slhost >/dev/null || { nohup "$S/slhost" >/dev/null 2>&1 < /dev/null & sleep 2; }
P=$(pgrep -x slhost | head -1)
[ -z "$P" ] && { echo "slhost not running"; exit 1; }
{ echo "process attach --pid $P"; cat; echo "detach"; echo "quit"; } > "$S/.run.lldb"
sudo -n lldb -b -s "$S/.run.lldb" 2>&1
