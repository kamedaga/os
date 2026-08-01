package qemu

import (
	"bufio"
	"encoding/json"
	"fmt"
	"net"
	"strings"
	"testing"
	"time"
)

type shortWriteConn struct {
	net.Conn
	maximum int
}

func (conn shortWriteConn) Write(data []byte) (int, error) {
	if len(data) > conn.maximum {
		data = data[:conn.maximum]
	}
	return conn.Conn.Write(data)
}

func TestParseInputSendCheckStreamMetadata(t *testing.T) {
	check, err := parseInputSendCheck("MOUSE_BEGIN@meta:repeat=4,rel:x=1,meta:interval-us=8000,rel:y=-1")
	if err != nil {
		t.Fatal(err)
	}
	if check.Marker != "MOUSE_BEGIN" || len(check.Patterns) != 1 || len(check.Patterns[0]) != 2 {
		t.Fatalf("unexpected check: %#v", check)
	}
	if check.Repeat != 4 || check.Interval != 8*time.Millisecond {
		t.Fatalf("unexpected stream schedule: %#v", check)
	}
	if check.Patterns[0][0].Kind != "rel" || check.Patterns[0][1].Kind != "rel" {
		t.Fatalf("metadata leaked into QMP events: %#v", check.Patterns)
	}
	if len(check.Patterns[0]) != 2 {
		t.Fatalf("metadata was parsed as an event: %#v", check.Patterns[0])
	}
}

func TestParseInputSendCheckAlternatingPatterns(t *testing.T) {
	check, err := parseInputSendCheck("TABLET_BEGIN@meta:repeat=4,meta:interval-us=1000,abs:x=10,abs:y=20;abs:x=30,abs:y=40")
	if err != nil {
		t.Fatal(err)
	}
	if check.Marker != "TABLET_BEGIN" || check.Repeat != 4 || check.Interval != time.Millisecond {
		t.Fatalf("unexpected check: %#v", check)
	}
	if len(check.Patterns) != 2 || len(check.Patterns[0]) != 2 || len(check.Patterns[1]) != 2 {
		t.Fatalf("unexpected patterns: %#v", check.Patterns)
	}
	if check.Patterns[0][0].Value != 10 || check.Patterns[1][0].Value != 30 {
		t.Fatalf("pattern order changed: %#v", check.Patterns)
	}
}

func TestParseInputSendCheckRejectsInvalidStreamMetadata(t *testing.T) {
	tests := []string{
		"M@meta:repeat=0,rel:x=1",
		"M@meta:repeat=100001,rel:x=1",
		"M@meta:repeat=no,rel:x=1",
		"M@meta:interval-us=1000,rel:x=1",
		"M@meta:repeat=2,meta:repeat=3,rel:x=1",
		"M@meta:repeat=2,meta:interval-us=-1,rel:x=1",
		"M@meta:repeat=2,meta:interval-us=30000001,rel:x=1",
		"M@meta:unknown=2,rel:x=1",
		"M@meta:repeat=2,meta:interval-us=1000",
		"M@abs:x=1;abs:x=2",
		"M@meta:repeat=1,abs:x=1;abs:x=2",
		"M@meta:repeat=2,abs:x=1;",
		"M@meta:repeat=2,abs:x=1;meta:interval-us=1000,abs:x=2",
	}
	for _, value := range tests {
		t.Run(value, func(t *testing.T) {
			if _, err := parseInputSendCheck(value); err == nil {
				t.Fatalf("parseInputSendCheck(%q) unexpectedly succeeded", value)
			}
		})
	}
}

func TestInputSendDelayUsesAbsoluteDeadlines(t *testing.T) {
	start := time.Unix(100, 0)
	if got := inputSendDelay(start, 2, 10*time.Millisecond, start.Add(7*time.Millisecond)); got != 13*time.Millisecond {
		t.Fatalf("delay = %s, want 13ms", got)
	}
	if got := inputSendDelay(start, 2, 10*time.Millisecond, start.Add(25*time.Millisecond)); got != 0 {
		t.Fatalf("late delay = %s, want 0", got)
	}
}

func TestValidateInputSendStreamResponseReportsQMPError(t *testing.T) {
	pending := map[string]struct{}{"request-1": {}}
	message := map[string]json.RawMessage{
		"id":    json.RawMessage(`"request-1"`),
		"error": json.RawMessage(`{"class":"GenericError","desc":"rejected"}`),
	}
	err := validateInputSendStreamResponse(message, pending)
	if err == nil || !strings.Contains(err.Error(), "rejected") {
		t.Fatalf("error = %v, want rejected QMP response", err)
	}
	if len(pending) != 0 {
		t.Fatalf("errored response was not drained: %#v", pending)
	}
}

func TestValidateInputSendStreamResponseRejectsDuplicateAndMissingIDs(t *testing.T) {
	pending := map[string]struct{}{"request-1": {}, "request-2": {}}
	success := map[string]json.RawMessage{
		"id":     json.RawMessage(`"request-1"`),
		"return": json.RawMessage(`{}`),
	}
	if err := validateInputSendStreamResponse(success, pending); err != nil {
		t.Fatal(err)
	}
	if err := validateInputSendStreamResponse(success, pending); err == nil || !strings.Contains(err.Error(), "unexpected") {
		t.Fatalf("duplicate response error = %v", err)
	}
	if err := validateInputSendStreamResponse(map[string]json.RawMessage{"return": json.RawMessage(`{}`)}, pending); err == nil || !strings.Contains(err.Error(), "no id") {
		t.Fatalf("missing response id error = %v", err)
	}
	if len(pending) != 1 {
		t.Fatalf("pending responses = %#v", pending)
	}
}

func TestInputSendEventFramesSingleUsesLegacyRequest(t *testing.T) {
	clientConn, serverConn := net.Pipe()
	client := &qmpClient{conn: clientConn, reader: bufio.NewReader(clientConn)}
	serverDone := make(chan error, 1)
	go func() {
		defer serverConn.Close()
		line, err := bufio.NewReader(serverConn).ReadString('\n')
		if err != nil {
			serverDone <- err
			return
		}
		const expected = "{\"arguments\":{\"events\":[{\"data\":{\"axis\":\"x\",\"value\":2},\"type\":\"rel\"}]},\"execute\":\"input-send-event\"}\n"
		if line != expected {
			serverDone <- fmt.Errorf("request changed:\n got %s want %s", line, expected)
			return
		}
		_, err = serverConn.Write([]byte("{\"return\":{}}\n"))
		serverDone <- err
	}()
	if err := client.inputSendEventPatterns([][]inputSendEvent{{{Kind: "rel", Code: "x", Value: 2}}}, 1, time.Millisecond); err != nil {
		t.Fatal(err)
	}
	if err := <-serverDone; err != nil {
		t.Fatal(err)
	}
	client.Close()
}

func TestInputSendEventFramesPipelinesRequests(t *testing.T) {
	clientConn, serverConn := net.Pipe()
	partialConn := shortWriteConn{Conn: clientConn, maximum: 7}
	client := &qmpClient{conn: partialConn, reader: bufio.NewReader(partialConn)}
	serverDone := make(chan error, 1)
	go func() {
		defer serverConn.Close()
		reader := bufio.NewReader(serverConn)
		ids := make([]string, 0, 3)
		for index := 0; index < 3; index++ {
			line, err := reader.ReadBytes('\n')
			if err != nil {
				serverDone <- err
				return
			}
			var request struct {
				Execute string          `json:"execute"`
				ID      string          `json:"id"`
				Args    json.RawMessage `json:"arguments"`
			}
			if err := json.Unmarshal(line, &request); err != nil {
				serverDone <- err
				return
			}
			if request.Execute != "input-send-event" || request.ID == "" || !strings.Contains(string(request.Args), `"axis":"x"`) {
				serverDone <- fmt.Errorf("unexpected request: %s", line)
				return
			}
			ids = append(ids, request.ID)
		}
		// Deliberately wait for the entire stream before replying. A synchronous
		// request/response implementation cannot make it this far.
		if _, err := serverConn.Write([]byte("{\"event\":\"DEVICE_DELETED\",\"data\":{}}\n")); err != nil {
			serverDone <- err
			return
		}
		for index := len(ids) - 1; index >= 0; index-- {
			response, err := json.Marshal(map[string]any{"return": map[string]any{}, "id": ids[index]})
			if err != nil {
				serverDone <- err
				return
			}
			if _, err := serverConn.Write(append(response, '\n')); err != nil {
				serverDone <- err
				return
			}
		}
		serverDone <- nil
	}()

	patterns := [][]inputSendEvent{{{Kind: "rel", Code: "x", Value: 1}}}
	result := make(chan error, 1)
	go func() { result <- client.inputSendEventPatterns(patterns, 3, 0) }()
	select {
	case err := <-result:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(2 * time.Second):
		client.Close()
		t.Fatal("pipelined input stream timed out")
	}
	if err := <-serverDone; err != nil {
		t.Fatal(err)
	}
	client.Close()
}

func TestInputSendEventPatternsAlternatePerReport(t *testing.T) {
	clientConn, serverConn := net.Pipe()
	client := &qmpClient{conn: clientConn, reader: bufio.NewReader(clientConn)}
	serverDone := make(chan error, 1)
	go func() {
		defer serverConn.Close()
		reader := bufio.NewReader(serverConn)
		want := []int{10, 30, 10, 30}
		for index, wantValue := range want {
			line, err := reader.ReadBytes('\n')
			if err != nil {
				serverDone <- err
				return
			}
			var request struct {
				ID        string `json:"id"`
				Arguments struct {
					Events []struct {
						Data struct {
							Value int `json:"value"`
						} `json:"data"`
					} `json:"events"`
				} `json:"arguments"`
			}
			if err := json.Unmarshal(line, &request); err != nil {
				serverDone <- err
				return
			}
			if request.ID == "" || len(request.Arguments.Events) != 2 ||
				request.Arguments.Events[0].Data.Value != wantValue {
				serverDone <- fmt.Errorf("request %d did not use pattern value %d: %s", index, wantValue, line)
				return
			}
			response, _ := json.Marshal(map[string]any{"return": map[string]any{}, "id": request.ID})
			if _, err := serverConn.Write(append(response, '\n')); err != nil {
				serverDone <- err
				return
			}
		}
		serverDone <- nil
	}()

	patterns := [][]inputSendEvent{
		{{Kind: "abs", Code: "x", Value: 10}, {Kind: "abs", Code: "y", Value: 20}},
		{{Kind: "abs", Code: "x", Value: 30}, {Kind: "abs", Code: "y", Value: 40}},
	}
	if err := client.inputSendEventPatterns(patterns, 4, 0); err != nil {
		t.Fatal(err)
	}
	if err := <-serverDone; err != nil {
		t.Fatal(err)
	}
	client.Close()
}

func TestInputSendEventFramesHonorsStreamInterval(t *testing.T) {
	clientConn, serverConn := net.Pipe()
	client := &qmpClient{conn: clientConn, reader: bufio.NewReader(clientConn)}
	serverDone := make(chan error, 1)
	go func() {
		defer serverConn.Close()
		reader := bufio.NewReader(serverConn)
		for index := 0; index < 4; index++ {
			line, err := reader.ReadBytes('\n')
			if err != nil {
				serverDone <- err
				return
			}
			var request struct {
				ID string `json:"id"`
			}
			if err := json.Unmarshal(line, &request); err != nil {
				serverDone <- err
				return
			}
			response, _ := json.Marshal(map[string]any{"return": map[string]any{}, "id": request.ID})
			if _, err := serverConn.Write(append(response, '\n')); err != nil {
				serverDone <- err
				return
			}
		}
		serverDone <- nil
	}()

	start := time.Now()
	patterns := [][]inputSendEvent{{{Kind: "rel", Code: "x", Value: 1}}}
	if err := client.inputSendEventPatterns(patterns, 4, 15*time.Millisecond); err != nil {
		t.Fatal(err)
	}
	elapsed := time.Since(start)
	if elapsed < 35*time.Millisecond || elapsed > 500*time.Millisecond {
		t.Fatalf("stream elapsed = %s, want approximately 45ms", elapsed)
	}
	if err := <-serverDone; err != nil {
		t.Fatal(err)
	}
	client.Close()
}
