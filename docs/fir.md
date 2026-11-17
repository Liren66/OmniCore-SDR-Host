# AD9361 FIR and Clocks

These commands control the RX/TX FIR inside the AD9361 chip. They do not apply any further software filtering to the IQ received by the host, and they do not provide a filter frequency-response designer. A successfully parsed file, a matching CRC, and a successful hardware read-back only prove that the configuration was loaded as requested; they cannot be used to claim the passband, stopband, group delay, or analog RF performance has been measured.

## Typical workflow

~~~powershell
och clock status
och fir status --json
och clock sample-rate 30.72M
och fir rx load my_filter.ftr
och fir tx load my_filter.ftr
och fir both on
och fir status --json
och bench --rx --tx --sample-rate 30.72M --channels 1,2 --seconds 10
och fir both off
~~~

First design/choose coefficients for the actual sample rate, then confirm they are usable given the interpolation/decimation factors declared in the file and the current clocks. The command sequence above assumes the coefficients suit 30.72 MSPS as an example; it does not mean any file can be used at this sample rate. Some RX/TX factor combinations can only be enabled together; both on sets both sides in one transaction, while enabling only rx or tx keeps the other side's current on/off state.

Each load only loads the given direction, and that direction is off after a successful load; it never transmits implicitly. RX1/RX2 share one set of RX coefficients and TX1/TX2 share one set of TX coefficients; this release does not support different FIRs for the two channels of the same direction. The sample rate and the AD9361 clock chain are still shared by RX/TX, so changing one direction must not be understood as changing its clock independently.

The C/C++ FIR / clock write interfaces can be called while streams are running, with the device coordinating pause, apply, and resume; for the rules and failure handling see [API: Changing parameters at runtime](api.md#changing-parameters-at-runtime). The SDK performs state checks, two-channel coefficient SPI verification, clock read-back, and digital path checks for every hardware transaction; if an error shows `rollback=failed`, `applied=unknown`, you must first complete `rf.reinit` and confirm it succeeded. Standalone CLI commands still need to open the device exclusively and cannot change a device another process is streaming on.

A loaded custom FIR is kept in the RAM state of the device SDK, and subsequent normal configure/stream commands keep it. It is not guaranteed to survive a power cycle or reloading the ELF; the installer does not write the boot flash. Older SDKs need fir_upload=1 and clock_config=1 in system_info?; see [Firmware Compatibility](firmware-compatibility.md).

## Supported FTR format

The coefficient part of ADI FTR text and explicitly recognized configuration fields are supported, using the case and syntax below. Decimal integers only; no floating point, exponents, or k/M/G suffixes on coefficients. A UTF-8 BOM, CRLF, blank lines, and trailing # comments are accepted.

~~~text
# With two columns the order is always TX, RX; this only shows the syntax, and the ellipsis is not a valid coefficient.
TX 3 GAIN -6 INT 2
RX 3 GAIN 0 DEC 2
12, -34
56, 78
...
~~~

With only one direction, only the matching RX or TX header is needed. A single column is used for the direction declared in the file; two columns must declare both RX/TX, with TX in the first column and RX in the second. A file cannot mix single and double columns. All headers and configuration fields must come before the coefficients.

The project ships [identity16.ftr](../examples/fir/identity16.ftr) as a loadable format/transaction test file: two columns for TX/RX, factor=1, gain=0, first tap 32767, and the other 15 taps 0. It is not a recommended low-pass filter, does not promise exact unity gain or a measured RF frequency response, and cannot replace the filter design your application needs.

| Field | Constraint |
|---|---|
| Channel mask | Must be 3 for the selected direction, i.e. both physical channels of that direction |
| Each coefficient | Signed 16-bit integer, -32768..32767 |
| Tap count | 16..128, must be a multiple of 16 |
| RX DEC / TX INT | 1, 2, or 4 |
| RX GAIN | -12, -6, 0, 6 dB |
| TX GAIN | -6, 0 dB |
| TX INT 1 | At most 64 taps |
| File / line | At most 256 KiB / 4096 bytes |

These are only static limits. The usable tap count is further limited by the current ADC/DAC clocks, the sample rate, and the configuration of the other direction. A valid 128-tap text may be rejected by hardware verification under some high-speed clock combinations; the program will not lower the sample rate on its own to make it pass.

The parser rejects unknown fields, duplicate headers, duplicate configuration fields, incomplete integers, out-of-range values, overlong lines, and binary content. The file is fully validated before the device is opened, so bad input never starts an upload.

## Full FTR clock/bandwidth configuration

Files generated by ADI may also contain these fields:

~~~text
RTX <BBPLL> <DAC> <T2> <T1> <CLKTF> <TX_SAMPLE>
RRX <BBPLL> <ADC> <R2> <R1> <CLKRF> <RX_SAMPLE>
BWTX <Hz>
BWRX <Hz>
~~~

The FIR loader in this release **does not apply these clock and bandwidth values automatically**. Files containing these fields are rejected by default, to avoid the ambiguous state where the coefficients are loaded but the matching clocks are not applied. If you really only need the coefficients, gain, and factor from it, run explicitly:

~~~powershell
och fir rx load full_profile.ftr --coefficients-only --json
~~~

The program still verifies that these fields have the right number of positive uint32 decimal Hz values, prints a notice on stderr, and lists the ignored fields in the JSON ignored_profile_fields. Ignoring them is not a successful import of the complete filter configuration, and the designed frequency response cannot be guaranteed on that basis.

When you need the complete configuration, set a valid chain through clock sample-rate or clock set according to the design requirements, set the analog bandwidth through set rx.rf-bandwidth / set tx.rf-bandwidth, and then check the actual state. Changing the sample rate changes the frequency response in Hz of the same coefficients, so they must be re-evaluated or redesigned.

## Clock commands

clock status reads back reference_hz and both sets of six-stage clocks. clock sample-rate 30.72M lets the SDK compute a valid shared chain and explicitly rejects combinations outside the clock or FIR range. If the fractional divider simply cannot hit the rate exactly (e.g. 11.2 MSPS), the nearest whole-Hz rate that can be achieved exactly is used instead (11200011 Hz, see the [API Reference](api.md)), and the requested and actual values are reported as a `WARN` (in JSON mode, `sample_rate_adjusted` on stderr). set rx.sample-rate, set tx.sample-rate, set clock.sample-rate, config, and the `--sample-rate` of streaming commands all use the same entry point.

When you need to specify the chain exactly:

~~~powershell
och clock set --rx-chain "983.04M,245.76M,122.88M,61.44M,30.72M,30.72M" --tx-chain "983.04M,245.76M,122.88M,61.44M,30.72M,30.72M"
~~~

This is an example of the chain format with the FIR bypassed; in practice it must match the enabled FIR factors. Each set must contain exactly six positive integer Hz values, optionally with k/M/G suffixes, with no spaces after the commas. The RX order is BBPLL, ADC, R2, R1, RF, sample rate; the TX order is BBPLL, DAC, T2, T1, TF, sample rate. BBPLL and the final sample rate must be the same on both sides; the SDK additionally checks valid dividers, clock ranges, and the FIR computation budget. On success the output is the actual read-back.

The AD9361 reference clock is a fixed 40 MHz and cannot be changed through clock set. Since 0.3.2, the ADF4002 can discipline this 40 MHz oscillator to an external 10 MHz; see [Reference Clock and External LO](reference-lo.md). For PPS disciplining see [PPS](pps.md), for timed RX/TX see [Hardware Timestamps](timestamps.md), and for synchronizing sample clocks across boards see [Multi-Board Sync](multi-board-sync.md); selecting the external reference by itself does not constitute coherent sync or proof of lock.

## C/C++ interfaces

och_get_fir / och_set_fir / och_set_fir_enabled correspond to FIR state, configuration, and enabling both directions; och_get_clock / och_set_clock / och_set_sample_rate correspond to clocks. The C++ device provides wrappers with the same names: fir, set_fir, set_fir_enabled, clock, set_clock, set_sample_rate.

verified in the FIR state means the configuration saved by the SDK passed SPI verification; queried coefficients come from the most recent verified snapshot, not a fresh SPI read on every query. CRC32 checks the consistency of uploaded/snapshot coefficients and does not mean the frequency response is acceptable. The API does not accept file paths: higher layers can design their own coefficients and fill in och_fir_config; FTR file parsing is currently a CLI feature.

For background on the format, see the [ADI AD9361 driver documentation](https://analogdevicesinc.github.io/linux/drivers/iio-transceiver/ad9361/) and the [ADI Filter Wizard](https://github.com/analogdevicesinc/ad936x-filter-wizard). This project does not copy their filter design implementation.
