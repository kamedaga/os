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
	request := map[string]any{"execute": name}
	if len(arguments) != 0 {
		request["arguments"] = arguments
	}
	encoded, err := json.Marshal(request)
	if err != nil {
		return err
	}
	encoded = append(encoded, '\n')
	_ = client.conn.SetDeadline(time.Now().Add(5 * time.Second))
	if _, err := client.conn.Write(encoded); err != nil {
		return err
	}
	response, err := client.readResponse()
	if err != nil {
		return err
	}
	if raw, failed := response["error"]; failed {
		return fmt.Errorf("QMP %s failed: %s", name, raw)
	}
	if _, ok := response["return"]; !ok {
		return fmt.Errorf("QMP %s returned no result", name)
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
