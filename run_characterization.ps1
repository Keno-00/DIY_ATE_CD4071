param(
    [Parameter(Mandatory=$true)]
    [int]$Batch,
    
    [Parameter(Mandatory=$true)]
    [int]$Sample,
    
    [string]$PortName = "COM5"
)

$baudRate = 115200
$desktopPath = "C:\Users\Keno\Desktop\CD4071_ATE_Results"
$outputFile = "$desktopPath\Batch_$Batch\Sample_$Sample.csv"

# Validate directories
if (-not (Test-Path "$desktopPath\Batch_$Batch")) {
    Write-Error "Batch directory Batch_$Batch does not exist."
    exit 1
}

Write-Host "`nConnecting to $PortName at $baudRate baud..." -ForegroundColor Cyan
$port = New-Object System.IO.Ports.SerialPort $PortName, $baudRate, None, 8, one
$port.ReadTimeout = 5000
$port.WriteTimeout = 5000

try {
    $port.Open()
    Write-Host "Waiting 3 seconds for Arduino Mega boot..." -ForegroundColor Yellow
    Start-Sleep -Seconds 3
    
    # Clear boot buffer
    $port.ReadExisting() | Out-Null
    
    Write-Host "Sending characterization command: 'test ic $Batch $Sample'..." -ForegroundColor Green
    $port.WriteLine("test ic $Batch $Sample")
    
    # 90 second timeout for 4 gates
    $timeout = [DateTime]::Now.AddSeconds(90)
    $csvData = [System.Collections.Generic.List[string]]::new()
    $captureStarted = $false
    
    while ([DateTime]::Now -lt $timeout) {
        if ($port.BytesToRead -gt 0) {
            $line = $port.ReadLine().Trim()
            
            # Print serial trace to screen
            if ($line.Length -gt 0) {
                Write-Host "  $line"
            }
            
            # Check for CSV Header or Data
            if ($line -like "*Batch,IC_Sample,Gate*") {
                $captureStarted = $true
                $csvData.Clear()
                $csvData.Add($line)
            }
            elseif ($captureStarted -and ($line -split ',').Length -eq 9) {
                $csvData.Add($line)
            }
            
            # Stop condition
            if ($line -like "*=========================================*" -and $captureStarted -and $csvData.Count -gt 1) {
                break
            }
        }
        Start-Sleep -Milliseconds 10
    }
    
    if ($csvData.Count -gt 1) {
        Write-Host "`nSweep complete! Writing data to $outputFile..." -ForegroundColor Green
        $csvData | Out-File -FilePath $outputFile -Encoding utf8 -Force
        Write-Host "CSV data successfully saved to Desktop!" -ForegroundColor Cyan
    } else {
        Write-Warning "Could not capture complete CSV data from the device."
    }
}
catch {
    Write-Error $_.Exception.Message
}
finally {
    if ($port.IsOpen) {
        $port.Close()
        Write-Host "Port closed." -ForegroundColor Yellow
    }
}
