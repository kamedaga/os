package ui

import (
	"fmt"
	"strings"
	"time"
)

func Task(name string) {
	fmt.Printf("\n> Task :%s\n", name)
}

func Success(started time.Time, label string) {
	fmt.Printf("\nBUILD SUCCESSFUL in %s  %s\n", elapsed(started), label)
}

func Failed(started time.Time, label string) {
	fmt.Printf("\nBUILD FAILED in %s  %s\n", elapsed(started), label)
}

func KeyValues(title string, rows [][2]string) {
	fmt.Println(title)
	width := 0
	for _, row := range rows {
		if len(row[0]) > width {
			width = len(row[0])
		}
	}
	for _, row := range rows {
		fmt.Printf("  %-*s  %s\n", width, row[0], row[1])
	}
}

func Table(title string, columns []string, rows [][]string) {
	fmt.Println(title)
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
	printRow(columns, widths)
	for _, row := range rows {
		printRow(row, widths)
	}
}

func printRow(values []string, widths []int) {
	parts := make([]string, len(values))
	for i, value := range values {
		parts[i] = fmt.Sprintf("%-*s", widths[i], value)
	}
	fmt.Println("  " + strings.Join(parts, "  "))
}

func elapsed(started time.Time) string {
	duration := time.Since(started)
	if duration < time.Second {
		return fmt.Sprintf("%dms", duration.Milliseconds())
	}
	return fmt.Sprintf("%.1fs", duration.Seconds())
}
