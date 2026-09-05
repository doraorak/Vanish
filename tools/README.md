# Analysis tooling

SkyLight is cache-only, so `otool`/`nm` cannot read it and the server-side
symbols are local (invisible to `dyld_info`). lldb can read both, but only
against a mapped copy.

WindowServer is deliberately **not** that copy: attaching lldb to it stops the
compositor and freezes the machine. `slhost` is a stand-in that does nothing but
`dlopen` SkyLight and `pause()`.

```bash
clang -o slhost slhost.c
echo 'image lookup -r -s "CGXOrderWindow" -- SkyLight' | ./sl.sh
echo 'disassemble -n _XOrderWindow' | ./sl.sh
```

`sl.sh` starts `slhost` if it is not already up, attaches lldb as root, runs the
commands on stdin, and detaches. It needs `sudo -n lldb` to work.

Two flags that cost time to find:

- `image lookup -r -s <regex>` searches the **symbol table** and finds the local
  server-side names. `-r -n` searches function names, needs debug info, and
  silently finds nothing for these.
- Both `-o continue` and `-o quit` are required when driving lldb non-interactively,
  and a multi-line `-c` string breaks its argdumper — put commands in a file.
