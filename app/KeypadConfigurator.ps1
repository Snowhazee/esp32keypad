Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.IO.Ports

[System.Windows.Forms.Application]::EnableVisualStyles()

$script:serialPort = $null
$script:receiveBuffer = ''
$script:updatingControls = $false

$form = New-Object System.Windows.Forms.Form
$form.Text = 'ESP32 Keypad'
$form.StartPosition = 'CenterScreen'
$form.ClientSize = New-Object System.Drawing.Size(580, 470)
$form.MinimumSize = New-Object System.Drawing.Size(596, 509)
$form.Font = New-Object System.Drawing.Font('Segoe UI', 9)

$layout = New-Object System.Windows.Forms.TableLayoutPanel
$layout.Dock = 'Fill'
$layout.Padding = New-Object System.Windows.Forms.Padding(16)
$layout.ColumnCount = 3
$layout.RowCount = 8
$layout.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle('Absolute', 150)))
$layout.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle('Percent', 100)))
$layout.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle('Absolute', 88)))
$form.Controls.Add($layout)

function Add-Label($text, $row) {
    $label = New-Object System.Windows.Forms.Label
    $label.Text = $text
    $label.Dock = 'Fill'
    $label.TextAlign = 'MiddleLeft'
    $layout.Controls.Add($label, 0, $row)
}

function Add-Combo($row) {
    $combo = New-Object System.Windows.Forms.ComboBox
    $combo.Dock = 'Fill'
    $combo.DropDownStyle = 'DropDownList'
    $combo.Margin = New-Object System.Windows.Forms.Padding(3, 6, 12, 6)
    $layout.Controls.Add($combo, 1, $row)
    return $combo
}

$portCombo = Add-Combo 0
$scanButton = New-Object System.Windows.Forms.Button
$scanButton.Text = 'Refresh'
$scanButton.Dock = 'Fill'
$layout.Controls.Add($scanButton, 2, 0)

$profileCombo = Add-Combo 1
$profileCombo.Items.AddRange(@('Profile 1', 'Profile 2', 'Profile 3'))
$profileCombo.SelectedIndex = 0
$connectButton = New-Object System.Windows.Forms.Button
$connectButton.Text = 'Connect'
$connectButton.Dock = 'Fill'
$layout.Controls.Add($connectButton, 2, 1)

$separator = New-Object System.Windows.Forms.Label
$separator.Text = 'KEY MAPPING'
$separator.Font = New-Object System.Drawing.Font('Segoe UI Semibold', 9)
$separator.Dock = 'Fill'
$layout.Controls.Add($separator, 0, 2)
$layout.SetColumnSpan($separator, 3)

$buttonCombos = @()
for ($index = 0; $index -lt 3; $index++) {
    Add-Label "Button $($index + 1)" ($index + 3)
    $buttonCombos += Add-Combo ($index + 3)
}

Add-Label 'Encoder clockwise' 6
$clockwiseCombo = Add-Combo 6
Add-Label 'Encoder counter-clockwise' 7
$counterClockwiseCombo = Add-Combo 7

$bottom = New-Object System.Windows.Forms.FlowLayoutPanel
$bottom.Dock = 'Bottom'
$bottom.Height = 86
$bottom.Padding = New-Object System.Windows.Forms.Padding(16, 8, 16, 8)
$bottom.WrapContents = $true
$form.Controls.Add($bottom)

$encoderPressLabel = New-Object System.Windows.Forms.Label
$encoderPressLabel.Text = 'Encoder press'
$encoderPressLabel.Width = 104
$encoderPressLabel.Height = 30
$encoderPressLabel.TextAlign = 'MiddleLeft'
$bottom.Controls.Add($encoderPressLabel)
$encoderPressCombo = New-Object System.Windows.Forms.ComboBox
$encoderPressCombo.Width = 110
$encoderPressCombo.DropDownStyle = 'DropDownList'
$bottom.Controls.Add($encoderPressCombo)

$saveButton = New-Object System.Windows.Forms.Button
$saveButton.Text = 'Save to keypad'
$saveButton.Width = 118
$saveButton.Height = 30
$bottom.Controls.Add($saveButton)
$updateButton = New-Object System.Windows.Forms.Button
$updateButton.Text = 'Update firmware...'
$updateButton.Width = 130
$updateButton.Height = 30
$bottom.Controls.Add($updateButton)
$statusLabel = New-Object System.Windows.Forms.Label
$statusLabel.Text = 'Not connected'
$statusLabel.Width = 510
$statusLabel.Height = 24
$bottom.Controls.Add($statusLabel)

$keyOptions = @()
$keyOptions += [pscustomobject]@{ Text = 'None'; Value = 0 }
for ($code = 32; $code -le 126; $code++) {
    $character = [char]$code
    $display = if ($code -eq 32) { 'Space' } else { [string]$character }
    $keyOptions += [pscustomobject]@{ Text = $display; Value = $code }
}

$actionOptions = @([pscustomobject]@{ Text = 'Off'; Value = 0 }, [pscustomobject]@{ Text = 'Volume'; Value = -1 })
for ($code = 32; $code -le 126; $code++) {
    $character = [char]$code
    $display = if ($code -eq 32) { 'Key: Space' } else { "Key: $character" }
    $actionOptions += [pscustomobject]@{ Text = $display; Value = $code }
}

foreach ($combo in ($buttonCombos + @($encoderPressCombo))) {
    $combo.DisplayMember = 'Text'
    $combo.ValueMember = 'Value'
    $combo.DataSource = @($keyOptions)
}
foreach ($combo in @($clockwiseCombo, $counterClockwiseCombo)) {
    $combo.DisplayMember = 'Text'
    $combo.ValueMember = 'Value'
    $combo.DataSource = @($actionOptions)
}

function Set-ComboValue($combo, [int]$value) {
    for ($index = 0; $index -lt $combo.Items.Count; $index++) {
        if ($combo.Items[$index].Value -eq $value) {
            $combo.SelectedIndex = $index
            return
        }
    }
    $combo.SelectedIndex = 0
}

function Send-Command([string]$command) {
    if ($null -eq $script:serialPort -or -not $script:serialPort.IsOpen) {
        return
    }
    $script:serialPort.WriteLine($command)
}

function Refresh-Ports {
    $selectedPort = [string]$portCombo.SelectedItem
    $portCombo.Items.Clear()
    foreach ($portName in [System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object) {
        [void]$portCombo.Items.Add($portName)
    }
    if ($portCombo.Items.Contains($selectedPort)) {
        $portCombo.SelectedItem = $selectedPort
    } elseif ($portCombo.Items.Count -gt 0) {
        $portCombo.SelectedIndex = 0
    }
}

function Apply-Configuration([string]$line) {
    $parts = $line.Split('|')
    if ($parts.Count -ne 8 -or $parts[0] -ne 'CFG') {
        return
    }
    $script:updatingControls = $true
    try {
        Set-ComboValue $buttonCombos[0] ([int]$parts[2])
        Set-ComboValue $buttonCombos[1] ([int]$parts[3])
        Set-ComboValue $buttonCombos[2] ([int]$parts[4])
        Set-ComboValue $clockwiseCombo ([int]$parts[5])
        Set-ComboValue $counterClockwiseCombo ([int]$parts[6])
        Set-ComboValue $encoderPressCombo ([int]$parts[7])
        $statusLabel.Text = "Connected to $($portCombo.SelectedItem) - Profile $([int]$parts[1] + 1)"
    } finally {
        $script:updatingControls = $false
    }
}

$scanButton.Add_Click({ Refresh-Ports })
$connectButton.Add_Click({
    if ($null -ne $script:serialPort -and $script:serialPort.IsOpen) {
        $script:serialPort.Close()
        $script:serialPort.Dispose()
        $script:serialPort = $null
        $connectButton.Text = 'Connect'
        $statusLabel.Text = 'Not connected'
        return
    }
    if ($null -eq $portCombo.SelectedItem) {
        [System.Windows.Forms.MessageBox]::Show('Connect the keypad with a USB data cable, then refresh the port list.', 'No serial port')
        return
    }
    try {
        $script:serialPort = New-Object System.IO.Ports.SerialPort
        $script:serialPort.PortName = [string]$portCombo.SelectedItem
        $script:serialPort.BaudRate = 115200
        $script:serialPort.NewLine = "`n"
        $script:serialPort.Open()
        $connectButton.Text = 'Disconnect'
        $statusLabel.Text = 'Connected; reading settings...'
        Send-Command 'GET'
    } catch {
        $statusLabel.Text = 'Connection failed'
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Serial connection failed')
    }
})

$profileCombo.Add_SelectedIndexChanged({
    if (-not $script:updatingControls -and $null -ne $script:serialPort -and $script:serialPort.IsOpen) {
        Send-Command "PROFILE|$($profileCombo.SelectedIndex)"
    }
})

$saveButton.Add_Click({
    if ($null -eq $script:serialPort -or -not $script:serialPort.IsOpen) {
        [System.Windows.Forms.MessageBox]::Show('Connect to the keypad first.', 'Not connected')
        return
    }
    for ($index = 0; $index -lt 3; $index++) {
        Send-Command "SET|$index|$($buttonCombos[$index].SelectedItem.Value)"
    }
    Send-Command "SET|3|$($clockwiseCombo.SelectedItem.Value)"
    Send-Command "SET|4|$($counterClockwiseCombo.SelectedItem.Value)"
    Send-Command "SET|5|$($encoderPressCombo.SelectedItem.Value)"
    Send-Command 'SAVE'
    $statusLabel.Text = 'Settings saved to keypad'
})

$pollTimer = New-Object System.Windows.Forms.Timer
$pollTimer.Interval = 50
$pollTimer.Add_Tick({
    if ($null -eq $script:serialPort -or -not $script:serialPort.IsOpen -or $script:serialPort.BytesToRead -eq 0) {
        return
    }
    try {
        $script:receiveBuffer += $script:serialPort.ReadExisting()
        while ($script:receiveBuffer.Contains("`n")) {
            $line, $script:receiveBuffer = $script:receiveBuffer -split "`n", 2
            Apply-Configuration $line.Trim()
            if ($line.Trim() -eq 'SAVED') {
                $statusLabel.Text = 'Settings saved to keypad'
            }
        }
    } catch {
        $statusLabel.Text = 'Serial read failed'
    }
})
$pollTimer.Start()

$updateButton.Add_Click({
    if ($null -eq $script:serialPort -or -not $script:serialPort.IsOpen) {
        [System.Windows.Forms.MessageBox]::Show('Connect to the keypad first so the app can use its serial port.', 'Not connected')
        return
    }
    $firmwareDialog = New-Object System.Windows.Forms.OpenFileDialog
    $firmwareDialog.Title = 'Select the ESP32-S3 firmware file'
    $firmwareDialog.Filter = 'Firmware binary (*.bin)|*.bin'
    if ($firmwareDialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { return }

    $toolDialog = New-Object System.Windows.Forms.OpenFileDialog
    $toolDialog.Title = 'Select esptool.exe'
    $toolDialog.Filter = 'esptool executable (esptool.exe)|esptool.exe|Executable (*.exe)|*.exe'
    if ($toolDialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { return }

    $portName = [string]$portCombo.SelectedItem
    $script:serialPort.Close()
    $script:serialPort.Dispose()
    $script:serialPort = $null
    $connectButton.Text = 'Connect'
    $pollTimer.Stop()
    $message = "The keypad must be in ESP32-S3 bootloader mode.`n`nHold BOOT, tap RESET, release BOOT, then choose Yes to flash. Keep the USB cable connected."
    $choice = [System.Windows.Forms.MessageBox]::Show($message, 'Ready to flash', 'YesNo', 'Information')
    if ($choice -ne [System.Windows.Forms.DialogResult]::Yes) {
        $pollTimer.Start()
        return
    }
    try {
        $startInfo = New-Object System.Diagnostics.ProcessStartInfo
        $startInfo.FileName = $toolDialog.FileName
        $startInfo.Arguments = "--chip esp32s3 --port $portName --baud 460800 write-flash 0x10000 `"$($firmwareDialog.FileName)`""
        $startInfo.UseShellExecute = $true
        $process = [System.Diagnostics.Process]::Start($startInfo)
        $process.WaitForExit()
        if ($process.ExitCode -eq 0) {
            [System.Windows.Forms.MessageBox]::Show('Firmware flashed. Reset the keypad to start the new version.', 'Update complete')
        } else {
            [System.Windows.Forms.MessageBox]::Show("esptool exited with code $($process.ExitCode). Confirm bootloader mode and try again.", 'Update failed')
        }
    } catch {
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Update failed')
    } finally {
        $pollTimer.Start()
        Refresh-Ports
    }
})

$form.Add_FormClosing({
    $pollTimer.Stop()
    if ($null -ne $script:serialPort) {
        if ($script:serialPort.IsOpen) { $script:serialPort.Close() }
        $script:serialPort.Dispose()
    }
})

Refresh-Ports
[void]$form.ShowDialog()
