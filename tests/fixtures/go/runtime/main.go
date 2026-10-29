package main

import (
	"runtime"
	"sync"
	"time"
)

func main() {
	println("GO-CHECK: start")
	runtime.GOMAXPROCS(4)
	var w sync.WaitGroup
	for i := 0; i < 4; i++ {
		w.Add(1)
		go func() {
			defer w.Done()
			var n uint64
			for j := 0; j < 1000000; j++ {
				n += uint64(j)
			}
			if n != 499999500000 {
				panic("sum")
			}
			time.Sleep(time.Millisecond)
		}()
	}
	w.Wait()
	println("GO-CHECK: threads, calculation, timer PASS")
}
