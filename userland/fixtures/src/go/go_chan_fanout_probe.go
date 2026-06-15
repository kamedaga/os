package main

import "runtime"

const workers = 15

func worker(index int, sem chan struct{}, done chan int) {
	defer func() {
		<-sem
		close(done)
	}()

	buf := make([]byte, 16*1024)
	for i := range buf {
		buf[i] = byte(index + i)
	}
	if buf[0] == 255 {
		done <- index
	}
}

func main() {
	sem := make(chan struct{}, runtime.GOMAXPROCS(0)+10)
	done := make([]chan int, workers)
	for i := range done {
		done[i] = make(chan int)
	}

	go func() {
		for i := range done {
			sem <- struct{}{}
			go worker(i, sem, done[i])
		}
	}()

	for i := range done {
		for range done[i] {
		}
	}
	println("GOCHAN_OK")
}
