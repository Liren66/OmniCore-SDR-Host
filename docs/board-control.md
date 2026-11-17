# RGB, Temperature-Controlled Fan, and Zynq Temperature

The board SDK enables the RGB status indicator by default, at 15% channel intensity (at most 38/255 per channel); automatic fan control is off by default with PWM at 0%. These switch settings persist while the current program runs on the board: closing/reopening OCH does not reset them, and `rf_reinit!` keeps them too. They return to defaults after the board program restarts.

## Command line

| Function | OCH command | SDK command |
|---|---|---|
| Turn RGB on/off | `och set rgb.enabled on` / `off` | `rgb_enable=1` / `0` |
| Query RGB switch | `och get rgb.enabled` | `rgb_enable?` |
| Query RGB state | `och get rgb.status` | `rgb_status?` |
| Turn automatic fan control on/off | `och set fan.enabled on` / `off` | `fan_enable=1` / `0` |
| Query fan control switch | `och get fan.enabled` | `fan_enable?` |
| Query fan state | `och get fan.status` | `fan_status?` |
| Core temperature | `och get core.temperature` | `core_temp?` |

Switches also accept `0/1`. All commands support `--json`. `och get system.status` includes RGB, fan, Zynq temperature, and the existing AD9361 temperature, with the temperature lines labeled `source=zynq` and `source=ad9361`. The standalone core temperature query additionally shows degrees Celsius (`Celsius`) in the terminal, and the fan state additionally shows the duty cycle as a percentage (`duty_pct`).

`och get system.info` should include `board_rgb=1 fan_control=1 core_temp=1`. The canonical commands and the C/C++ interfaces check the capability first and validate the response; older firmware without the capability returns unsupported. OCH switch and status queries do not stop IQ streams and do not change the RF configuration or buffers. When an application already holds the device, use the C/C++ interface inside that application; it cannot be controlled from another OCH process.

## RGB states

OCH 0.3.6 supports both the current 15% firmware and the older 25% firmware, and shows brightness as the device actually reports it. The table below gives the color values for the current 15% firmware.

| State | Color | R/G/B | Blink |
|---|---|---|---|
| initializing | Yellow | 38/38/0 | Solid |
| idle | Green | 0/38/0 | Solid |
| rx: RX upload on | Blue | 0/0/38 | Solid |
| tx: TX transmit on | Purple | 38/0/38 | Solid |
| duplex: RX upload and TX transmit on | Cyan | 0/38/38 | Solid |
| recovering: bridge error cleanup or RX recovery | Orange | 38/10/0 | 1 Hz |
| fault: RF, digital interface, or bridge initialization failed | Red | 38/0/0 | Solid |
| overtemp: Zynq over-temperature warning | Red | 38/0/0 | 4 Hz |
| off | Black | 0/0/0 | Off |

RGB off takes priority; the rest are, in order of priority: over-temperature, fault, abnormal recovery, initializing, RX/TX, idle. After an error has recovered, no alarm is raised based on past error counts. The color fields in the RGB state are the output values of the current blink phase. The state reflects the SDK's running state, not optical detection, actual RF power, or ADF4002 hardware lock. 15% is a scaling of channel values, not a photometrically calibrated perceived brightness.

## Fan control policy

Fan control uses the Zynq XADC internal temperature, with continuous hardware conversion and 16-sample averaging; software reads it about once per second. Valid temperatures are transferred in millidegrees Celsius; for example, `64500` means 64.5 °C. When the temperature is invalid, the text shows `unknown`, and the C API value is zero with `valid=0`; applications must check the valid flag. If no valid software sample has been obtained for more than 2.5 seconds, or the XADC configuration/reading is abnormal, enabled fan control requests 100% PWM.

| Rising temperature range | High-level duty cycle |
|---|---:|
| Below 40 °C | 0% |
| 40 to below 50 °C | 30% |
| 50 to below 60 °C | 50% |
| 60 to below 70 °C | 70% |
| 70 °C and above | 100% |

Stepping down requires falling 2 °C below the corresponding threshold; for example, going from 50% to 30% requires falling below 48 °C. When starting from 0%, it first outputs 100% for about 500 ms, then runs at the target duty cycle. Turning fan control off immediately requests 0%, and over-temperature does not change the switch automatically. The over-temperature warning triggers at 80 °C and clears below 75 °C; these are software settings of this project.

`duty_permille` and `target_permille` range from 0 to 1000; 700 corresponds to 70%. PWM is fixed at 25 kHz, the fan pin is D19, and the RGB pin is E19. The duty cycle is the high-level fraction of the PWM control pin; whether 0% actually stops the fan and the minimum start-up duty cycle depend on the connected fan, and the interface provides no speed feedback. The service runs in the main loop and in existing delay/UART waits, without a new timer interrupt.

## C/C++ interface

New interfaces that do not change existing ABI 1 structs:

```c
och_rgb_status rgb = {0};
och_fan_status fan = {0};
och_core_temperature temp = {0};
rgb.struct_size = sizeof(rgb);
fan.struct_size = sizeof(fan);
temp.struct_size = sizeof(temp);

/* Check the return code of every call; the device handle comes from och_open. */
int rc = och_get_core_temperature(device, &temp);
if (rc == OCH_OK && temp.valid) {
    double celsius = temp.milli_celsius / 1000.0;
    /* Use celsius. */
}
rc = och_set_rgb_enabled(device, 1);
rc = och_set_fan_enabled(device, 1);
rc = och_get_rgb_status(device, &rgb);
rc = och_get_fan_status(device, &fan);
rc = och_set_fan_enabled(device, 0);
```

The C++ wrapper provides `device.rgb_status()`, `device.fan_status()`, `device.core_temperature()`, `device.set_rgb_enabled(bool)`, and `device.set_fan_enabled(bool)`. Set operations verify the read-back, and an identical setting is not written again. A control failure returns an error but does not stop existing streams.

The host installer only updates OCH. The board needs exported hardware that includes BOARD_CTRL_AXI and an SDK ELF or BOOT.bin with board control support loaded; the installer does not download board firmware.
