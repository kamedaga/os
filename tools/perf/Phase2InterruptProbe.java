import java.util.Arrays;

public final class Phase2InterruptProbe {
    private static long percentile(long[] values, int percent) {
        return values[(values.length - 1) * percent / 100];
    }

    public static void main(String[] args) throws Exception {
        final int count = 300;
        long[] samples = new long[count];
        int errors = 0;
        for (int i = 0; i < count; ++i) {
            final boolean[] interrupted = {false};
            Thread sleeper = new Thread(() -> {
                try {
                    Thread.sleep(5000);
                } catch (InterruptedException expected) {
                    interrupted[0] = true;
                }
            }, "phase2-interrupt-sleeper");
            sleeper.start();
            Thread.sleep(1);
            long start = System.nanoTime();
            sleeper.interrupt();
            sleeper.join();
            samples[i] = System.nanoTime() - start;
            if (!interrupted[0] || sleeper.isAlive()) ++errors;
        }
        Arrays.sort(samples);
        long total = 0;
        for (long sample : samples) total += sample;
        System.out.printf(
            "RESULT kind=java_interrupt_join requested_ns=0 count=%d errors=%d " +
            "min_ns=%d p50_ns=%d p95_ns=%d p99_ns=%d max_ns=%d avg_ns=%d%n",
            count, errors, samples[0], percentile(samples, 50),
            percentile(samples, 95), percentile(samples, 99),
            samples[count - 1], total / count);
    }
}
