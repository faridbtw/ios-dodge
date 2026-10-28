v15: same loader GPIO path as v14; USB idle auto-reboot disabled. New XNU protocol supports bounded24..32-byte identify (OP13 observed28).


25 October 2026: current stable-overlay-v11 profile adds centered turtle artwork,
larger full-screen live logs/progress, cached text-surface composition and USB
reconnect recovery. Transport/polling matches the twice cable-tested v8 profile.
Build and source notices are preserved; see validation/uefi/ and QUICKSTART.txt.

28 October 2026: live-reset-v12 retains the v11 turtle/font/log/reconnect
implementation and dispatches exact Dodge reboot/oem reboot to EFI cold reset
before active-touch guards. It does not clean up or reuse GPU-owned memory.
192 sanitizer dispatch cases passed. Live seven-core GPU iOS reset to Android
through fastboot reboot; Android startup completed in22.7seconds. Validation:
validation/uefi/live-reset/STATUS.json. This source change requires loading the
new RAM UEFI; it cannot patch an already running older loader.
