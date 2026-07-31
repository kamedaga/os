package qemu

import (
	"bufio"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"os"
	"strconv"
	"strings"
	"sync"
	"time"
)

type screendumpCheck struct {
	Marker    string
	X         int
	Y         int
	Width     int
	Height    int
	Red       uint8
	Green     uint8
	Blue      uint8
	Tolerance uint8
	Path      string
}

func parseScreendumpCheck(value string, index int, artifacts string) (screendumpCheck, error) {
	var check screendumpCheck
	markerAndRegion := strings.SplitN(value, "@", 2)
	if len(markerAndRegion) != 2 || markerAndRegion[0] == "" {
		return check, fmt.Errorf("invalid screendump check %q; expected MARKER@X,Y,W,H=#RRGGBB[:TOLERANCE]", value)
	}
	regionAndColor := strings.SplitN(markerAndRegion[1], "=", 2)
	if len(regionAndColor) != 2 {
		return check, fmt.Errorf("invalid screendump check %q; missing expected color", value)
	}
	coords := strings.Split(regionAndColor[0], ",")
	if len(coords) != 4 {
		return check, fmt.Errorf("invalid screendump region %q", regionAndColor[0])
	}
	parsed := make([]int, 4)
	for i := range coords {
		value, err := strconv.Atoi(coords[i])
		if err != nil || value < 0 {
			return check, fmt.Errorf("invalid screendump coordinate %q", coords[i])
		}
		parsed[i] = value
	}
	if parsed[2] == 0 || parsed[3] == 0 {
		return check, fmt.Errorf("screendump region must have non-zero width and height")
	}
	colorAndTolerance := strings.SplitN(regionAndColor[1], ":", 2)
	color := strings.TrimPrefix(colorAndTolerance[0], "#")
	if len(color) != 6 {
		return check, fmt.Errorf("invalid screendump color %q", colorAndTolerance[0])
	}
	rgb, err := strconv.ParseUint(color, 16, 24)
	if err != nil {
		return check, fmt.Errorf("invalid screendump color %q", colorAndTolerance[0])
	}
	var tolerance uint64
	if len(colorAndTolerance) == 2 {
		tolerance, err = strconv.ParseUint(colorAndTolerance[1], 10, 8)
		if err != nil {
			return check, fmt.Errorf("invalid screendump tolerance %q", colorAndTolerance[1])
		}
	}
	check = screendumpCheck{
		Marker: markerAndRegion[0], X: parsed[0], Y: parsed[1], Width: parsed[2], Height: parsed[3],
		Red: uint8(rgb >> 16), Green: uint8(rgb >> 8), Blue: uint8(rgb), Tolerance: uint8(tolerance),
		Path: fmt.Sprintf("%s/screendump-%02d.ppm", strings.TrimRight(artifacts, "/"), index+1),
	}
	return check, nil
}

type qmpClient struct {
	conn   net.Conn
	reader *bufio.Reader
	mu     sync.Mutex
}

type inputSendEvent struct {
	Kind  string
	Code  string
	Value int
	Down  bool

	// repeat and interval are parser metadata carried by the first real event.
	// They are stripped before constructing QMP input-send-event frames.
	repeat   int
	interval time.Duration
}

const (
	maxInputSendRepeat   = 100_000
	maxInputSendInterval = 60 * time.Second
	maxInputSendDuration = 60 * time.Second
)

type inputSendCheck struct {
	Marker string
	Events []inputSendEvent
}

func parseInputSendCheck(value string) (inputSendCheck, error) {
	var check inputSendCheck
	markerAndEvents := strings.SplitN(value, "@", 2)
	if len(markerAndEvents) != 2 || markerAndEvents[0] == "" || markerAndEvents[1] == "" {
		return check, fmt.Errorf("invalid input send event %q; expected MARKER@[meta:repeat=N,meta:interval-us=N,]key:a=down,rel:x=4,btn:left=up", value)
	}
	check.Marker = markerAndEvents[0]
	repeat := 1
	var interval time.Duration
	var repeatSet, intervalSet bool
	for _, token := range strings.Split(markerAndEvents[1], ",") {
		nameAndValue := strings.SplitN(token, "=", 2)
		kindAndCode := strings.SplitN(nameAndValue[0], ":", 2)
		if len(nameAndValue) != 2 || len(kindAndCode) != 2 || kindAndCode[1] == "" {
			return inputSendCheck{}, fmt.Errorf("invalid input event %q", token)
		}
		if kindAndCode[0] == "meta" {
			parsed, err := strconv.ParseInt(nameAndValue[1], 10, 64)
			if err != nil {
				return inputSendCheck{}, fmt.Errorf("invalid input send metadata %q", token)
			}
			switch kindAndCode[1] {
			case "repeat":
				if repeatSet {
					return inputSendCheck{}, fmt.Errorf("duplicate input send metadata %q", kindAndCode[1])
				}
				if parsed < 1 || parsed > maxInputSendRepeat {
					return inputSendCheck{}, fmt.Errorf("input send repeat must be between 1 and %d", maxInputSendRepeat)
				}
				repeat = int(parsed)
				repeatSet = true
			case "interval-us":
				if intervalSet {
					return inputSendCheck{}, fmt.Errorf("duplicate input send metadata %q", kindAndCode[1])
				}
				if parsed < 0 || parsed > int64(maxInputSendInterval/time.Microsecond) {
					return inputSendCheck{}, fmt.Errorf("input send interval-us must be between 0 and %d", maxInputSendInterval/time.Microsecond)
				}
				interval = time.Duration(parsed) * time.Microsecond
				intervalSet = true
			default:
				return inputSendCheck{}, fmt.Errorf("unsupported input send metadata %q", kindAndCode[1])
			}
			continue
		}
		event := inputSendEvent{Kind: kindAndCode[0], Code: kindAndCode[1]}
		switch event.Kind {
		case "key", "btn":
			if nameAndValue[1] != "down" && nameAndValue[1] != "up" {
				return inputSendCheck{}, fmt.Errorf("invalid %s state %q", event.Kind, nameAndValue[1])
			}
			event.Down = nameAndValue[1] == "down"
		case "rel", "abs":
			parsed, err := strconv.Atoi(nameAndValue[1])
			if err != nil {
				return inputSendCheck{}, fmt.Errorf("invalid %s value %q", event.Kind, nameAndValue[1])
			}
			event.Value = parsed
		default:
			return inputSendCheck{}, fmt.Errorf("unsupported input event kind %q", event.Kind)
		}
		check.Events = append(check.Events, event)
	}
	if len(check.Events) == 0 {
		return inputSendCheck{}, fmt.Errorf("input send event %q contains no events", value)
	}
	if intervalSet && !repeatSet {
		return inputSendCheck{}, fmt.Errorf("input send interval-us requires meta:repeat")
	}
	if time.Duration(repeat)*interval > maxInputSendDuration {
		return inputSendCheck{}, fmt.Errorf("input send stream duration must not exceed %s", maxInputSendDuration)
	}
	if repeatSet {
		check.Events[0].repeat = repeat
		check.Events[0].interval = interval
	}
	return check, nil
}

func connectQMP(socketPath string, timeout time.Duration) (*qmpClient, error) {
	conn, err := net.DialTimeout("unix", socketPath, timeout)
	if err != nil {
		return nil, err
	}
	client := &qmpClient{conn: conn, reader: bufio.NewReader(conn)}
	if _, err := client.readResponse(); err != nil {
		_ = conn.Close()
		return nil, fmt.Errorf("read QMP greeting: %w", err)
	}
	if err := client.execute("qmp_capabilities", nil); err != nil {
		_ = conn.Close()
		return nil, err
	}
	return client, nil
}

func (client *qmpClient) Close() {
	if client != nil && client.conn != nil {
		_ = client.conn.Close()
	}
}

func (client *qmpClient) readResponse() (map[string]json.RawMessage, error) {
	for {
		line, err := client.reader.ReadBytes('\n')
		if err != nil {
			return nil, err
		}
		var message map[string]json.RawMessage
		if err := json.Unmarshal(line, &message); err != nil {
			return nil, err
		}
		if _, event := message["event"]; event {
			continue
		}
		return message, nil
	}
}

func (client *qmpClient) execute(name string, arguments map[string]any) error {
	_, err := client.executeRaw(name, arguments)
	return err
}

func (client *qmpClient) executeRaw(name string, arguments map[string]any) (json.RawMessage, error) {
	client.mu.Lock()
	defer client.mu.Unlock()
	request := map[string]any{"execute": name}
	if len(arguments) != 0 {
		request["arguments"] = arguments
	}
	encoded, err := json.Marshal(request)
	if err != nil {
		return nil, err
	}
	encoded = append(encoded, '\n')
	_ = client.conn.SetDeadline(time.Now().Add(5 * time.Second))
	if _, err := client.conn.Write(encoded); err != nil {
		return nil, err
	}
	response, err := client.readResponse()
	if err != nil {
		return nil, err
	}
	if raw, failed := response["error"]; failed {
		return nil, fmt.Errorf("QMP %s failed: %s", name, raw)
	}
	result, ok := response["return"]
	if !ok {
		return nil, fmt.Errorf("QMP %s returned no result", name)
	}
	return result, nil
}

type qmpCPUThread struct {
	CPUIndex int `json:"cpu-index"`
	ThreadID int `json:"thread-id"`
}

func (client *qmpClient) queryCPUThreads() ([]qmpCPUThread, error) {
	raw, err := client.executeRaw("query-cpus-fast", nil)
	if err != nil {
		return nil, err
	}
	var threads []qmpCPUThread
	if err := json.Unmarshal(raw, &threads); err != nil {
		return nil, fmt.Errorf("decode QMP CPU threads: %w", err)
	}
	return threads, nil
}

func inputSendEventArguments(events []inputSendEvent) (map[string]any, error) {
	qmpEvents := make([]any, 0, len(events))
	for _, event := range events {
		data := map[string]any{}
		switch event.Kind {
		case "key":
			data = map[string]any{"down": event.Down, "key": map[string]any{"type": "qcode", "data": event.Code}}
		case "btn":
			data = map[string]any{"down": event.Down, "button": event.Code}
		case "rel", "abs":
			data = map[string]any{"axis": event.Code, "value": event.Value}
		default:
			return nil, fmt.Errorf("unsupported input event kind %q", event.Kind)
		}
		qmpEvents = append(qmpEvents, map[string]any{"type": event.Kind, "data": data})
	}
	return map[string]any{"events": qmpEvents}, nil
}

func (client *qmpClient) inputSendEvents(events []inputSendEvent) error {
	arguments, err := inputSendEventArguments(events)
	if err != nil {
		return err
	}
	return client.execute("input-send-event", arguments)
}

func inputSendEventClass(event inputSendEvent) (string, error) {
	switch event.Kind {
	case "key":
		return "keyboard", nil
	case "btn", "rel", "abs":
		return "pointer", nil
	default:
		return "", fmt.Errorf("unsupported input event kind %q", event.Kind)
	}
}

func splitInputSendEventFrames(events []inputSendEvent) ([][]inputSendEvent, error) {
	var frames [][]inputSendEvent
	var currentClass string
	for _, event := range events {
		eventClass, err := inputSendEventClass(event)
		if err != nil {
			return nil, err
		}
		if len(frames) == 0 || eventClass != currentClass {
			frames = append(frames, nil)
			currentClass = eventClass
		}
		frames[len(frames)-1] = append(frames[len(frames)-1], event)
	}
	return frames, nil
}

func inputSendEventSchedule(events []inputSendEvent) (int, time.Duration, []inputSendEvent, error) {
	if len(events) == 0 {
		return 0, 0, nil, fmt.Errorf("input event sequence is empty")
	}
	repeat := events[0].repeat
	interval := events[0].interval
	if repeat == 0 {
		repeat = 1
	}
	if repeat < 1 || repeat > maxInputSendRepeat {
		return 0, 0, nil, fmt.Errorf("input send repeat must be between 1 and %d", maxInputSendRepeat)
	}
	if interval < 0 || interval > maxInputSendInterval {
		return 0, 0, nil, fmt.Errorf("input send interval must be between 0 and %s", maxInputSendInterval)
	}
	if time.Duration(repeat)*interval > maxInputSendDuration {
		return 0, 0, nil, fmt.Errorf("input send stream duration must not exceed %s", maxInputSendDuration)
	}
	clean := append([]inputSendEvent(nil), events...)
	for i := range clean {
		clean[i].repeat = 0
		clean[i].interval = 0
	}
	return repeat, interval, clean, nil
}

const inputSendStreamWindow = 128

type inputSendStreamResponse struct {
	message map[string]json.RawMessage
	err     error
}

func inputSendDelay(start time.Time, iteration int, interval time.Duration, now time.Time) time.Duration {
	delay := start.Add(time.Duration(iteration) * interval).Sub(now)
	if delay < 0 {
		return 0
	}
	return delay
}

func validateInputSendStreamResponse(message map[string]json.RawMessage, pending map[string]struct{}) error {
	rawID, ok := message["id"]
	if !ok {
		return fmt.Errorf("QMP input-send-event response has no id")
	}
	var id string
	if err := json.Unmarshal(rawID, &id); err != nil {
		return fmt.Errorf("decode QMP input-send-event response id: %w", err)
	}
	if _, ok := pending[id]; !ok {
		return fmt.Errorf("unexpected QMP input-send-event response id %q", id)
	}
	delete(pending, id)
	if raw, failed := message["error"]; failed {
		return fmt.Errorf("QMP input-send-event failed: %s", raw)
	}
	if _, ok := message["return"]; !ok {
		return fmt.Errorf("QMP input-send-event returned no result")
	}
	return nil
}

func (client *qmpClient) writeInputSendStreamRequest(id string, events []inputSendEvent) error {
	arguments, err := inputSendEventArguments(events)
	if err != nil {
		return err
	}
	request := map[string]any{"execute": "input-send-event", "arguments": arguments, "id": id}
	encoded, err := json.Marshal(request)
	if err != nil {
		return err
	}
	encoded = append(encoded, '\n')
	_ = client.conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	for len(encoded) != 0 {
		written, writeErr := client.conn.Write(encoded)
		if writeErr != nil {
			return writeErr
		}
		if written == 0 {
			return io.ErrUnexpectedEOF
		}
		encoded = encoded[written:]
	}
	return nil
}

func (client *qmpClient) inputSendEventFrames(events []inputSendEvent) error {
	repeat, interval, cleanEvents, err := inputSendEventSchedule(events)
	if err != nil {
		return err
	}
	frames, err := splitInputSendEventFrames(cleanEvents)
	if err != nil {
		return err
	}
	if repeat == 1 {
		for _, frame := range frames {
			if err := client.inputSendEvents(frame); err != nil {
				return err
			}
		}
		return nil
	}
	if len(frames) > int(^uint(0)>>1)/repeat {
		return fmt.Errorf("too many input-send-event requests")
	}
	totalRequests := repeat * len(frames)

	// Keep the normal execute path out of the stream until every response has
	// been drained. QMP command IDs let the pipelined responses be validated.
	client.mu.Lock()
	defer client.mu.Unlock()
	_ = client.conn.SetDeadline(time.Time{})
	defer client.conn.SetDeadline(time.Time{})

	responses := make(chan inputSendStreamResponse, inputSendStreamWindow)
	readerDone := make(chan struct{})
	go func() {
		defer close(readerDone)
		for index := 0; index < totalRequests; index++ {
			_ = client.conn.SetReadDeadline(time.Now().Add(5 * time.Second))
			message, readErr := client.readResponse()
			responses <- inputSendStreamResponse{message: message, err: readErr}
			if readErr != nil {
				return
			}
		}
	}()

	pending := make(map[string]struct{}, inputSendStreamWindow)
	sent := 0
	received := 0
	var firstResponseErr error
	handleResponse := func(response inputSendStreamResponse) error {
		if response.err != nil {
			return response.err
		}
		received++
		if responseErr := validateInputSendStreamResponse(response.message, pending); responseErr != nil && firstResponseErr == nil {
			firstResponseErr = responseErr
		}
		return nil
	}
	drainAvailable := func() error {
		for {
			select {
			case response := <-responses:
				if err := handleResponse(response); err != nil {
					return err
				}
			default:
				return nil
			}
		}
	}
	abortReader := func() {
		_ = client.conn.Close()
		for {
			select {
			case <-responses:
			case <-readerDone:
				return
			}
		}
	}

	start := time.Now()
	for iteration := 0; iteration < repeat; iteration++ {
		if iteration > 0 && interval > 0 {
			if delay := inputSendDelay(start, iteration, interval, time.Now()); delay > 0 {
				timer := time.NewTimer(delay)
			waitForDeadline:
				for {
					select {
					case response := <-responses:
						if err := handleResponse(response); err != nil {
							timer.Stop()
							abortReader()
							return fmt.Errorf("read QMP input-send-event response: %w", err)
						}
					case <-timer.C:
						break waitForDeadline
					}
				}
			}
		}
		if err := drainAvailable(); err != nil {
			abortReader()
			return fmt.Errorf("read QMP input-send-event response: %w", err)
		}
		for _, frame := range frames {
			for sent-received >= inputSendStreamWindow {
				if err := handleResponse(<-responses); err != nil {
					abortReader()
					return fmt.Errorf("read QMP input-send-event response: %w", err)
				}
			}
			id := fmt.Sprintf("pacgo-input-stream-%d", sent)
			pending[id] = struct{}{}
			if err := client.writeInputSendStreamRequest(id, frame); err != nil {
				delete(pending, id)
				abortReader()
				return fmt.Errorf("write QMP input-send-event request: %w", err)
			}
			sent++
		}
	}
	for received < sent {
		if err := handleResponse(<-responses); err != nil {
			abortReader()
			return fmt.Errorf("read QMP input-send-event response: %w", err)
		}
	}
	<-readerDone
	if firstResponseErr != nil {
		return firstResponseErr
	}
	if len(pending) != 0 {
		return fmt.Errorf("QMP input-send-event left %d responses pending", len(pending))
	}
	return nil
}

func (client *qmpClient) screendump(device string, check screendumpCheck) error {
	_ = os.Remove(check.Path)
	arguments := map[string]any{"filename": check.Path, "format": "ppm"}
	if device != "" {
		arguments["device"] = device
	}
	if err := client.execute("screendump", arguments); err != nil {
		return err
	}
	return validatePPMRegion(check)
}

func ppmToken(reader *bufio.Reader) (string, error) {
	for {
		b, err := reader.ReadByte()
		if err != nil {
			return "", err
		}
		if b == '#' {
			if _, err := reader.ReadString('\n'); err != nil {
				return "", err
			}
			continue
		}
		if b == ' ' || b == '\n' || b == '\r' || b == '\t' {
			continue
		}
		var token strings.Builder
		token.WriteByte(b)
		for {
			b, err = reader.ReadByte()
			if err != nil {
				return "", err
			}
			if b == ' ' || b == '\n' || b == '\r' || b == '\t' {
				return token.String(), nil
			}
			token.WriteByte(b)
		}
	}
}

func validatePPMRegion(check screendumpCheck) error {
	file, err := os.Open(check.Path)
	if err != nil {
		return err
	}
	defer file.Close()
	reader := bufio.NewReader(file)
	magic, err := ppmToken(reader)
	if err != nil || magic != "P6" {
		return fmt.Errorf("invalid screendump PPM header in %s", check.Path)
	}
	widthText, err := ppmToken(reader)
	if err != nil {
		return err
	}
	heightText, err := ppmToken(reader)
	if err != nil {
		return err
	}
	maxText, err := ppmToken(reader)
	if err != nil {
		return err
	}
	width, widthErr := strconv.Atoi(widthText)
	height, heightErr := strconv.Atoi(heightText)
	maximum, maxErr := strconv.Atoi(maxText)
	if widthErr != nil || heightErr != nil || maxErr != nil || width <= 0 || height <= 0 || maximum != 255 {
		return fmt.Errorf("unsupported screendump PPM geometry %sx%s max=%s", widthText, heightText, maxText)
	}
	if check.X+check.Width > width || check.Y+check.Height > height {
		return fmt.Errorf("screendump region %d,%d %dx%d exceeds %dx%d surface", check.X, check.Y, check.Width, check.Height, width, height)
	}
	pixels := make([]byte, width*height*3)
	if _, err := io.ReadFull(reader, pixels); err != nil {
		return err
	}
	within := func(actual, expected uint8) bool {
		delta := int(actual) - int(expected)
		if delta < 0 {
			delta = -delta
		}
		return delta <= int(check.Tolerance)
	}
	for y := check.Y; y < check.Y+check.Height; y++ {
		for x := check.X; x < check.X+check.Width; x++ {
			offset := (y*width + x) * 3
			if !within(pixels[offset], check.Red) || !within(pixels[offset+1], check.Green) || !within(pixels[offset+2], check.Blue) {
				return fmt.Errorf("screendump %s pixel %d,%d is #%02x%02x%02x; expected #%02x%02x%02x tolerance=%d", check.Marker, x, y, pixels[offset], pixels[offset+1], pixels[offset+2], check.Red, check.Green, check.Blue, check.Tolerance)
			}
		}
	}
	return nil
}
