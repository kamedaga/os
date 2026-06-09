package qemu

import (
	"io"
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestRunSendExpectTTY(t *testing.T) {
	socketPath := filepath.Join(t.TempDir(), "console.sock")
	listener, err := net.Listen("unix", socketPath)
	if err != nil {
		t.Fatal(err)
	}
	defer listener.Close()

	serverDone := make(chan error, 1)
	go func() {
		conn, err := listener.Accept()
		if err != nil {
			serverDone <- err
			return
		}
		defer conn.Close()

		buf := make([]byte, 1024)
		n, err := conn.Read(buf)
		if err != nil {
			serverDone <- err
			return
		}
		if got := string(buf[:n]); !strings.Contains(got, "fastfetch") {
			serverDone <- io.ErrUnexpectedEOF
			return
		}
		_, err = conn.Write([]byte("CapabilityOS\n"))
		serverDone <- err
	}()

	logPath := filepath.Join(t.TempDir(), "console.log")
	logFile, err := os.Create(logPath)
	if err != nil {
		t.Fatal(err)
	}
	defer logFile.Close()

	sent, matched, err := runSendExpectTTY(socketPath, []string{"fastfetch"}, []string{"CapabilityOS"}, time.Second, logFile)
	if err != nil {
		t.Fatal(err)
	}
	if sent != 1 {
		t.Fatalf("sent = %d, want 1", sent)
	}
	if len(matched) != 1 || matched[0] != "CapabilityOS" {
		t.Fatalf("matched = %#v", matched)
	}
	if err := <-serverDone; err != nil {
		t.Fatal(err)
	}
}
