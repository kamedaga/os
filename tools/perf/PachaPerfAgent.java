import java.io.BufferedWriter;
import java.io.FileWriter;
import java.io.IOException;
import java.lang.instrument.Instrumentation;
import java.lang.management.ClassLoadingMXBean;
import java.lang.management.CompilationMXBean;
import java.lang.management.GarbageCollectorMXBean;
import java.lang.management.ManagementFactory;
import java.lang.management.MemoryMXBean;
import java.lang.management.ThreadInfo;
import java.lang.management.ThreadMXBean;
import java.util.List;

/** Low-overhead JVM sampler used by the PachaOS phase-1 performance bench. */
public final class PachaPerfAgent {
    private static String clean(String value) {
        return value.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ');
    }

    private static String frames(ThreadInfo info) {
        StringBuilder value = new StringBuilder(512);
        for (StackTraceElement frame : info.getStackTrace()) {
            if (value.length() != 0) value.append(';');
            value.append(frame.getClassName()).append('.').append(frame.getMethodName());
        }
        return clean(value.toString());
    }

    public static void premain(String argument, Instrumentation instrumentation) {
        final String output = argument == null || argument.isBlank()
                ? "/tmp/pacha-perf-agent.tsv" : argument;
        Thread sampler = new Thread(() -> sample(output), "pacha-perf-sampler");
        sampler.setDaemon(true);
        sampler.setPriority(Thread.MIN_PRIORITY);
        sampler.start();
    }

    private static void sample(String output) {
        ClassLoadingMXBean classes = ManagementFactory.getClassLoadingMXBean();
        CompilationMXBean compiler = ManagementFactory.getCompilationMXBean();
        List<GarbageCollectorMXBean> collectors = ManagementFactory.getGarbageCollectorMXBeans();
        MemoryMXBean memory = ManagementFactory.getMemoryMXBean();
        ThreadMXBean threads = ManagementFactory.getThreadMXBean();
        if (threads.isThreadCpuTimeSupported() && !threads.isThreadCpuTimeEnabled()) {
            try { threads.setThreadCpuTimeEnabled(true); } catch (RuntimeException ignored) { }
        }
        com.sun.management.OperatingSystemMXBean os = null;
        if (ManagementFactory.getOperatingSystemMXBean() instanceof com.sun.management.OperatingSystemMXBean bean) {
            os = bean;
        }
        final long origin = System.nanoTime();
        try (BufferedWriter writer = new BufferedWriter(new FileWriter(output, false), 65536)) {
            writer.write("# SAMPLE mono_ms process_cpu_ns loaded total_loaded unloaded gc_count gc_ms jit_ms heap_used heap_committed threads\n");
            writer.write("# THREAD mono_ms id state cpu_ns user_ns name\n");
            writer.write("# STACK mono_ms id state name frames (every 2 seconds, max depth 24)\n");
            while (true) {
                long monoMs = (System.nanoTime() - origin) / 1_000_000L;
                long gcCount = 0;
                long gcMs = 0;
                for (GarbageCollectorMXBean collector : collectors) {
                    if (collector.getCollectionCount() >= 0) gcCount += collector.getCollectionCount();
                    if (collector.getCollectionTime() >= 0) gcMs += collector.getCollectionTime();
                }
                long jitMs = compiler.isCompilationTimeMonitoringSupported()
                        ? compiler.getTotalCompilationTime() : -1;
                long processCpu = os == null ? -1 : os.getProcessCpuTime();
                long[] ids = threads.getAllThreadIds();
                writer.write("SAMPLE\t" + monoMs + "\t" + processCpu + "\t"
                        + classes.getLoadedClassCount() + "\t" + classes.getTotalLoadedClassCount() + "\t"
                        + classes.getUnloadedClassCount() + "\t" + gcCount + "\t" + gcMs + "\t"
                        + jitMs + "\t" + memory.getHeapMemoryUsage().getUsed() + "\t"
                        + memory.getHeapMemoryUsage().getCommitted() + "\t" + ids.length + "\n");
                boolean stackSample = monoMs / 2000 != (monoMs - 1000) / 2000;
                ThreadInfo[] info = stackSample
                        ? threads.getThreadInfo(ids, 24)
                        : threads.getThreadInfo(ids);
                for (int index = 0; index < ids.length; index++) {
                    if (info[index] == null) continue;
                    long cpu = -1;
                    long user = -1;
                    if (threads.isThreadCpuTimeEnabled()) {
                        try {
                            cpu = threads.getThreadCpuTime(ids[index]);
                            user = threads.getThreadUserTime(ids[index]);
                        } catch (RuntimeException ignored) { }
                    }
                    writer.write("THREAD\t" + monoMs + "\t" + ids[index] + "\t"
                            + info[index].getThreadState() + "\t" + cpu + "\t" + user + "\t"
                            + clean(info[index].getThreadName()) + "\n");
                    if (stackSample) {
                        writer.write("STACK\t" + monoMs + "\t" + ids[index] + "\t"
                                + info[index].getThreadState() + "\t"
                                + clean(info[index].getThreadName()) + "\t"
                                + frames(info[index]) + "\n");
                    }
                }
                writer.flush();
                Thread.sleep(1000);
            }
        } catch (InterruptedException ignored) {
            Thread.currentThread().interrupt();
        } catch (Throwable error) {
            System.err.println("PachaPerfAgent: " + error);
            error.printStackTrace(System.err);
        }
    }
}
