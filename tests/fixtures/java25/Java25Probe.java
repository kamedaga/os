import java.util.concurrent.atomic.AtomicInteger;

/** Small executable contracts for HotSpot; run identically on Linux and LPR. */
public final class Java25Probe {
    static final class Box {
        int value = 17;
        int get() { return value; }
    }

    static volatile Object sink;
    static int readField(Box box) { return box.value; }
    static int arrayLength(int[] array) { return array.length; }
    static int arrayElement(int[] array) { return array[0]; }
    static int invoke(Box box) { return box.get(); }
    static int recursive(int depth) { return recursive(depth + 1) + depth; }

    static void check(boolean ok, String what) {
        if (!ok) throw new AssertionError(what);
    }

    static void nullChecks(String which) {
        Box box = new Box();
        int[] array = { 23 };
        long sum = 0;
        // Warm the exact methods later called with null, including C1/C2 paths.
        for (int i = 0; i < 40_000; ++i) {
            sum += readField(box) + arrayLength(array) + arrayElement(array) + invoke(box);
        }
        check(sum == 40_000L * 58, "warmup result");
        System.out.println("JAVA25_NULL_WARMUP=OK");
        if (which.equals("all") || which.equals("null-field")) {
            try {
                sink = readField(null);
                throw new AssertionError("null field did not throw");
            } catch (NullPointerException expected) {
                System.out.println("JAVA25_NULL_FIELD=OK");
            }
        }
        if (which.equals("all") || which.equals("null-array-length")) {
            try {
                sink = arrayLength(null);
                throw new AssertionError("null array length did not throw");
            } catch (NullPointerException expected) {
                System.out.println("JAVA25_NULL_ARRAY_LENGTH=OK");
            }
        }
        if (which.equals("all") || which.equals("null-array-element")) {
            try {
                sink = arrayElement(null);
                throw new AssertionError("null array element did not throw");
            } catch (NullPointerException expected) {
                System.out.println("JAVA25_NULL_ARRAY_ELEMENT=OK");
            }
        }
        if (which.equals("all") || which.equals("null-invoke")) {
            try {
                sink = invoke(null);
                throw new AssertionError("null method did not throw");
            } catch (NullPointerException expected) {
                System.out.println("JAVA25_NULL_INVOKE=OK");
            }
        }
    }

    static void threadChecks() throws InterruptedException {
        AtomicInteger count = new AtomicInteger();
        Thread[] threads = new Thread[4];
        for (int i = 0; i < threads.length; ++i) {
            threads[i] = new Thread(() -> {
                for (int j = 0; j < 10_000; ++j) count.incrementAndGet();
            });
            threads[i].start();
        }
        for (Thread thread : threads) thread.join();
        check(count.get() == 40_000, "threads/atomics/join");
        System.out.println("JAVA25_THREADS=OK");
    }

    static void stackChecks() {
        try {
            sink = recursive(0);
            throw new AssertionError("stack overflow did not throw");
        } catch (StackOverflowError expected) {
            check(readField(new Box()) == 17, "execution after stack overflow");
            System.out.println("JAVA25_STACK_RECOVERY=OK");
        }
    }

    static void gcChecks() {
        long checksum = 0;
        for (int round = 0; round < 8; ++round) {
            byte[][] live = new byte[16][];
            for (int i = 0; i < live.length; ++i) {
                live[i] = new byte[512 * 1024];
                live[i][0] = (byte)(i + round);
            }
            sink = live;
            System.gc();
            for (int i = 0; i < live.length; ++i) {
                check(live[i][0] == i + round, "live object after GC");
                checksum += live[i][0];
            }
        }
        check(checksum == 1408, "GC checksum");
        sink = null;
        System.out.println("JAVA25_GC=OK");
    }

    public static void main(String[] args) throws Exception {
        check(Runtime.version().feature() == 25, "requires Java 25");
        System.out.println("JAVA25_START version=" + Runtime.version());
        String which = args.length == 0 ? "all" : args[0];
        switch (which) {
            case "null" -> nullChecks("all");
            case "null-field", "null-array-length", "null-array-element", "null-invoke" -> nullChecks(which);
            case "threads" -> threadChecks();
            case "stack" -> stackChecks();
            case "gc" -> gcChecks();
            case "all" -> {
                nullChecks("all");
                threadChecks();
                stackChecks();
                gcChecks();
            }
            default -> throw new IllegalArgumentException(which);
        }
        System.out.println("JAVA25_PROBE=PASS case=" + which);
    }
}
