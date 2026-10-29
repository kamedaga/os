import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.concurrent.atomic.AtomicLong;

/** A small application: compute primes, join workers, and round-trip a UTF-8 file. */
public final class Java25Demo {
    static boolean prime(int value) {
        if (value < 2) return false;
        for (int d = 2; d <= value / d; d++) {
            if (value % d == 0) return false;
        }
        return true;
    }

    public static void main(String[] args) throws Exception {
        if (Runtime.version().feature() != 25) throw new AssertionError("Java 25 required");
        System.out.println("Hello from CapabilityOS! Java " + Runtime.version());
        AtomicLong sum = new AtomicLong();
        AtomicLong count = new AtomicLong();
        Thread[] workers = new Thread[2];
        for (int i = 0; i < workers.length; i++) {
            int parity = i;
            workers[i] = new Thread(() -> {
                long localSum = 0, localCount = 0;
                for (int n = 2 + parity; n <= 100_000; n += 2) {
                    if (prime(n)) { localSum += n; localCount++; }
                }
                sum.addAndGet(localSum);
                count.addAndGet(localCount);
            }, "prime-worker-" + i);
            workers[i].start();
        }
        for (Thread worker : workers) worker.join();
        if (count.get() != 9592 || sum.get() != 454396537L) {
            throw new AssertionError("incorrect prime result");
        }
        Path directory = Path.of(args.length == 0 ? "/tmp/java25-demo" : args[0]);
        Files.createDirectories(directory);
        Path result = directory.resolve("result.txt");
        String message = "Java 25 on CapabilityOS\n素数: " + count.get() + "\n合計: " + sum.get() + "\n";
        Files.writeString(result, message, StandardCharsets.UTF_8);
        if (!Files.readString(result, StandardCharsets.UTF_8).equals(message)) {
            throw new AssertionError("UTF-8 file round-trip failed");
        }
        System.out.println("Primes: " + count.get() + ", sum: " + sum.get());
        System.out.println("UTF-8 file round-trip: OK");
        System.out.println("Workers: " + workers.length + ", file: " + result);
        System.out.println("JAVA25_DEMO=PASS");
    }
}
