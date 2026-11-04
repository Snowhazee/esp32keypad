Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.IO.Ports

[System.Windows.Forms.Application]::EnableVisualStyles()

$script:serialPort = $null
$script:receiveBuffer = ''
$script:updatingControls = $false
$script:autoDetecting = $false
$script:autoDetectPorts = @()
$script:autoDetectIndex = 0
$script:autoDetectDeadline = [datetime]::MinValue
$script:awaitingConfiguration = $false
$script:lastConfigurationRequest = [datetime]::MinValue
$script:awaitingSave = $false
$script:saveDeadline = [datetime]::MinValue

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
$layout.RowCount = 6
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
    Add-Label "K$($index + 1)" ($index + 3)
    $buttonCombos += Add-Combo ($index + 3)
}

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

$resetProfileButton = New-Object System.Windows.Forms.Button
$resetProfileButton.Text = 'Reset profile'
$resetProfileButton.Width = 105
$resetProfileButton.Height = 30
$resetProfileButton.Enabled = $false
$bottom.Controls.Add($resetProfileButton)

$saveButton = New-Object System.Windows.Forms.Button
$saveButton.Text = 'Save to keypad'
$saveButton.Width = 118
$saveButton.Height = 30
$saveButton.Enabled = $false
$bottom.Controls.Add($saveButton)
$updateButton = New-Object System.Windows.Forms.Button
$updateButton.Text = 'Update firmware...'
$updateButton.Width = 130
$updateButton.Height = 30
$bottom.Controls.Add($updateButton)
$statusLabel = New-Object System.Windows.Forms.Label
$statusLabel.Text = 'Not connected'
$statusLabel.Width = 390
$statusLabel.Height = 24
$bottom.Controls.Add($statusLabel)

$keyOptions = @('None')
foreach ($code in 97..122) {
    $character = [char]$code
    $keyOptions += [string]$character
}
foreach ($code in 48..57) {
    $character = [char]$code
    $keyOptions += [string]$character
}
$keyOptions += 'Space'
$encoderPressOptions = @($keyOptions) + 'Windows key'

foreach ($combo in $buttonCombos) {
    [void]$combo.Items.AddRange([object[]]$keyOptions)
}
[void]$encoderPressCombo.Items.AddRange([object[]]$encoderPressOptions)

for ($index = 0; $index -lt $buttonCombos.Count; $index++) {
    $combo = $buttonCombos[$index]
    $combo.AccessibleName = "Button $($index + 1) key"
    $combo.DropDownStyle = 'DropDownList'
}
$encoderPressCombo.AccessibleName = 'Encoder press key'
$encoderPressCombo.DropDownStyle = 'DropDownList'

function Set-ComboValue($combo, [int]$value) {
    if ($value -ge 65 -and $value -le 90) {
        $value += 32
    }
    if ($value -eq 0) {
        $combo.SelectedIndex = 0
    } elseif ($value -ge 97 -and $value -le 122) {
        $combo.SelectedIndex = $value - 96
    } elseif ($value -ge 48 -and $value -le 57) {
        $combo.SelectedIndex = 27 + $value - 48
    } elseif ($value -eq 32) {
        $combo.SelectedIndex = 37
    } elseif ($value -eq 131 -and $combo.Items.Count -gt 38) {
        $combo.SelectedIndex = 38
    } else {
        $combo.SelectedIndex = 0
    }
}

function Get-ComboValue($combo, [bool]$allowWindowsKey = $false) {
    $index = $combo.SelectedIndex
    if ($index -eq 0) {
        return 0
    }
    if ($index -ge 1 -and $index -le 26) {
        return 96 + $index
    }
    if ($index -ge 27 -and $index -le 36) {
        return 48 + $index - 27
    }
    if ($index -eq 37) {
        return 32
    }
    if ($allowWindowsKey -and $index -eq 38) {
        return 131
    }
    throw "Select a key from the list for '$($combo.AccessibleName)'."
}

function Send-Command([string]$command) {
    if ($null -eq $script:serialPort -or -not $script:serialPort.IsOpen) {
        return
    }
    $script:serialPort.WriteLine($command)
}

function Get-SerialPortCandidates {
    $portNames = @([System.IO.Ports.SerialPort]::GetPortNames() | Sort-Object)
    $usbPortNames = @(
        Get-CimInstance -ClassName Win32_SerialPort -ErrorAction Stop |
            Where-Object { $_.PNPDeviceID -like 'USB\*' -and $portNames -contains $_.DeviceID } |
            ForEach-Object { $_.DeviceID } |
            Sort-Object
    )
    $otherPortNames = @($portNames | Where-Object { $usbPortNames -notcontains $_ })
    return @($usbPortNames) + @($otherPortNames)
}

function Refresh-Ports {
    $selectedPort = [string]$portCombo.SelectedItem
    $portCombo.Items.Clear()
    foreach ($portName in Get-SerialPortCandidates) {
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
          if ($script:serialPort.IsOpen) { $script:serialPort.Close() }
          $script:serialPort.Dispose()
          $script:serialPort = $null
      }
}

function Try-NextAutoDetectPort {
      while ($script:autoDetectIndex -lt $script:autoDetectPorts.Count) {
          $portName = $script:autoDetectPorts[$script:autoDetectIndex]
          $script:autoDetectIndex++
          try {
              $script:serialPort = New-Object System.IO.Ports.SerialPort
              $script:serialPort.PortName = $portName
              $script:serialPort.BaudRate = 115200
              $script:serialPort.DtrEnable = $true
              $script:serialPort.RtsEnable = $true
              $script:serialPort.NewLine = "`n"
              $script:serialPort.Open()
              $portCombo.SelectedItem = $portName
              $script:receiveBuffer = ''
              $script:autoDetectDeadline = [datetime]::Now.AddSeconds(5)
              $script:awaitingConfiguration = $true
              $script:lastConfigurationRequest = [datetime]::Now
              $statusLabel.Text = "Checking $portName for keypad..."
              Send-Command 'GET'
              return
          } catch {
              Close-SerialPort
          }
      }

      $script:autoDetecting = $false
      $portCombo.Enabled = $true
      $connectButton.Enabled = $true
      $scanButton.Enabled = $true
      $statusLabel.Text = 'Keypad not found. Check the USB cable, then select a port and connect.'
}

function Apply-Configuration([string]$line) {
      $parts = $line.Split('|')
      if ($parts.Count -ne 8 -or $parts[0] -ne 'CFG') {
          return $false
      }
      $script:updatingControls = $true
      try {
          $profileCombo.SelectedIndex = [int]$parts[1]
          Set-ComboValue $buttonCombos[0] ([int]$parts[2])
          Set-ComboValue $buttonCombos[1] ([int]$parts[3])
          Set-ComboValue $buttonCombos[2] ([int]$parts[4])
          Set-ComboValue $encoderPressCombo ([int]$parts[7])
          $script:awaitingConfiguration = $false
          $saveButton.Enabled = $true
          $resetProfileButton.Enabled = $true
          $statusLabel.Text = "Connected to $($portCombo.SelectedItem) - Profile $([int]$parts[1] + 1)"
    } finally {
        $script:updatingControls = $false
    }
    return $true
}

function Start-AutoDetect {
    Refresh-Ports
    $script:autoDetectPorts = @(Get-SerialPortCandidates)
    if ($script:autoDetectPorts.Count -eq 0) {
        $statusLabel.Text = 'No serial ports found. Connect the keypad and click Refresh.'
        return
    }

    $script:autoDetecting = $true
    $script:autoDetectIndex = 0
    $portCombo.Enabled = $false
    $connectButton.Enabled = $false
    $scanButton.Enabled = $false
    $statusLabel.Text = 'Searching for keypad...'
    Try-NextAutoDetectPort
}

$scanButton.Add_Click({ Refresh-Ports })
$connectButton.Add_Click({
    if ($null -ne $script:serialPort -and $script:serialPort.IsOpen) {
        Close-SerialPort
        $script:awaitingConfiguration = $false
        $script:awaitingSave = $false
        $saveButton.Enabled = $false
        $resetProfileButton.Enabled = $false
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
        $script:serialPort.DtrEnable = $true
        $script:serialPort.RtsEnable = $true
        $script:serialPort.NewLine = "`n"
        $script:serialPort.Open()
        $connectButton.Text = 'Disconnect'
        $script:receiveBuffer = ''
        $script:awaitingConfiguration = $true
        $script:autoDetectDeadline = [datetime]::Now.AddSeconds(5)
        $script:lastConfigurationRequest = [datetime]::Now
        $saveButton.Enabled = $false
        $resetProfileButton.Enabled = $false
        $statusLabel.Text = "Checking $($portCombo.SelectedItem) for keypad..."
        Send-Command 'GET'
    } catch {
        Close-SerialPort
        $connectButton.Text = 'Connect'
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

    try {
        $buttonValues = @()
        foreach ($combo in $buttonCombos) {
            $buttonValues += Get-ComboValue $combo
        }
        $encoderPressValue = Get-ComboValue $encoderPressCombo $true
    } catch {
        [System.Windows.Forms.MessageBox]::Show($_.Exception.Message, 'Invalid key selection')
        return
    }

    for ($index = 0; $index -lt 3; $index++) {
        Send-Command "SET|$index|$($buttonValues[$index])"
    }
    Send-Command "SET|5|$encoderPressValue"
    Send-Command 'SAVE'
    $script:awaitingSave = $true
    $script:saveDeadline = [datetime]::Now.AddSeconds(2)
    $saveButton.Enabled = $false
    $resetProfileButton.Enabled = $false
    $statusLabel.Text = "Saving to $($portCombo.SelectedItem)..."
})

$resetProfileButton.Add_Click({
    if ($null -eq $script:serialPort -or -not $script:serialPort.IsOpen) {
        [System.Windows.Forms.MessageBox]::Show('Connect to the keypad first.', 'Not connected')
        return
    }

    $profileName = [string]$profileCombo.SelectedItem
    $confirmation = [System.Windows.Forms.MessageBox]::Show(
        "Reset $profileName to K1=Z, K2=X, K3=Y and knob press=Windows key? Other profiles will not change.",
        'Reset current profile',
        [System.Windows.Forms.MessageBoxButtons]::YesNo,
        [System.Windows.Forms.MessageBoxIcon]::Question)
    if ($confirmation -ne [System.Windows.Forms.DialogResult]::Yes) {
        return
    }

    $script:updatingControls = $true
    try {
        Set-ComboValue $buttonCombos[0] 122
        Set-ComboValue $buttonCombos[1] 120
        Set-ComboValue $buttonCombos[2] 121
        Set-ComboValue $encoderPressCombo 131
    } finally {
        $script:updatingControls = $false
    }

    Send-Command 'SET|0|122'
    Send-Command 'SET|1|120'
    Send-Command 'SET|2|121'
    Send-Command 'SET|3|0'
    Send-Command 'SET|4|0'
    Send-Command 'SET|5|131'
    Send-Command 'SAVE'
    $script:awaitingSave = $true
    $script:saveDeadline = [datetime]::Now.AddSeconds(2)
    $saveButton.Enabled = $false
    $resetProfileButton.Enabled = $false
    $statusLabel.Text = "Saving $profileName defaults to $($portCombo.SelectedItem)..."
})

$pollTimer = New-Object System.Windows.Forms.Timer
$pollTimer.Interval = 50
$pollTimer.Add_Tick({
    if ($null -eq $script:serialPort -or -not $script:serialPort.IsOpen) {
        return
    }
    try {
        if ($script:serialPort.BytesToRead -gt 0) {
            $script:receiveBuffer += $script:serialPort.ReadExisting()
            while ($script:receiveBuffer.Contains("`n")) {
                $line, $script:receiveBuffer = $script:receiveBuffer -split "`n", 2
                $configurationReceived = Apply-Configuration $line.Trim()
                if ($configurationReceived -and $script:autoDetecting) {
                    $script:autoDetecting = $false
                    $portCombo.Enabled = $true
                    $connectButton.Enabled = $true
                    $scanButton.Enabled = $true
                    $connectButton.Text = 'Disconnect'
                }
                if ($line.Trim() -eq 'SAVED') {
                    $script:awaitingSave = $false
                    $saveButton.Enabled = $true
                    $resetProfileButton.Enabled = $true
                    $statusLabel.Text = "Settings saved to $($portCombo.SelectedItem)"
                }
            }
        }
        if ($script:autoDetecting -and [datetime]::Now -ge $script:autoDetectDeadline) {
            $script:awaitingConfiguration = $false
            Close-SerialPort
            Try-NextAutoDetectPort
        } elseif ($script:awaitingConfiguration -and [datetime]::Now -ge $script:autoDetectDeadline) {
            $script:awaitingConfiguration = $false
            Close-SerialPort
            $connectButton.Text = 'Connect'
            $saveButton.Enabled = $false
            $resetProfileButton.Enabled = $false
            $statusLabel.Text = "No keypad response on $($portCombo.SelectedItem). Select the ESP32 serial port and connect."
        }
        if ($script:awaitingConfiguration -and
            [datetime]::Now -ge $script:lastConfigurationRequest.AddMilliseconds(300)) {
            Send-Command 'GET'
            $script:lastConfigurationRequest = [datetime]::Now
        }
        if ($script:awaitingSave -and [datetime]::Now -ge $script:saveDeadline) {
            $script:awaitingSave = $false
            $saveButton.Enabled = $true
            $resetProfileButton.Enabled = $true
            $statusLabel.Text = "No save confirmation from $($portCombo.SelectedItem). Check the connection and try again."
        }
    } catch {
        $statusLabel.Text = "Serial read failed: $($_.Exception.Message)"
        $script:awaitingConfiguration = $false
        Close-SerialPort
        $connectButton.Text = 'Connect'
        if ($script:autoDetecting) {
            Try-NextAutoDetectPort
        }
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
    Close-SerialPort
})

Refresh-Ports
$form.Add_Shown({ Start-AutoDetect })
[void]$form.ShowDialog()
