import java.nio.ByteBuffer;
import java.util.Locale;
import org.lwjgl.glfw.GLFWErrorCallback;
import org.lwjgl.glfw.GLFWVidMode;
import org.lwjgl.opengl.GL;
import org.lwjgl.system.MemoryStack;
import static org.lwjgl.glfw.Callbacks.glfwFreeCallbacks;
import static org.lwjgl.glfw.GLFW.*;
import static org.lwjgl.opengl.GL33.*;

/** OpenGL 3.3 triangle with real GLFW keyboard and mouse callbacks. */
public final class LwjglWindowDemo {
    static boolean keySeen, mouseSeen;
    static int shader(int kind, String source) {
        int shader = glCreateShader(kind);
        glShaderSource(shader, source);
        glCompileShader(shader);
        if (glGetShaderi(shader, GL_COMPILE_STATUS) != GL_TRUE)
            throw new IllegalStateException(glGetShaderInfoLog(shader));
        return shader;
    }
    static void pixelCheck(int width, int height) {
        try (MemoryStack stack = MemoryStack.stackPush()) {
            ByteBuffer pixel = stack.malloc(4);
            glReadPixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            int r = pixel.get(0) & 255, g = pixel.get(1) & 255, b = pixel.get(2) & 255;
            if (Math.abs(r - 15) > 3 || Math.abs(g - 23) > 3 || Math.abs(b - 41) > 3)
                throw new AssertionError("background readback " + r + "," + g + "," + b);
            glReadPixels(width / 2, height / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
            r = pixel.get(0) & 255; g = pixel.get(1) & 255; b = pixel.get(2) & 255;
            if (r + g + b < 180) throw new AssertionError("triangle readback");
            if (glGetError() != GL_NO_ERROR) throw new AssertionError("OpenGL error");
            System.out.println("LWJGL_FRAMEBUFFER=PASS triangle=" + r + "," + g + "," + b);
        }
    }
    public static void main(String[] args) {
        boolean test = args.length > 0 && args[0].equals("--test");
        GLFWErrorCallback error = GLFWErrorCallback.createPrint(System.err).set();
        long window = 0;
        try {
            glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
            if (!glfwInit()) throw new IllegalStateException("glfwInit failed");
            glfwDefaultWindowHints();
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
            window = glfwCreateWindow(480, 320, "CapabilityOS - Java 25 / LWJGL", 0, 0);
            if (window == 0) throw new IllegalStateException("glfwCreateWindow failed");
            final long handle = window;
            glfwSetKeyCallback(window, (w, key, scan, action, mods) -> {
                if (action == GLFW_PRESS) {
                    System.out.println("LWJGL_KEY_PRESS=" + key);
                    if (key == GLFW_KEY_A) keySeen = true;
                    if (key == GLFW_KEY_ESCAPE) glfwSetWindowShouldClose(w, true);
                }
            });
            glfwSetMouseButtonCallback(window, (w, button, action, mods) -> {
                if (action == GLFW_PRESS && button == GLFW_MOUSE_BUTTON_LEFT) {
                    mouseSeen = true;
                    double[] x = {0}, y = {0}; glfwGetCursorPos(w, x, y);
                    System.out.println("LWJGL_MOUSE_PRESS=LEFT x=" + x[0] + " y=" + y[0]);
                }
            });
            glfwSetFramebufferSizeCallback(window, (w, width, height) -> glViewport(0, 0, width, height));
            glfwMakeContextCurrent(window);
            GL.createCapabilities();
            glfwSwapInterval(1);
            String renderer = glGetString(GL_RENDERER);
            System.out.println("LWJGL_GL_VERSION=" + glGetString(GL_VERSION));
            System.out.println("LWJGL_GL_RENDERER=" + renderer);
            String lower = renderer.toLowerCase(Locale.ROOT);
            if (lower.contains("llvmpipe") || lower.contains("softpipe") || lower.contains("swrast"))
                throw new AssertionError("software rendering: " + renderer);
            int vertex = shader(GL_VERTEX_SHADER, """
                #version 330 core
                layout(location=0) in vec2 position;
                layout(location=1) in vec3 color;
                out vec3 rgb;
                void main() { gl_Position=vec4(position,0.0,1.0); rgb=color; }
                """);
            int fragment = shader(GL_FRAGMENT_SHADER, """
                #version 330 core
                in vec3 rgb;
                out vec4 color;
                void main() { color=vec4(rgb,1.0); }
                """);
            int program = glCreateProgram();
            glAttachShader(program, vertex); glAttachShader(program, fragment); glLinkProgram(program);
            if (glGetProgrami(program, GL_LINK_STATUS) != GL_TRUE)
                throw new IllegalStateException(glGetProgramInfoLog(program));
            glDeleteShader(vertex); glDeleteShader(fragment);
            int vao = glGenVertexArrays(), vbo = glGenBuffers();
            glBindVertexArray(vao); glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, new float[] {
                -.75f,-.65f, 1,.15f,.15f,
                 .75f,-.65f, .15f,1,.15f,
                 0,.75f, .15f,.3f,1
            }, GL_STATIC_DRAW);
            glVertexAttribPointer(0, 2, GL_FLOAT, false, 20, 0L); glEnableVertexAttribArray(0);
            glVertexAttribPointer(1, 3, GL_FLOAT, false, 20, 8L); glEnableVertexAttribArray(1);
            glfwSetWindowPos(window, 100, 100); glfwShowWindow(window); glfwFocusWindow(window);
            GLFWVidMode mode = glfwGetVideoMode(glfwGetPrimaryMonitor());
            int[] wx = {0}, wy = {0}; glfwGetWindowPos(window, wx, wy);
            System.out.println("LWJGL_WINDOW=x=" + wx[0] + " y=" + wy[0] + " width=480 height=320 screen=" + mode.width() + "x" + mode.height());
            int frames = 0; double started = glfwGetTime();
            while (!glfwWindowShouldClose(window)) {
                int[] width = {0}, height = {0}; glfwGetFramebufferSize(window, width, height);
                glViewport(0, 0, width[0], height[0]);
                glClearColor(keySeen ? .2f : .06f, mouseSeen ? .2f : .09f, .16f, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                glUseProgram(program); glDrawArrays(GL_TRIANGLES, 0, 3);
                if (frames == 0) pixelCheck(width[0], height[0]);
                glfwSwapBuffers(window); glfwPollEvents();
                if (frames++ == 0) System.out.println("LWJGL_WINDOW_READY");
                if (keySeen && mouseSeen) glfwSetWindowTitle(handle, "CapabilityOS - keyboard OK / mouse OK");
                if (test && glfwGetTime() - started > 120) throw new AssertionError("input test timeout");
            }
            glDeleteBuffers(vbo); glDeleteVertexArrays(vao); glDeleteProgram(program);
            if (test && (!keySeen || !mouseSeen || frames < 3)) throw new AssertionError("missing input/frames");
            System.out.println("LWJGL_WINDOW=PASS frames=" + frames + " keyboard=" + keySeen + " mouse=" + mouseSeen);
        } finally {
            if (window != 0) { glfwFreeCallbacks(window); glfwDestroyWindow(window); }
            glfwTerminate(); error.free();
        }
    }
}
