package cli

import (
	"reflect"
	"strings"
	"testing"

	"capabilityos/pack/internal/qemu"
)

func TestParseOnlySelectsStages(t *testing.T) {
	for _, tc := range []struct {
		values []string
		want   prepareSelection
	}{
		{nil, prepareSelection{Kernel: true, Userland: true}},
		{[]string{"kernel"}, prepareSelection{Kernel: true}},
		{[]string{"userland"}, prepareSelection{Userland: true}},
		{[]string{"userland:filed,gpud_core", "userland:filed", "kernel"}, prepareSelection{Kernel: true, Userland: true, Apps: []string{"filed", "gpud_core"}}},
		{[]string{"userland:filed", "userland"}, prepareSelection{Userland: true}},
	} {
		got, err := parseOnly(tc.values)
		if err != nil {
			t.Fatalf("%v: %v", tc.values, err)
		}
		if !reflect.DeepEqual(got, tc.want) {
			t.Fatalf("%v: got %+v want %+v", tc.values, got, tc.want)
		}
	}
	for _, values := range [][]string{{"rootfs"}, {"userland:"}, {"userland:a,,b"}, {"userland", "userland:a"}} {
		if _, err := parseOnly(values); err == nil {
			t.Fatalf("%v accepted", values)
		}
	}
}

func TestOnlyRequiresPrepare(t *testing.T) {
	err := prepareOrCheckBoot(&context{}, &qemu.Options{}, false, []string{"kernel"}, false)
	if err == nil || !strings.Contains(err.Error(), "--prepare") {
		t.Fatalf("err = %v", err)
	}
}

func TestStrictFreshnessRefusesToBoot(t *testing.T) {
	issues := []freshnessIssue{{"kernel", "kernel input changed: src/main.zig"}}
	if err := reportFreshness(issues, false, "hint"); err != nil {
		t.Fatal(err)
	}
	if err := reportFreshness(issues, true, "hint"); err == nil {
		t.Fatal("strict mode booted a stale image")
	}
	if err := reportFreshness(nil, true, "hint"); err != nil {
		t.Fatal(err)
	}
}
