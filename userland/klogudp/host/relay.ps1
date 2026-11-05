param([Parameter(Mandatory=$true)][string]$WslIp, [int]$Port=5000)
# Run on Windows, not inside WSL. Forward UDP without changing packet bytes.
$receiver = [System.Net.Sockets.UdpClient]::new($Port)
$sender = [System.Net.Sockets.UdpClient]::new()
$peer = [System.Net.IPEndPoint]::new([System.Net.IPAddress]::Any, 0)
try {
    while ($true) {
        $bytes = $receiver.Receive([ref]$peer)
        [void]$sender.Send($bytes, $bytes.Length, $WslIp, $Port)
    }
} finally { $receiver.Dispose(); $sender.Dispose() }
