package buildsys

import (
	"bufio"
	"fmt"
	"io"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
)

type straceEvent struct {
	PID     int
	Syscall string
	Path    string
	Read    bool
	Exec    bool
	Missing bool
}

var stracePIDPattern = regexp.MustCompile(`^(?:\[pid\s+)?([0-9]+)\]?\s+(.*)$`)
var fdPathPattern = regexp.MustCompile(`<([^<>]+)>`)

// strace pads short calls and resumed calls so the result starts in a fixed
// column: "close(3)        = 0", "<... execve resumed>)     = 0".
var straceResultPattern = regexp.MustCompile(`\)\s+= `)

func parseStrace(reader io.Reader, initialDirectory string) ([]straceEvent, error) {
	scanner := bufio.NewScanner(reader)
	scanner.Buffer(make([]byte, 64*1024), 4*1024*1024)
	pending := make(map[string]string)
	cwds := make(map[int]string)
	var events []straceEvent
	for scanner.Scan() {
		pid, body, ok := splitStracePID(scanner.Text())
		if !ok {
			continue
		}
		if _, exists := cwds[pid]; !exists {
			cwds[pid] = initialDirectory
		}
		if index := strings.Index(body, " <unfinished ...>"); index >= 0 {
			name := body[:strings.IndexByte(body, '(')]
			pending[fmt.Sprintf("%d:%s", pid, name)] = body[:index]
			continue
		}
		if strings.HasPrefix(body, "<...") {
			end := strings.Index(body, " resumed>")
			if end < 0 {
				continue
			}
			name := strings.TrimSpace(strings.TrimPrefix(body[:end], "<..."))
			key := fmt.Sprintf("%d:%s", pid, name)
			prefix, exists := pending[key]
			if !exists {
				continue
			}
			delete(pending, key)
			body = prefix + body[end+len(" resumed>"):]
		}
		event, childPID, cwd, parsed := parseStraceCall(pid, body, cwds[pid])
		if !parsed {
			continue
		}
		if cwd != "" {
			cwds[pid] = cwd
		}
		if childPID > 0 {
			cwds[childPID] = cwds[pid]
		}
		if event.Path != "" {
			events = append(events, event)
		}
	}
	if err := scanner.Err(); err != nil {
		return nil, err
	}
	return events, nil
}

func splitStracePID(line string) (int, string, bool) {
	match := stracePIDPattern.FindStringSubmatch(line)
	if match == nil {
		return 0, "", false
	}
	pid, err := strconv.Atoi(match[1])
	if err != nil {
		return 0, "", false
	}
	return pid, match[2], true
}

func parseStraceCall(pid int, body, cwd string) (straceEvent, int, string, bool) {
	open := strings.IndexByte(body, '(')
	markers := straceResultPattern.FindAllStringIndex(body, -1)
	if open <= 0 || len(markers) == 0 || markers[len(markers)-1][0] < open {
		return straceEvent{}, 0, "", false
	}
	marker := markers[len(markers)-1]
	name := strings.TrimSpace(body[:open])
	arguments := splitStraceArguments(body[open+1 : marker[0]])
	result := strings.TrimSpace(body[marker[1]:])
	success := !strings.HasPrefix(result, "-1 ")
	missing := strings.HasPrefix(result, "-1 ENOENT")
	resolve := func(argumentIndex int, baseIndex int) string {
		if argumentIndex >= len(arguments) {
			return ""
		}
		path, ok := unquoteStraceString(arguments[argumentIndex])
		if !ok {
			return ""
		}
		if filepath.IsAbs(path) {
			return filepath.Clean(path)
		}
		base := cwd
		if baseIndex >= 0 && baseIndex < len(arguments) {
			if annotated := annotatedFDPath(arguments[baseIndex]); annotated != "" {
				base = annotated
			}
		}
		return filepath.Clean(filepath.Join(base, path))
	}
	if success {
		switch name {
		case "chdir":
			return straceEvent{}, 0, resolve(0, -1), true
		case "fchdir":
			if len(arguments) != 0 {
				return straceEvent{}, 0, annotatedFDPath(arguments[0]), true
			}
			return straceEvent{}, 0, "", true
		case "clone", "clone3", "fork", "vfork":
			fields := strings.Fields(result)
			if len(fields) == 0 {
				return straceEvent{}, 0, "", true
			}
			child, _ := strconv.Atoi(fields[0])
			return straceEvent{}, child, "", true
		}
	}
	event := straceEvent{PID: pid, Syscall: name, Missing: missing}
	switch name {
	case "open":
		event.Path = resolve(0, -1)
		event.Read = len(arguments) > 1 && !strings.Contains(arguments[1], "O_WRONLY")
	case "openat":
		event.Path = resolve(1, 0)
		event.Read = len(arguments) > 2 && !strings.Contains(arguments[2], "O_WRONLY")
	case "execve":
		event.Path = resolve(0, -1)
		event.Read = success
		event.Exec = success
	case "stat", "access", "readlink":
		event.Path = resolve(0, -1)
		event.Read = success
	case "newfstatat":
		event.Path = resolve(1, 0)
		event.Read = success
	default:
		return straceEvent{}, 0, "", true
	}
	if success && (name == "open" || name == "openat") {
		if resultPath := annotatedFDPath(result); resultPath != "" {
			event.Path = filepath.Clean(resultPath)
		}
	}
	return event, 0, "", true
}

func splitStraceArguments(value string) []string {
	var arguments []string
	start := 0
	depth := 0
	quoted := false
	escaped := false
	for index, char := range value {
		if escaped {
			escaped = false
			continue
		}
		if quoted && char == '\\' {
			escaped = true
			continue
		}
		if char == '"' {
			quoted = !quoted
			continue
		}
		if quoted {
			continue
		}
		switch char {
		case '(', '[', '{':
			depth++
		case ')', ']', '}':
			depth--
		case ',':
			if depth == 0 {
				arguments = append(arguments, strings.TrimSpace(value[start:index]))
				start = index + 1
			}
		}
	}
	arguments = append(arguments, strings.TrimSpace(value[start:]))
	return arguments
}

func unquoteStraceString(value string) (string, bool) {
	value = strings.TrimSpace(value)
	if !strings.HasPrefix(value, `"`) {
		return "", false
	}
	end := 1
	escaped := false
	for end < len(value) {
		if escaped {
			escaped = false
			end++
			continue
		}
		if value[end] == '\\' {
			escaped = true
			end++
			continue
		}
		if value[end] == '"' {
			decoded, err := strconv.Unquote(value[:end+1])
			return decoded, err == nil
		}
		end++
	}
	return "", false
}

func annotatedFDPath(value string) string {
	match := fdPathPattern.FindStringSubmatch(value)
	if match == nil || !filepath.IsAbs(match[1]) {
		return ""
	}
	return filepath.Clean(match[1])
}
