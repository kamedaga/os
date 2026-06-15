package main

import (
	"os"
	"os/exec"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"
	"time"
)

func envInt(name string, fallback int) int {
	value := os.Getenv(name)
	if value == "" {
		return fallback
	}
	parsed, err := strconv.Atoi(value)
	if err != nil || parsed <= 0 {
		return fallback
	}
	return parsed
}

func touchSparse(buf []byte, stride int, salt byte) uint64 {
	var sum uint64
	for i := 0; i < len(buf); i += stride {
		buf[i] = byte(i) ^ salt
		sum += uint64(buf[i])
	}
	if len(buf) != 0 {
		last := len(buf) - 1
		buf[last] = salt
		sum += uint64(buf[last])
	}
	return sum
}

func worker(id int, rounds int, size int, stride int, hold chan []byte, sums chan uint64, wg *sync.WaitGroup) {
	defer wg.Done()
	var total uint64
	for round := 0; round < rounds; round++ {
		println("GOVM_ROUND_BEGIN", id, round)
		buf := make([]byte, size)
		atomic.AddInt64(&liveBuffers, 1)
		total += touchSparse(buf, stride, byte(id+round))
		if round%2 == 0 {
			hold <- buf
			<-hold
		}
		if round%4 == 3 {
			runtime.GC()
		}
		atomic.AddInt64(&liveBuffers, -1)
		println("GOVM_ROUND_END", id, round)
	}
	sums <- total
}

var liveBuffers int64

func main() {
	if len(os.Args) > 1 && os.Args[1] == "child" {
		childMain()
		return
	}
	if os.Getenv("GO_VM_PROBE_MODE") == "child-churn" {
		childChurnMain()
		return
	}

	workers := envInt("GO_VM_PROBE_WORKERS", runtime.GOMAXPROCS(0)+1)
	rounds := envInt("GO_VM_PROBE_ROUNDS", 16)
	mb := envInt("GO_VM_PROBE_MB", 32)
	stride := envInt("GO_VM_PROBE_STRIDE", 4096)
	if stride < 1 {
		stride = 4096
	}

	size := mb * 1024 * 1024
	hold := make(chan []byte, workers)
	sums := make(chan uint64, workers)
	var wg sync.WaitGroup
	println("GOVM_START", workers, rounds, mb, stride)
	stop := make(chan struct{})
	go func() {
		ticker := time.NewTicker(2 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case <-ticker.C:
				var ms runtime.MemStats
				runtime.ReadMemStats(&ms)
				println("GOVM_TICK", atomic.LoadInt64(&liveBuffers), ms.HeapAlloc, ms.HeapSys, ms.HeapReleased, runtime.NumGoroutine())
			case <-stop:
				return
			}
		}
	}()
	for i := 0; i < workers; i++ {
		wg.Add(1)
		go worker(i, rounds, size, stride, hold, sums, &wg)
	}
	wg.Wait()
	close(stop)
	close(sums)
	var total uint64
	for sum := range sums {
		total += sum
	}
	runtime.GC()
	println("GOVM_OK", total)
}

func childMain() {
	mb := envInt("GO_VM_PROBE_CHILD_MB", envInt("GO_VM_PROBE_MB", 32))
	stride := envInt("GO_VM_PROBE_STRIDE", 4096)
	rounds := envInt("GO_VM_PROBE_CHILD_ROUNDS", 2)
	var total uint64
	for round := 0; round < rounds; round++ {
		buf := make([]byte, mb*1024*1024)
		total += touchSparse(buf, stride, byte(round))
		runtime.GC()
	}
	println("GOVM_CHILD_OK", total)
}

func childChurnMain() {
	workers := envInt("GO_VM_PROBE_WORKERS", 2)
	rounds := envInt("GO_VM_PROBE_ROUNDS", 16)
	childMB := envInt("GO_VM_PROBE_CHILD_MB", envInt("GO_VM_PROBE_MB", 32))
	println("GOVM_CHURN_START", workers, rounds, childMB)
	var wg sync.WaitGroup
	errs := make(chan error, workers*rounds)
	for workerID := 0; workerID < workers; workerID++ {
		wg.Add(1)
		go func(id int) {
			defer wg.Done()
			for round := 0; round < rounds; round++ {
				println("GOVM_CHURN_BEGIN", id, round)
				cmd := exec.Command(os.Args[0], "child")
				cmd.Env = append(os.Environ(),
					"GO_VM_PROBE_CHILD_MB="+strconv.Itoa(childMB),
					"GO_VM_PROBE_CHILD_ROUNDS=2",
				)
				out, err := cmd.CombinedOutput()
				if len(out) != 0 {
					os.Stdout.Write(out)
				}
				if err != nil {
					errs <- err
					println("GOVM_CHURN_ERR", id, round, err.Error())
					return
				}
				println("GOVM_CHURN_END", id, round)
			}
		}(workerID)
	}
	wg.Wait()
	close(errs)
	for err := range errs {
		if err != nil {
			println("GOVM_CHURN_FAIL", err.Error())
			os.Exit(1)
		}
	}
	println("GOVM_CHURN_OK")
}
