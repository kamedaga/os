package buildsys

import (
	"reflect"
	"strings"
	"testing"
)

func TestParseStracePathsExecFailuresAndResumedCalls(t *testing.T) {
	log := `100 chdir("/work/sub") = 0
100 openat(AT_FDCWD</work/sub>, "header.h", O_RDONLY|O_CLOEXEC <unfinished ...>
100 <... openat resumed>) = 3</work/sub/header.h>
100 newfstatat(AT_FDCWD</work/sub>, "../source.c", {st_mode=S_IFREG|0644}, 0) = 0
101 execve("/nix/store/tool/bin/cc", ["cc"], 0x0) = 0
101 access("/work/missing.h", F_OK) = -1 ENOENT (No such file or directory)
101 openat(3</work/include>, "nested.h", O_RDONLY) = 4</work/include/nested.h>
101 openat(AT_FDCWD</work/sub>, "output.o", O_WRONLY|O_CREAT, 0666) = 5</work/sub/output.o>
`
	events, err := parseStrace(strings.NewReader(log), "/work")
	if err != nil {
		t.Fatal(err)
	}
	want := []straceEvent{
		{PID: 100, Syscall: "openat", Path: "/work/sub/header.h", Read: true},
		{PID: 100, Syscall: "newfstatat", Path: "/work/source.c", Read: true},
		{PID: 101, Syscall: "execve", Path: "/nix/store/tool/bin/cc", Read: true, Exec: true},
		{PID: 101, Syscall: "access", Path: "/work/missing.h", Missing: true},
		{PID: 101, Syscall: "openat", Path: "/work/include/nested.h", Read: true},
		{PID: 101, Syscall: "openat", Path: "/work/sub/output.o"},
	}
	if !reflect.DeepEqual(events, want) {
		t.Fatalf("events = %#v\nwant   = %#v", events, want)
	}
}

func TestParseStraceForkInheritsWorkingDirectory(t *testing.T) {
	log := `10 chdir("/work/sub") = 0
10 clone(child_stack=NULL, flags=SIGCHLD) = 11
11 open("child.h", O_RDONLY) = 3</work/sub/child.h>
`
	events, err := parseStrace(strings.NewReader(log), "/work")
	if err != nil {
		t.Fatal(err)
	}
	if len(events) != 1 || events[0].Path != "/work/sub/child.h" {
		t.Fatalf("events = %#v", events)
	}
}

func TestParseStraceAlignedResults(t *testing.T) {
	// Real strace output pads results to a column, including resumed calls.
	log := `200 execve("/nix/store/coreutils/bin/sort", ["sort", "-u"], 0x0 /* 3 vars */ <unfinished ...>
201 access("/work/short.h", R_OK)       = 0
200 <... execve resumed>)             = 0
201 openat(AT_FDCWD</work>, "a.h", O_RDONLY <unfinished ...>
201 <... openat resumed>)             = 3</work/a.h>
201 access("/work/gone.h", F_OK)        = -1 ENOENT (No such file or directory)
`
	events, err := parseStrace(strings.NewReader(log), "/work")
	if err != nil {
		t.Fatal(err)
	}
	want := []straceEvent{
		{PID: 201, Syscall: "access", Path: "/work/short.h", Read: true},
		{PID: 200, Syscall: "execve", Path: "/nix/store/coreutils/bin/sort", Read: true, Exec: true},
		{PID: 201, Syscall: "openat", Path: "/work/a.h", Read: true},
		{PID: 201, Syscall: "access", Path: "/work/gone.h", Missing: true},
	}
	if !reflect.DeepEqual(events, want) {
		t.Fatalf("events = %#v\nwant   = %#v", events, want)
	}
}
