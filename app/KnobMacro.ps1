Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.IO.Ports

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class KnobMacroKey {
    [DllImport("user32.dll")]
    public static extern void keybd_event(byte virtualKey, byte scanCode, uint flags, UIntPtr extraInfo);
}
'@

[System.Windows.Forms.Application]::EnableVisualStyles()

$script:serialPort = $null
$script:receiveBuffer = ''
$script:macroEnabled = $true
$script:awaitingKeypad = $false
$script:handshakeDeadline = [datetime]::MinValue
$script:lastHandshakeRequest = [datetime]::MinValue

$form = New-Object System.Windows.Forms.Form
$form.Text = 'Knob Macro'
$form.StartPosition = 'CenterScreen'
$form.ClientSize = New-Object System.Drawing.Size(430, 190)
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.Font = New-Object System.Drawing.Font('Segoe UI', 10)

$portLabel = New-Object System.Windows.Forms.Label
$portLabel.Text = 'Serial port'
$portLabel.Location = New-Object System.Drawing.Point(20, 22)
$portLabel.Size = New-Object System.Drawing.Size(90, 25)
$form.Controls.Add($portLabel)

$portCombo = New-Object System.Windows.Forms.ComboBox
$portCombo.Location = New-Object System.Drawing.Point(110, 19)
$portCombo.Size = New-Object System.Drawing.Size(185, 28)
$portCombo.DropDownStyle = 'DropDownList'
$form.Controls.Add($portCombo)

$refreshButton = New-Object System.Windows.Forms.Button
$refreshButton.Text = 'Refresh'
$refreshButton.Location = New-Object System.Drawing.Point(305, 18)
$refreshButton.Size = New-Object System.Drawing.Size(95, 30)
$form.Controls.Add($refreshButton)

$connectButton = New-Object System.Windows.Forms.Button
$connectButton.Text = 'Connect'
$connectButton.Location = New-Object System.Drawing.Point(110, 62)
$connectButton.Size = New-Object System.Drawing.Size(115, 32)
$form.Controls.Add($connectButton)

$testButton = New-Object System.Windows.Forms.Button
$testButton.Text = 'Test key'
$testButton.Location = New-Object System.Drawing.Point(20, 62)
$testButton.Size = New-Object System.Drawing.Size(80, 32)
$form.Controls.Add($testButton)

$macroCheckBox = New-Object System.Windows.Forms.CheckBox
$macroCheckBox.Text = 'Knob press sends Windows key'
$macroCheckBox.Checked = $true
$macroCheckBox.AutoSize = $true
$macroCheckBox.Location = New-Object System.Drawing.Point(240, 68)
$form.Controls.Add($macroCheckBox)

$statusLabel = New-Object System.Windows.Forms.Label
$statusLabel.Text = 'Close Serial Monitor, select COM port, then Connect.'
$statusLabel.Location = New-Object System.Drawing.Point(20, 116)
$statusLabel.Size = New-Object System.Drawing.Size(385, 45)
$form.Controls.Add($statusLabel)

function Refresh-Ports {
    $selectedPort = [string]$portCombo.SelectedItem
    $portCombo.Items.Clear()
    $portNames = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object)
    $usbPortNames = @(
        Get-CimInstance -ClassName Win32_SerialPort -ErrorAction Stop |
            Where-Object { $_.PNPDeviceID -like 'USB\*' -and $portNames -contains $_.DeviceID } |
            ForEach-Object { $_.DeviceID } |
            Sort-Object
    )
    $portCandidates = @($usbPortNames) + @($portNames | Where-Object { $usbPortNames -notcontains $_ })
    foreach ($portName in $portCandidates) {
        [void]$portCombo.Items.Add($portName)
    }
    if ($portCombo.Items.Contains($selectedPort)) {
        $portCombo.SelectedItem = $selectedPort
    } elseif ($portCombo.Items.Count -gt 0) {
        $portCombo.SelectedIndex = 0
    }
}

function Close-SerialPort {
    if ($null -ne $script:serialPort) {
        if ($script:serialPort.IsOpen) {
            $script:serialPort.Close()
        }
        $script:serialPort.Dispose()
        $script:serialPort = $null
    }
    $script:awaitingKeypad = $false
    $connectButton.Text = 'Connect'
    $portCombo.Enabled = $true
}

function Send-WindowsKey {
    [KnobMacroKey]::keybd_event(0x5B, 0, 0, [UIntPtr]::Zero)
    [KnobMacroKey]::keybd_event(0x5B, 0, 0x0002, [UIntPtr]::Zero)
}

$refreshButton.Add_Click({ Refresh-Ports })
$testButton.Add_Click({
    Send-WindowsKey
    $statusLabel.Text = 'Windows key test sent.'
})

$connectButton.Add_Click({
    if ($null -ne $script:serialPort -and $script:serialPort.IsOpen) {
        Close-SerialPort
        $statusLabel.Text = 'Disconnected.'
        return
    }
    if ($null -eq $portCombo.SelectedItem) {
        $statusLabel.Text = 'No COM port found. Connect the keypad and click Refresh.'
        return
    }

    try {
        $script:serialPort = New-Object System.IO.Ports.SerialPort
        $script:serialPort.PortName = [string]$portCombo.SelectedItem
        $script:serialPort.BaudRate = 115200
        $script:serialPort.NewLine = "`n"
        $script:serialPort.DtrEnable = $false
        $script:serialPort.RtsEnable = $false
        $script:serialPort.Open()
        $script:receiveBuffer = ''
        $script:awaitingKeypad = $true
        $script:handshakeDeadline = [datetime]::Now.AddSeconds(5)
        $script:lastHandshakeRequest = [datetime]::Now
        $connectButton.Text = 'Disconnect'
        $portCombo.Enabled = $false
        $statusLabel.Text = "Checking $($portCombo.SelectedItem) for keypad..."
        $script:serialPort.WriteLine('GET')
    } catch {
        Close-SerialPort
        $statusLabel.Text = "Connection failed: $($_.Exception.Message)"
    }
})

$macroCheckBox.Add_CheckedChanged({
    $script:macroEnabled = $macroCheckBox.Checked
})

$pollTimer = New-Object System.Windows.Forms.Timer
$pollTimer.Interval = 25
$pollTimer.Add_Tick({
    if ($null -eq $script:serialPort -or -not $script:serialPort.IsOpen) {
        return
    }
    try {
        if ($script:serialPort.BytesToRead -gt 0) {
            $script:receiveBuffer += $script:serialPort.ReadExisting()
            while ($script:receiveBuffer.Contains("`n")) {
                $line, $script:receiveBuffer = $script:receiveBuffer -split "`n", 2
                $line = $line.Trim()
                if ($line -match '^CFG\|[0-2]\|' -or $line -like 'BUTTON|ENC_SW(GPIO8)=*') {
                    if ($script:awaitingKeypad) {
                        $script:awaitingKeypad = $false
                        $statusLabel.Text = "Listening on $($portCombo.SelectedItem). Press the knob switch to open Start."
                    }
                }
                if ($line -eq 'BUTTON|ENC_SW(GPIO8)=PRESSED') {
                    if ($script:macroEnabled) {
                        Send-WindowsKey
                        $statusLabel.Text = "Windows key sent at $(Get-Date -Format 'HH:mm:ss')."
                    } else {
                        $statusLabel.Text = 'Knob switch detected; macro is disabled.'
                    }
                }
            }
        }
        if ($script:awaitingKeypad -and
            [datetime]::Now -ge $script:lastHandshakeRequest.AddMilliseconds(300)) {
            $script:serialPort.WriteLine('GET')
            $script:lastHandshakeRequest = [datetime]::Now
        }
        if ($script:awaitingKeypad -and [datetime]::Now -ge $script:handshakeDeadline) {
            $portName = [string]$portCombo.SelectedItem
            Close-SerialPort
            $statusLabel.Text = "No keypad response on $portName. Check the ESP32 port and close Serial Monitor."
        }
    } catch {
        $statusLabel.Text = "Serial read failed: $($_.Exception.Message)"
        Close-SerialPort
    }
})
$pollTimer.Start()

$form.Add_FormClosing({
    $pollTimer.Stop()
    Close-SerialPort
})

Refresh-Ports
[void]$form.ShowDialog()
