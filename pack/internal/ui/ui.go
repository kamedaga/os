package ui

import (
	"fmt"
	"os"
	"strings"
	"sync"
	"time"

	"capabilityos/pack/internal/progress"
)

const (
	reset  = "\x1b[0m"
	bold   = "\x1b[1m"
	dim    = "\x1b[2m"
	red    = "\x1b[31m"
	green  = "\x1b[32m"
	yellow = "\x1b[33m"
	blue   = "\x1b[34m"
	cyan   = "\x1b[36m"
)

var taskCount int

type progressReporter struct{}

type progressBar struct {
	mu          sync.Mutex
	label       string
	total       int64
	current     int64
	message     string
	started     time.Time
	last        time.Time
	stop        chan struct{}
	interactive bool
	closed      bool
}

func Task(name string) {
	taskCount++
	fmt.Printf("\n%s> Task%s %s:%s%s\n", style(blue+bold), style(reset), style(cyan+bold), name, style(reset))
}

func Success(started time.Time, label string) {
	fmt.Printf("\n%sBUILD SUCCESSFUL%s in %s  %s%s%s\n", style(green+bold), style(reset), elapsed(started), style(dim), label, style(reset))
	actionable()
}

func Failed(started time.Time, label string) {
	fmt.Printf("\n%sBUILD FAILED%s in %s  %s%s%s\n", style(red+bold), style(reset), elapsed(started), style(dim), label, style(reset))
	actionable()
}

func Pending(message string) {
	fmt.Printf("  %s!%s %s%s%s\n", style(yellow+bold), style(reset), style(yellow), message, style(reset))
}

func Done(message string) {
	fmt.Printf("  %sOK%s %s\n", style(green+bold), style(reset), message)
}

func NewProgressReporter() progress.Reporter {
	return progressReporter{}
}

func (progressReporter) Start(label string, total int64) progress.Span {
	bar := &progressBar{
		label:       label,
		total:       total,
		started:     time.Now(),
		interactive: progressInteractive(),
		stop:        make(chan struct{}),
	}
	if !bar.interactive {
		fmt.Printf("  ... %s\n", label)
		return bar
	}
	bar.render(true)
	go bar.animate()
	return bar
}

func (bar *progressBar) SetTotal(total int64) {
	bar.mu.Lock()
	defer bar.mu.Unlock()
	bar.total = total
	bar.render(false)
}

func (bar *progressBar) Set(current int64, message string) {
	bar.mu.Lock()
	defer bar.mu.Unlock()
	bar.current = current
	bar.message = message
	bar.render(false)
}

func (bar *progressBar) Add(delta int64, message string) {
	bar.mu.Lock()
	defer bar.mu.Unlock()
	bar.current += delta
	bar.message = message
	bar.render(false)
}

func (bar *progressBar) Message(message string) {
	bar.mu.Lock()
	defer bar.mu.Unlock()
	bar.message = message
	bar.render(false)
}

func (bar *progressBar) Done(message string) {
	bar.finish(message, true)
}

func (bar *progressBar) Fail(message string) {
	bar.finish(message, false)
}

func (bar *progressBar) Close() {
	bar.finish("", true)
}

func (bar *progressBar) finish(message string, ok bool) {
	bar.mu.Lock()
	defer bar.mu.Unlock()
	if bar.closed {
		return
	}
	bar.closed = true
	if !bar.interactive {
		if message != "" {
			prefix := "OK"
			if !ok {
				prefix = "FAILED"
			}
			fmt.Printf("  %s %s\n", prefix, message)
		}
		return
	}
	close(bar.stop)
	fmt.Print("\r\033[K")
}

func (bar *progressBar) animate() {
	ticker := time.NewTicker(120 * time.Millisecond)
	defer ticker.Stop()
	for {
		select {
		case <-ticker.C:
			bar.mu.Lock()
			bar.render(false)
			bar.mu.Unlock()
		case <-bar.stop:
			return
		}
	}
}

func (bar *progressBar) render(force bool) {
	if bar.closed {
		return
	}
	if !bar.interactive {
		return
	}
	now := time.Now()
	if !force && now.Sub(bar.last) < 80*time.Millisecond {
		return
	}
	bar.last = now
	width := int64(24)
	filled := int64(0)
	percent := ""
	if bar.total > 0 {
		if bar.current > bar.total {
			bar.current = bar.total
		}
		filled = bar.current * width / bar.total
		percent = fmt.Sprintf(" %3d%%", bar.current*100/bar.total)
	} else {
		filled = (now.Sub(bar.started).Milliseconds() / 120) % width
		percent = " ..."
	}
	parts := make([]byte, width)
	for i := range parts {
		if bar.total > 0 {
			if int64(i) < filled {
				parts[i] = '='
			} else {
				parts[i] = '-'
			}
		} else if int64(i) == filled {
			parts[i] = '>'
		} else {
			parts[i] = '-'
		}
	}
	message := bar.message
	if message == "" {
		message = bar.label
	}
	elapsedText := elapsed(bar.started)
	fmt.Printf("\r\033[K  %s[%s]%s%s  %s  %s%s%s",
		style(dim), string(parts), percent, style(reset),
		bar.label,
		style(dim), truncate(message, 72)+" "+elapsedText, style(reset),
	)
}

func KeyValues(title string, rows [][2]string) {
	fmt.Printf("%s%s%s\n", style(bold), title, style(reset))
	width := 0
	for _, row := range rows {
		if len(row[0]) > width {
			width = len(row[0])
		}
	}
	for _, row := range rows {
		fmt.Printf("  %s%-*s%s  %s\n", style(dim), width, row[0], style(reset), styleValue(row[1]))
	}
}

func Table(title string, columns []string, rows [][]string) {
	fmt.Printf("%s%s%s\n", style(bold), title, style(reset))
	widths := make([]int, len(columns))
	for i, column := range columns {
		widths[i] = len(column)
	}
	for _, row := range rows {
		for i, value := range row {
			if len(value) > widths[i] {
				widths[i] = len(value)
			}
		}
	}
	printRow(columns, widths, true)
	printRule(widths)
	for _, row := range rows {
		printRow(row, widths, false)
	}
}

func printRow(values []string, widths []int, header bool) {
	parts := make([]string, len(values))
	for i, value := range values {
		cell := fmt.Sprintf("%-*s", widths[i], value)
		if header {
			cell = style(dim+bold) + cell + style(reset)
		} else {
			cell = styleCell(value, cell)
		}
		parts[i] = cell
	}
	fmt.Println("  " + strings.Join(parts, "  "))
}

func printRule(widths []int) {
	parts := make([]string, len(widths))
	for i, width := range widths {
		parts[i] = style(dim) + strings.Repeat("-", width) + style(reset)
	}
	fmt.Println("  " + strings.Join(parts, "  "))
}

func styleCell(value string, padded string) string {
	switch value {
	case "active":
		return style(green) + padded + style(reset)
	case "skip":
		return style(yellow) + padded + style(reset)
	case "zig":
		return style(cyan) + padded + style(reset)
	case "file":
		return style(blue) + padded + style(reset)
	default:
		return padded
	}
}

func styleValue(value string) string {
	switch {
	case value == "active":
		return style(green) + value + style(reset)
	case value == "skip" || value == "-":
		return style(yellow) + value + style(reset)
	case strings.Contains(value, " active"):
		return style(green) + value + style(reset)
	case strings.Contains(value, "skipped"):
		return style(yellow) + value + style(reset)
	default:
		return value
	}
}

func actionable() {
	if taskCount == 0 {
		return
	}
	word := "tasks"
	if taskCount == 1 {
		word = "task"
	}
	fmt.Printf("%s%d actionable %s: %d executed%s\n", style(dim), taskCount, word, taskCount, style(reset))
}

func elapsed(started time.Time) string {
	duration := time.Since(started)
	if duration < time.Second {
		return fmt.Sprintf("%dms", duration.Milliseconds())
	}
	return fmt.Sprintf("%.1fs", duration.Seconds())
}

func progressInteractive() bool {
	if os.Getenv("PACGO_PROGRESS") == "plain" || os.Getenv("NO_COLOR") != "" {
		return false
	}
	if os.Getenv("PACGO_PROGRESS") == "always" {
		return true
	}
	info, err := os.Stdout.Stat()
	return err == nil && info.Mode()&os.ModeCharDevice != 0 && os.Getenv("TERM") != "dumb"
}

func truncate(value string, limit int) string {
	if limit <= 0 || len(value) <= limit {
		return value
	}
	if limit <= 3 {
		return value[:limit]
	}
	return value[:limit-3] + "..."
}

func style(code string) string {
	if !colorEnabled() {
		return ""
	}
	return code
}

func colorEnabled() bool {
	if os.Getenv("NO_COLOR") != "" || os.Getenv("PACGO_COLOR") == "never" {
		return false
	}
	if os.Getenv("PACGO_COLOR") == "always" {
		return true
	}
	return os.Getenv("TERM") != "dumb"
}
