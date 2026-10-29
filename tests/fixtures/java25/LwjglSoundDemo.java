import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.IntBuffer;
import org.lwjgl.openal.AL;
import org.lwjgl.openal.ALC;
import org.lwjgl.openal.ALCCapabilities;
import org.lwjgl.system.MemoryUtil;
import static org.lwjgl.openal.AL10.*;
import static org.lwjgl.openal.AL11.AL_SAMPLE_OFFSET;
import static org.lwjgl.openal.ALC10.*;

/** Real playback: a stereo tone, pause/resume, then four queued music buffers. */
public final class LwjglSoundDemo {
    static final int RATE = 48000;
    static void checkAL(String operation) {
        int error = alGetError();
        if (error != AL_NO_ERROR) throw new IllegalStateException(operation + ": AL error " + error);
    }
    static int tone(double left, double right, int frames) {
        ByteBuffer pcm = MemoryUtil.memAlloc(frames * 4).order(ByteOrder.nativeOrder());
        try {
            for (int i = 0; i < frames; ++i) {
                double envelope = Math.min(1, Math.min(i, frames - 1 - i) / 240.0);
                pcm.putShort((short)(3276 * envelope * Math.sin(2 * Math.PI * left * i / RATE)));
                pcm.putShort((short)(3276 * envelope * Math.sin(2 * Math.PI * right * i / RATE)));
            }
            pcm.flip();
            int buffer = alGenBuffers();
            alBufferData(buffer, AL_FORMAT_STEREO16, pcm, RATE);
            checkAL("upload PCM");
            return buffer;
        } finally { MemoryUtil.memFree(pcm); }
    }
    static void awaitStopped(int source) throws InterruptedException {
        long deadline = System.nanoTime() + 8_000_000_000L;
        while (alGetSourcei(source, AL_SOURCE_STATE) != AL_STOPPED) {
            checkAL("playback");
            if (System.nanoTime() >= deadline) throw new AssertionError("playback timeout");
            Thread.sleep(10);
        }
        checkAL("playback stopped");
    }
    public static void main(String[] args) throws Exception {
        long device = alcOpenDevice((ByteBuffer)null);
        if (device == 0) throw new IllegalStateException("No real audio output device");
        long context = 0;
        try {
            ALCCapabilities capabilities = ALC.createCapabilities(device);
            context = alcCreateContext(device, (IntBuffer)null);
            if (context == 0 || !alcMakeContextCurrent(context))
                throw new IllegalStateException("alcCreateContext: " + alcGetError(device));
            AL.createCapabilities(capabilities);
            System.out.println("LWJGL_AUDIO_DEVICE=" + alcGetString(device, ALC_DEVICE_SPECIFIER));
            System.out.println("LWJGL_AUDIO_RENDERER=" + alGetString(AL_RENDERER));
            System.out.println("LWJGL_AUDIO_VERSION=" + alGetString(AL_VERSION));
            System.out.println("LWJGL_AUDIO_RATE=" + alcGetInteger(device, ALC_FREQUENCY));
            int source = alGenSources();
            int buffer = tone(440, 660, RATE);
            try {
                alSourcei(source, AL_BUFFER, buffer);
                alSourcePlay(source); checkAL("play stereo");
                Thread.sleep(200);
                int offset = alGetSourcei(source, AL_SAMPLE_OFFSET);
                if (alGetSourcei(source, AL_SOURCE_STATE) != AL_PLAYING || offset <= 0)
                    throw new AssertionError("PCM clock did not advance: " + offset);
                alSourcePause(source); checkAL("pause");
                if (alGetSourcei(source, AL_SOURCE_STATE) != AL_PAUSED)
                    throw new AssertionError("pause state");
                Thread.sleep(150);
                alSourcePlay(source); awaitStopped(source);
                System.out.println("LWJGL_AUDIO_STATIC=PASS offset=" + offset + " pause-resume=true");
                alSourcei(source, AL_BUFFER, 0);
            } finally { alDeleteBuffers(buffer); }
            int[] buffers = new int[4];
            try {
                for (int i = 0; i < buffers.length; ++i) buffers[i] = tone(550 + i * 110, 550 + i * 110, RATE / 4);
                alSourceQueueBuffers(source, buffers);
                alSourcePlay(source); checkAL("start stream");
                awaitStopped(source);
                int processed = alGetSourcei(source, AL_BUFFERS_PROCESSED);
                if (processed != buffers.length) throw new AssertionError("processed=" + processed);
                for (int expected : buffers) {
                    int actual = alSourceUnqueueBuffers(source);
                    if (actual != expected) throw new AssertionError("queue order");
                }
                checkAL("drain stream");
                System.out.println("LWJGL_AUDIO_STREAM=PASS buffers=" + processed);
            } finally { alDeleteSources(source); for (int id : buffers) if (id != 0) alDeleteBuffers(id); }
            if (alcGetError(device) != ALC_NO_ERROR) throw new AssertionError("ALC error");
            System.out.println("LWJGL_AUDIO=PASS");
        } finally {
            alcMakeContextCurrent(0);
            if (context != 0) alcDestroyContext(context);
            if (!alcCloseDevice(device)) throw new IllegalStateException("alcCloseDevice failed");
        }
    }
}
