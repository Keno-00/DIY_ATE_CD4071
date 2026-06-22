# DIY ATE Firmware

This directory contains the firmware for the DIY ATE controller running on the Arduino Mega 2560.

## Structure

- `diy_ate_controller/`: Arduino sketch directory containing `diy_ate_controller.ino`.

## Prerequisite: Arduino CLI
The command line tool `arduino-cli` must be installed. It is located at:
`C:\Program Files\Arduino CLI\arduino-cli.exe`

If it is not in your system's PATH, you can run it using the absolute path.

## Setup AVR Core
To install AVR platform support for the Arduino Mega:
```powershell
& "C:\Program Files\Arduino CLI\arduino-cli.exe" core update-index
& "C:\Program Files\Arduino CLI\arduino-cli.exe" core install arduino:avr
```

## Compilation
To compile the sketch:
```powershell
& "C:\Program Files\Arduino CLI\arduino-cli.exe" compile --fqbn arduino:avr:mega diy_ate_controller
```

## Uploading
To upload the compiled sketch to your Arduino Mega, locate its COM port (e.g. `COM3`) and run:
```powershell
& "C:\Program Files\Arduino CLI\arduino-cli.exe" upload -p COM3 --fqbn arduino:avr:mega diy_ate_controller
```

To list connected boards and detect the COM port automatically:
```powershell
& "C:\Program Files\Arduino CLI\arduino-cli.exe" board list
```
