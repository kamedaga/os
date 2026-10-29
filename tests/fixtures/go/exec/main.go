package main

import (
	"os/exec"
	"sync"
	"time"
)

func main() {
	println("EXEC-CHECK start")
	var w sync.WaitGroup
	w.Add(1)
	go func() {
		defer w.Done()
		for i := 0; i < 100; i++ {
			time.Sleep(time.Millisecond)
		}
	}()
	for i := 0; i < 4; i++ {
		err := exec.Command("/bin/true").Run()
		if err != nil {
			println(err.Error())
			panic("exec")
		}
	}
	w.Wait()
	println("EXEC-CHECK PASS")
}
