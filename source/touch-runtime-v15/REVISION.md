Current: v13 screen-brightness UEFI, retaining v12 live cold reboot, centered turtle, larger console, USB reconnect and touch lease guards.

Added native DCS51 diagnostic and explicit display configuration-AHB vote before XNU starts. Pixel/byte/PHY rates and panel mode are unchanged. Commands remain blocked while iOS owns touch. Protocol-only brightness lookup is retained for diagnosis; the phone has no installed DisplayPwrCtrl server.

Boot with fastboot boot only. Firmware templates and generated images remain local. See validation/screen-brightness and QUICKSTART.txt.
