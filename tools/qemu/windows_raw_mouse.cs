using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Web.Script.Serialization;
using System.Windows.Forms;

// Input-only diagnostic/bridge. Rendering stays in the existing QEMU window.
public sealed class QemuRawMouse : Form
{
    [StructLayout(LayoutKind.Sequential)]
    struct Device { public ushort page, usage; public uint flags; public IntPtr target; }
    [StructLayout(LayoutKind.Sequential)]
    struct Header { public uint type, size; public IntPtr device, parameter; }
    [DllImport("user32.dll", SetLastError=true)]
    static extern bool RegisterRawInputDevices(Device[] devices, uint count, uint size);
    [DllImport("user32.dll", SetLastError=true)]
    static extern uint GetRawInputData(IntPtr input, uint command, IntPtr data, ref uint size, uint headerSize);

    readonly Label status = new Label();
    readonly Button captureButton = new Button();
    readonly Timer flush = new Timer();
    readonly JavaScriptSerializer json = new JavaScriptSerializer();
    readonly HashSet<string> heldKeys = new HashSet<string>();
    readonly HashSet<string> heldButtons = new HashSet<string>();
    readonly StreamWriter log;
    TcpClient client;
    StreamReader reader;
    StreamWriter writer;
    bool capturing;
    bool cursorHidden;
    int dx, dy;
    long packets, absolutePackets, totalX, totalY;
    readonly string endpoint;
    string lastMessage = "Ready";

    public QemuRawMouse(string endpoint, string logPath)
    {
        this.endpoint = String.IsNullOrWhiteSpace(endpoint) ? null : endpoint;
        log = new StreamWriter(logPath, false); log.AutoFlush = true;
        Text = "QEMU Raw Mouse - " + (this.endpoint == null ? "Probe" : "Input");
        ClientSize = new Size(480, 160); FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false; KeyPreview = true;
        captureButton.Text = "Capture mouse (Esc releases)";
        captureButton.SetBounds(16, 16, 448, 36);
        status.SetBounds(16, 64, 448, 84);
        Controls.Add(captureButton); Controls.Add(status);
        captureButton.Click += delegate { StartCapture(); };
        Deactivate += delegate { StopCapture("Focus lost"); };
        FormClosing += delegate { StopCapture("Closing"); };
        FormClosed += delegate { if (client != null) client.Close(); log.Dispose(); };
        flush.Interval = 16;
        flush.Tick += delegate { FlushMotion(); UpdateStatus(null); };
        flush.Start(); UpdateStatus("Ready");
    }

    void UpdateStatus(string message)
    {
        if (message != null) lastMessage = message;
        status.Text = (capturing ? "Captured; Esc releases" : lastMessage) +
            "\r\nRelative packets: " + packets + "   Absolute ignored: " + absolutePackets +
            "\r\nTotal dx: " + totalX + "   dy: " + totalY +
            "\r\n" + (endpoint ?? "Probe only; nothing sent to QEMU");
    }

    protected override void OnHandleCreated(EventArgs e)
    {
        base.OnHandleCreated(e);
        Device[] devices = { new Device { page=1, usage=2, flags=0, target=Handle } };
        if (!RegisterRawInputDevices(devices, 1, (uint)Marshal.SizeOf(typeof(Device))))
            throw new System.ComponentModel.Win32Exception();
    }

    void Connect()
    {
        if (endpoint == null || client != null) return;
        int colon = endpoint.LastIndexOf(':');
        if (colon < 1) throw new ArgumentException("Expected host:port");
        TcpClient pending = new TcpClient();
        try {
            var attempt = pending.BeginConnect(endpoint.Substring(0, colon), int.Parse(endpoint.Substring(colon+1)), null, null);
            if (!attempt.AsyncWaitHandle.WaitOne(1000)) throw new IOException("Connection timed out");
            pending.EndConnect(attempt); pending.NoDelay = true;
            pending.ReceiveTimeout = 1000; pending.SendTimeout = 500;
            reader = new StreamReader(pending.GetStream());
            writer = new StreamWriter(pending.GetStream()); writer.NewLine = "\r\n"; writer.AutoFlush = true;
            if (reader.ReadLine() == null) throw new IOException("No QMP greeting");
            writer.WriteLine("{\"execute\":\"qmp_capabilities\"}"); ReadReply(reader, json);
            client = pending;
            // QMP may pause while the render thread is busy. Never wait for
            // input acknowledgements on the Windows message-pump thread.
            pending.ReceiveTimeout = 0;
            TcpClient connection = pending;
            StreamReader input = reader;
            var responses = new System.Threading.Thread(delegate() {
                try {
                    var decoder = new JavaScriptSerializer();
                    for (;;) ReadReply(input, decoder);
                } catch (Exception ex) {
                    try {
                        BeginInvoke((Action)delegate {
                            if (client == connection) StopCapture(ex.Message);
                        });
                    } catch (InvalidOperationException) { }
                }
            });
            responses.IsBackground = true;
            responses.Start();
        } catch { pending.Close(); throw; }
    }

    static void ReadReply(StreamReader input, JavaScriptSerializer decoder)
    {
        for (;;) {
            string line = input.ReadLine();
            if (line == null) throw new IOException("QMP disconnected");
            var reply = decoder.Deserialize<Dictionary<string, object>>(line);
            if (reply.ContainsKey("event")) continue;
            if (reply.ContainsKey("error")) throw new IOException(line);
            if (reply.ContainsKey("return")) return;
        }
    }

    void Send(List<object> events)
    {
        if (endpoint == null || events.Count == 0) return;
        writer.WriteLine(json.Serialize(new { execute="input-send-event", arguments=new { events=events } }));
    }
    static object ButtonEvent(string name, bool down) { return new { type="btn", data=new { button=name, down=down } }; }
    static object KeyEvent(string code, bool down) { return new { type="key", data=new { down=down, key=new { type="qcode", data=code } } }; }

    void StartCapture()
    {
        if (capturing) return;
        try {
            Connect(); Activate();
            Rectangle area = RectangleToScreen(ClientRectangle);
            Cursor.Position = new Point(area.Left + area.Width/2, area.Top + area.Height/2);
            Cursor.Clip = area; Cursor.Hide(); cursorHidden = true;
            dx=dy=0; capturing = true; captureButton.Enabled = false;
            log.WriteLine("CAPTURE started"); UpdateStatus(null);
        } catch (Exception ex) { StopCapture(ex.Message); MessageBox.Show(this, ex.Message, "QEMU Raw Mouse"); }
    }

    void StopCapture(string reason)
    {
        bool wasCaptured = capturing;
        capturing = false; dx=dy=0;
        // Release the Windows cursor before any potentially failing I/O.
        Cursor.Clip = Rectangle.Empty;
        if (cursorHidden) { Cursor.Show(); cursorHidden = false; }
        captureButton.Enabled = true;
        if (wasCaptured) {
            try {
                List<object> releases = new List<object>();
                foreach (string key in heldKeys) releases.Add(KeyEvent(key, false));
                foreach (string button in heldButtons) releases.Add(ButtonEvent(button, false));
                Send(releases);
            } catch { if (client != null) client.Close(); client=null; }
            if (client != null) { client.Close(); client=null; }
            heldKeys.Clear(); heldButtons.Clear();
            log.WriteLine("CAPTURE stopped: " + reason);
            log.WriteLine("SUMMARY relative="+packets+" absolute="+absolutePackets+" dx="+totalX+" dy="+totalY);
        }
        UpdateStatus(reason);
    }

    void FlushMotion()
    {
        if (!capturing || (dx==0 && dy==0)) return;
        int x=dx, y=dy; dx=dy=0;
        try {
            var events = new List<object>();
            if (x!=0) events.Add(new { type="rel", data=new { axis="x", value=x } });
            if (y!=0) events.Add(new { type="rel", data=new { axis="y", value=y } });
            Send(events);
        } catch (Exception ex) { StopCapture(ex.Message); }
    }

    protected override void WndProc(ref Message message)
    {
        if (message.Msg == 0x00ff && capturing) {
            uint size=0, header=(uint)Marshal.SizeOf(typeof(Header));
            GetRawInputData(message.LParam, 0x10000003, IntPtr.Zero, ref size, header);
            if (size >= header+24 && size <= 4096) {
                IntPtr data=Marshal.AllocHGlobal((int)size);
                try {
                    if (GetRawInputData(message.LParam, 0x10000003, data, ref size, header) == size && Marshal.ReadInt32(data)==0) {
                        int offset=(int)header;
                        ushort flags=(ushort)Marshal.ReadInt16(data, offset);
                        if ((flags & 1)!=0) absolutePackets++;
                        else {
                            int x=Marshal.ReadInt32(data, offset+12), y=Marshal.ReadInt32(data, offset+16);
                            packets++; dx+=x; dy+=y; totalX+=x; totalY+=y;
                            if (packets<=20) log.WriteLine("REL dx="+x+" dy="+y);
                        }
                        ushort buttons=(ushort)Marshal.ReadInt16(data, offset+4);
                        string[] names={"left","right","middle","side","extra"};
                        var events=new List<object>();
                        for (int i=0;i<names.Length;i++) {
                            if ((buttons & (1<<(i*2)))!=0) { heldButtons.Add(names[i]); events.Add(ButtonEvent(names[i],true)); }
                            if ((buttons & (2<<(i*2)))!=0) { heldButtons.Remove(names[i]); events.Add(ButtonEvent(names[i],false)); }
                        }
                        if ((buttons & 0x400)!=0) {
                            int wheel=Marshal.ReadInt16(data, offset+6);
                            for(int i=0;i<Math.Abs(wheel)/120;i++) { string name=wheel>0?"wheel-up":"wheel-down"; events.Add(ButtonEvent(name,true)); events.Add(ButtonEvent(name,false)); }
                        }
                        if(events.Count!=0) { FlushMotion(); if(capturing) Send(events); }
                    }
                } catch (Exception ex) { StopCapture(ex.Message); }
                finally { Marshal.FreeHGlobal(data); }
            }
        }
        base.WndProc(ref message);
    }

    static string KeyCode(Keys key)
    {
        if(key>=Keys.A && key<=Keys.Z) return key.ToString().ToLowerInvariant();
        if(key>=Keys.D0 && key<=Keys.D9) return ((int)key-(int)Keys.D0).ToString();
        if(key>=Keys.F1 && key<=Keys.F12) return "f"+((int)key-(int)Keys.F1+1);
        switch(key) { case Keys.Space:return "spc"; case Keys.ShiftKey:return "shift"; case Keys.ControlKey:return "ctrl";
            case Keys.Tab:return "tab"; case Keys.Enter:return "ret"; case Keys.Up:return "up"; case Keys.Down:return "down";
            case Keys.Left:return "left"; case Keys.Right:return "right"; default:return null; }
    }
    protected override void OnKeyDown(KeyEventArgs e)
    {
        if(capturing) {
            e.SuppressKeyPress=true;
            try {
                if(e.KeyCode==Keys.Escape) { Send(new List<object>{KeyEvent("esc",true),KeyEvent("esc",false)}); StopCapture("Esc released capture"); }
                else { string code=KeyCode(e.KeyCode); if(code!=null && heldKeys.Add(code)) Send(new List<object>{KeyEvent(code,true)}); }
            } catch(Exception ex) { StopCapture(ex.Message); }
        }
        base.OnKeyDown(e);
    }
    protected override void OnKeyUp(KeyEventArgs e)
    {
        if(capturing) {
            e.SuppressKeyPress=true;
            try { string code=KeyCode(e.KeyCode); if(code!=null && heldKeys.Remove(code)) Send(new List<object>{KeyEvent(code,false)}); }
            catch(Exception ex) { StopCapture(ex.Message); }
        }
        base.OnKeyUp(e);
    }

    [STAThread]
    public static void Run(string endpoint, string logPath)
    {
        Application.EnableVisualStyles();
        using(var window=new QemuRawMouse(endpoint,logPath)) Application.Run(window);
    }
    public static string LayoutTest()
    {
        int header=Marshal.SizeOf(typeof(Header)), device=Marshal.SizeOf(typeof(Device));
        if(header!=(IntPtr.Size==8?24:16) || device!=(IntPtr.Size==8?16:12)) throw new Exception("Raw Input layout mismatch");
        return "PASS RAWINPUTHEADER="+header+" RAWINPUTDEVICE="+device;
    }
}
