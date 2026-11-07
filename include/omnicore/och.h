/* OmniCore Host public ABI 1. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef OMNICORE_OCH_H
#define OMNICORE_OCH_H
#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32)
# if defined(OCH_BUILD)
#  define OCH_API __declspec(dllexport)
# else
#  define OCH_API __declspec(dllimport)
# endif
#else
# define OCH_API __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define OCH_ABI_VERSION 1u
#define OCH_VERSION "0.6.2"
#define OCH_PPS_LOCK_TOLERANCE_PPM 1
typedef struct och_device och_device;
typedef struct och_stream och_stream;
typedef enum och_result {
    OCH_OK=0, OCH_TIMEOUT=-1, OCH_INVALID=-2, OCH_NOT_FOUND=-3,
    OCH_BUSY=-4, OCH_IO=-5, OCH_UNSUPPORTED=-6, OCH_STOPPED=-7,
    OCH_BUFFER_SMALL=-8, OCH_PROTOCOL=-9
} och_result;
typedef enum och_direction { OCH_RX=0, OCH_TX=1 } och_direction;
typedef enum och_format { OCH_PACKED12=0, OCH_SC16=1, OCH_FC32=2, OCH_SC8=3 } och_format;
enum { OCH_META_START=1u, OCH_META_DISCONTINUITY=2u, OCH_META_HOST_OVERFLOW=4u,
       OCH_META_DEVICE_OVERFLOW=8u, OCH_META_TIME_UNKNOWN=16u, /* no longer produced */
       OCH_META_RECONFIGURED=32u,
       /* Framed USB transport (always on since 0.6.0): */
       OCH_META_HAS_TIME=64u,      /* time_ticks is the hardware time of the first sample */
       OCH_META_END_OF_BURST=128u, /* last samples of a finite/stopped RX command */
       OCH_META_LATE=256u };       /* the RX command time had already passed */
enum { OCH_EVENT_DEVICE=1, OCH_EVENT_RX_SYNC=2, OCH_EVENT_HOST_OVERFLOW=3,
       OCH_EVENT_TRANSPORT=4, OCH_EVENT_TX_STARVED=5, OCH_EVENT_RECONFIGURED=6,
       /* Hardware-timed events (FPGA event FIFO); och_event.hw_time is valid. */
       OCH_EVENT_TX_BURST_ACK=7, OCH_EVENT_TX_LATE=8, OCH_EVENT_TX_UNDERFLOW=9,
       OCH_EVENT_TX_ERROR=10, OCH_EVENT_RX_GAP=11, OCH_EVENT_RX_LATE=12,
       OCH_EVENT_TIME_INVALIDATED=13 };
/* TX burst metadata flags (och_send_burst). */
enum { OCH_TX_HAS_TIME=1u, OCH_TX_START_OF_BURST=2u, OCH_TX_END_OF_BURST=8u };
/* RX stream start / och_rx_command flags. */
enum { OCH_RX_TIMED=1u,    /* stream args: RX waits for a command; start_time used */
       OCH_RX_NOW=2u,      /* start immediately (ignore time) */
       OCH_RX_STOP=4u };   /* och_rx_command: stop, finishing with END_OF_BURST */
/* PPS / time source for och_set_time_source. */
enum { OCH_TIME_SOURCE_NONE=0, OCH_TIME_SOURCE_EXTERNAL=1, OCH_TIME_SOURCE_INTERNAL=2 };
typedef struct och_device_info {
    uint32_t struct_size, abi_version;
    char serial[128], name[128], driver[32], firmware[128];
    uint32_t usb_speed_mbps; /* 5000 SuperSpeed, 480 USB 2.0 High-Speed; 0 = not queried (och_find) */
    uint32_t rx_channels, tx_channels, hardware_timestamps;
    uint32_t max_sample_rate; /* RF capability, not a guaranteed USB streaming rate. */
} och_device_info;
/* Shared sample clock. Channel masks: 0=disabled, 1=channel1, 2=channel2, 3=both.
 * All frequencies here are Hz, gain/attenuation are dB. No implicit TX start. */
typedef struct och_config {
    uint32_t struct_size, sample_rate, rx_mask, tx_mask;
    uint64_t rx_frequency, tx_frequency;
    uint32_t rx_bandwidth, tx_bandwidth;
    double rx_gain[2], tx_attenuation[2];
    uint32_t rx_agc; /* 0 manual, 1 fast, 2 slow, 3 hybrid (device supported modes). */
    uint32_t usb_iq_bits; /* 0 or 12: legacy packed12. 8: signed8 wire, requires both masks=3.
                          * Occupies the first former reserved slot; ABI 1 size is unchanged.
                          * Destroy all streams before changing the wire format. */
    uint32_t usb_framing; /* Ignored: since 0.6.0 every 16 KiB block carries a 16-byte header
                          * with hardware sample time (FPGA CH32H417_IP 1.3); there is
                          * no legacy preamble mode. Kept for source compatibility. */
    uint32_t reserved[5];
} och_config;
typedef struct och_stream_args {
    uint32_t struct_size, direction, format;
    uint32_t transfer_bytes; /* default196608, multiple of 16384, max1048576 */
    uint32_t usb_queue_depth; /* default32 */
    uint32_t host_buffer_bytes; /* default64MiB, allocated before start */
    /* RX, framed transport only: start_flags OCH_RX_TIMED makes the receiver
     * wait; och_start then issues one command at start_time (or OCH_RX_NOW)
     * for num_samples per channel (0 = continuous). Former reserved slots. */
    uint32_t start_flags, num_samples, start_time_lo, start_time_hi;
    uint32_t reserved[6];
} och_stream_args;
typedef struct och_metadata {
    uint32_t struct_size, flags;
    uint64_t epoch; /* increases at each start, gap or time discontinuity */
    uint64_t sample_index; /* per-channel position within this epoch, NOT a HW timestamp */
    uint64_t host_dropped_samples; /* known local loss since previous delivered block */
    int64_t device_dropped_samples; /* -1: unavailable in current FPGA protocol */
    uint64_t host_monotonic_ns; /* observation time, NOT sample time */
    /* 0.6.0 extension (struct_size selects the layout; older sizes remain
     * accepted). Valid when flags has OCH_META_HAS_TIME. 1 tick = 1 sample. */
    uint64_t time_ticks;
    uint32_t tick_rate, reserved_meta;
} och_metadata;
typedef struct och_tx_metadata {
    uint32_t struct_size, flags; /* OCH_TX_* */
    uint64_t time_ticks;         /* first sample of the burst when OCH_TX_HAS_TIME */
} och_tx_metadata;
typedef struct och_time_status {
    uint32_t struct_size, capable, valid, armed, source, ref_present, pps_count, tick_rate;
    uint64_t now, last_pps;      /* sample ticks; now is a snapshot when read */
    uint32_t framing, rx_timed, fpga_version, reserved[5];
} och_time_status;
typedef struct och_stats {
    uint32_t struct_size, running;
    uint64_t usb_bytes, samples, host_dropped_samples, headers, resyncs;
    uint64_t host_queue_bytes, host_queue_peak_bytes, usb_queue_pending;
    uint64_t transfer_errors, device_overflows, device_underflows, event_drops;
    uint64_t tx_starvation_events, elapsed_ns, tx_padding_samples;
} och_stats;
typedef struct och_event {
    uint32_t struct_size, type;
    uint64_t sequence, host_monotonic_ns, epoch;
    char message[512];
    uint64_t hw_time; /* 0.6.0 extension: sample time for timed event types */
    uint32_t code, block;
    /* 0.6.1 extension: device events this record stands for. 1 normally; N
     * for an SDK summary line ("count=N") that coalesces a burst of
     * tx_underflow / rx_overflow_gap events. block/hw_time are the last one. */
    uint32_t count, reserved_event;
} och_event;
/* AD9361 hardware FIR. Both physical channels in the selected direction share
 * these coefficients. get returns the last configuration verified by SDK SPI
 * readback; set verifies hardware again. No host-side software FIR is implied. */
typedef struct och_fir_config {
    uint32_t struct_size, direction, channel_mask, factor, tap_count;
    int32_t gain_db;
    uint32_t enabled, verified, crc32;
    int16_t coefficients[128];
    uint32_t reserved[7];
} och_fir_config;
/* Clock order RX: BBPLL, ADC, R2, R1, RF, sample rate;
 * TX: BBPLL, DAC, T2, T1, TF, sample rate. All values are Hz.
 * reference_hz is read-only (0 on set means preserve it). */
typedef struct och_clock_config {
    uint32_t struct_size, reference_hz, rx_hz[6], tx_hz[6], reserved[8];
} och_clock_config;
/* AD9361 RX tracking shared by both physical receive channels. DC controls
 * RF and baseband DC tracking together; IQ controls quadrature tracking.
 * Disabling tracking preserves existing correction coefficients; IQ phase/gain
 * correction stays enabled. Not a host IQ filter or TX calibration. */
typedef struct och_rx_correction_config {
    uint32_t struct_size, dc_enabled, iq_enabled, reserved[5];
} och_rx_correction_config;
typedef enum och_signal_source { OCH_SOURCE_INTERNAL=0, OCH_SOURCE_EXTERNAL=1 } och_signal_source;
/* Nominal RF frequencies in Hz, not measurements of an external signal.
 * External LO input must be twice the nominal RF frequency (70 MHz..4 GHz).
 * Returning to internal restores the SDK's saved internal PLL frequency. */
typedef struct och_lo_source_config {
    uint32_t struct_size, rx_source, tx_source, reserved0;
    uint64_t rx_frequency, tx_frequency;
    uint32_t reserved[8];
} och_lo_source_config;
/* ADF4002 disciplines the board's 40 MHz oscillator from a 10 MHz input.
 * The schematic connects MUXOUT only to LED D5, with no processor feedback.
 * chip_verified/led_known remain 0; led_on remains -1.
 * Since 0.3.4, lock_known/locked describe PPS frequency monitoring, NOT the
 * ADF4002 hardware lock. A valid, fresh PPS measurement gives lock_known=1;
 * locked=1 means the DATA_CLK error is within +/-1 ppm (inclusive).
 * In VCXO mode PPS (PPS without 10 MHz, SDK vcxo=1) they instead report the
 * CH32 PA4 loop: lock_known=1 while the bridge answers, locked as in
 * och_vcxo_status. Mode REF never reads lock (judge D5), as before.
 * Disabled/absent/invalid/unsupported/failed PPS monitoring gives
 * lock_known=0, locked=-1: hide the lock indicator and judge PLL lock by D5.
 * Reference selection does not require PPS or wait for frequency lock.
 * gpio_verified describes processor GPIO pad readback, not ADF4002 readback.
 * led_mode is 0=off or 1=digital lock indication; it is not observed LED state.
 * source is meaningful only when source_known=1. A fault can leave cached
 * source=0 with source_known=0 and last_error!=0; explicitly set internal to
 * recover. These diagnostic fields occupy formerly reserved slots. */
typedef struct och_reference_status {
    uint32_t struct_size, source, initialized, gpio_verified, chip_verified;
    uint32_t lock_known;
    int32_t locked;
    uint32_t led_known;
    int32_t led_on;
    uint32_t led_mode, reference_hz, oscillator_hz, r_counter, n_counter;
    uint32_t source_known;
    int32_t last_error;
    uint32_t reserved[8];
} och_reference_status;
/* PPS is monitoring only; cold boot defaults off. Measured frequency and
 * error_ppm are meaningful only when frequency_valid=1 (otherwise zero).
 * age_ms=UINT32_MAX means no edge since enable/rearm. No UTC or IQ timestamp.
 * Since 0.3.4, lock_known/locked use formerly reserved slots (size unchanged).
 * Show locked only when lock_known=1; 1 means abs(error)<=1 ppm, 0 means
 * outside tolerance. Otherwise locked=-1. This is not PLL/phase lock. */
typedef enum och_pps_state {
    OCH_PPS_DISABLED, OCH_PPS_WAITING, OCH_PPS_VALID, OCH_PPS_NO_SIGNAL,
    OCH_PPS_INVALID_INTERVAL, OCH_PPS_CLOCK_UNAVAILABLE
} och_pps_state;
typedef struct och_pps_status {
    uint32_t struct_size, available, enabled, state, signal_present, frequency_valid;
    uint32_t nominal_hz, measured_hz, pulse_count, invalid_count, age_ms;
    uint32_t raw_counter, hardware_timeout, lock_known;
    double error_ppm;
    int32_t locked;
    uint32_t reserved[7];
} och_pps_status;
typedef struct och_reference_test_result {
    uint32_t struct_size, gpio_verified, chip_verified, visual_confirmation_required;
    uint32_t reserved[4];
} och_reference_test_result;
/* 40 MHz VCTCXO tune node, shared by the ADF4002 charge pump and CH32 PA4
 * (DAC1). Mode = reference source + 2 * PPS enable (SDK vcxo=1):
 * OFF both high-Z; REF ADF4002 PLL to 10 MHz; PPS the CH32 drives PA4 from
 * the Zynq PPS DATA_CLK counts; REF_PPS ADF4002 PLL, PPS is reference only.
 * The SDK enables the charge pump only after the CH32 reports PA4 high-Z for
 * the current request; pending=1 until then, error=-116 after a 3 s timeout.
 * CH32 fields (state..samples) are meaningful only when bridge_alive=1;
 * otherwise state=UNKNOWN, pa4_driving=-1, dac_code=-1 and the rest zero.
 * locked is -1 outside mode PPS; in PPS it is 1 when the CH32 phase loop
 * tracks within +/-1000 ns of the PPS edges. */
typedef enum och_vcxo_mode {
    OCH_VCXO_OFF=0, OCH_VCXO_REF=1, OCH_VCXO_PPS=2, OCH_VCXO_REF_PPS=3
} och_vcxo_mode;
typedef enum och_vcxo_state {
    OCH_VCXO_STATE_OFF, OCH_VCXO_STATE_CALIBRATE, OCH_VCXO_STATE_ACQUIRE,
    OCH_VCXO_STATE_TRACK, OCH_VCXO_STATE_HOLD, OCH_VCXO_STATE_UNKNOWN
} och_vcxo_state;
typedef struct och_vcxo_status {
    uint32_t struct_size, requested_mode, applied_mode, pending;
    int32_t error;
    uint32_t pll_enabled, bridge_alive, ack_age_ms;
    int32_t pa4_driving, dac_code;
    uint32_t state, samples, request_id;
    int32_t locked;
    double k_ppb_per_lsb, error_ppb, phase_ns;
    uint32_t reserved[8];
} och_vcxo_status;
/* Best-effort HD3SS3220 startup snapshot from the existing CH32 EP83 log.
 * NOT a live current measurement, USB-PD contract or LED GPIO readback.
 * No timestamps/boot IDs exist in this firmware. Missing/conflicting logs
 * remain unknown. Values are meaningful only when snapshot_valid is 1. */
typedef struct och_power_status {
    uint32_t struct_size, log_endpoint_present, snapshot_valid, conflicting_snapshots;
    uint32_t attached_state, current_detected;
    int32_t source_current_ma; /* -1 unknown, including USB-default advertisement */
    int32_t supports_3a; /* -1 unknown/not attached as sink, 0 no, 1 yes */
    uint32_t raw_08, raw_09, raw_0a;
    uint32_t reserved[5];
} och_power_status;
/* timeout_ms: 0 returns this handle's cache; 1..2000 permits bounded EP83
 * reads when no stream objects exist. During streaming, only cache is read.
 * No log/no EP83/timeout is OCH_OK with snapshot_valid=0, never false 3 A.
 * Successful reads consume debug bytes; cache lasts only for this handle. */
OCH_API int och_get_power_status(och_device*, och_power_status*, uint32_t timeout_ms);
OCH_API int och_get_fir(och_device*, och_direction, och_fir_config*);
/* Typed configuration changes pause existing streams and restore only the
 * directions that were running. Handles/buffers remain valid. RX leases must
 * be released first (BUSY otherwise). Failure stops both directions and
 * invalidates configuration; explicitly configure and recreate streams. */
OCH_API int och_set_fir(och_device*, const och_fir_config*);
OCH_API int och_set_fir_enabled(och_device*, uint32_t rx_enabled, uint32_t tx_enabled);
OCH_API int och_get_clock(och_device*, och_clock_config*);
OCH_API int och_set_clock(och_device*, const och_clock_config*);
/* Sample rate (also och_configure): if the SDK has no exact clock plan for
 * the rate, e.g. 11.2 MSPS needs a BBPLL its fractional-N cannot hit exactly,
 * the nearest whole-Hz rate that it can hit is applied instead (11200011 Hz,
 * typically within ~20 Hz). Read the applied rate with och_get_config or
 * och_get_clock. Rates outside the clock/FIR limits still fail. */
OCH_API int och_set_sample_rate(och_device*, uint32_t sample_rate_hz);
/* Requires SDK system_info rx_correction=1. get reads both current enables.
 * Setters preserve the other correction and verify readback. Unsupported
 * firmware and unchanged settings do not pause streams. Changed settings
 * use the same coordinated reconfiguration and failure semantics as above. */
OCH_API int och_get_rx_correction(och_device*, och_rx_correction_config*);
OCH_API int och_set_rx_dc_correction(och_device*, uint32_t enabled);
OCH_API int och_set_rx_iq_correction(och_device*, uint32_t enabled);
/* Requires SDK external_lo=1. Preserves the other direction, verifies source
 * and nominal frequencies, and synchronizes the och_config frequency cache.
 * Returning RX to another band also reads back band-clamped manual gains.
 * configure preserves source selection; external frequency writes only declare
 * the external generator's frequency, they cannot tune that generator. */
OCH_API int och_get_lo_source(och_device*, och_lo_source_config*);
OCH_API int och_set_rx_lo_source(och_device*, uint32_t source);
OCH_API int och_set_tx_lo_source(och_device*, uint32_t source);
/* Requires adf4002=1 and, for source changes, ref_source=1. Source 0 is internal
 * free-running and source 1 enables discipline from the external 10 MHz input.
 * No external-reference presence or lock guarantee is implied by success.
 * Changes and self-test use coordinated stream pause/restart. The test flashes
 * D5 three times with charge pump tri-stated and restores the previous source;
 * a human must observe D5 to confirm chip communication. Unknown/failed source
 * state is readable but cannot be a setter no-op or self-test starting state.
 * Explicitly set internal to recover before testing; no RF reinit is implicit. */
OCH_API int och_get_reference_status(och_device*, och_reference_status*);
/* With SDK vcxo=1, selecting external waits up to 5 s for the CH32 PA4
 * high-Z handshake and fails with OCH_PROTOCOL if the SDK reports an error. */
OCH_API int och_set_reference_source(och_device*, uint32_t source);
OCH_API int och_test_reference(och_device*, och_reference_test_result*);
/* Requires SDK pps_monitor=1 and PPS-capable FPGA. Both calls are safe during
 * streaming (including an RX lease): no stop, reconfiguration or queue reset.
 * Enable persists across handles/RF reinit, but not firmware restart.
 * With SDK vcxo=1 and internal reference, enabling PPS starts the CH32 PA4
 * loop: its first calibration steps the 40 MHz oscillator by about 1-2 ppm
 * for 18 s, and acquisition then pulls it to the PPS frequency. */
OCH_API int och_get_pps_status(och_device*, och_pps_status*);
OCH_API int och_set_pps_enabled(och_device*, uint32_t enabled);
/* Requires SDK vcxo=1. Safe during streaming; no stop or reconfiguration. */
OCH_API int och_get_vcxo_status(och_device*, och_vcxo_status*);
/* Board services added in 0.3.5; existing ABI 1 structures are unchanged.
 * RGB defaults on at 15% channel scale; legacy 25% firmware is also supported.
 * Fan automatic control defaults off.
 * Output colors include the instantaneous blink phase. Fan duty is high time,
 * not measured RPM. Invalid temperatures have value=0; always check valid.
 * milli_celsius is Zynq die temperature; age_ms=UINT32_MAX means no sample. */
typedef enum och_rgb_state {
    OCH_RGB_OFF, OCH_RGB_INITIALIZING, OCH_RGB_IDLE, OCH_RGB_RX, OCH_RGB_TX,
    OCH_RGB_DUPLEX, OCH_RGB_RECOVERING, OCH_RGB_FAULT, OCH_RGB_OVERTEMP
} och_rgb_state;
typedef struct och_rgb_status {
    uint32_t struct_size, available, enabled, brightness_pct, state;
    uint32_t red, green, blue, blink_hz, reserved[7];
} och_rgb_status;
typedef struct och_fan_status {
    uint32_t struct_size, available, enabled, frequency_hz, duty_permille, target_permille;
    uint32_t boost, failsafe, temperature_valid;
    int32_t temperature_mc;
    uint32_t temperature_age_ms, overtemperature, reserved[4];
} och_fan_status;
typedef struct och_core_temperature {
    uint32_t struct_size, valid;
    int32_t milli_celsius;
    uint32_t age_ms, overtemperature, reserved[3];
} och_core_temperature;
/* Requires board_rgb / fan_control / core_temp capabilities respectively.
 * These calls preserve active streams, queues and outstanding RX leases.
 * Switches persist across RF reinit and host handles, not firmware restart. */
OCH_API int och_get_rgb_status(och_device*, och_rgb_status*);
OCH_API int och_set_rgb_enabled(och_device*, uint32_t enabled);
OCH_API int och_get_fan_status(och_device*, och_fan_status*);
OCH_API int och_set_fan_enabled(och_device*, uint32_t enabled);
OCH_API int och_get_core_temperature(och_device*, och_core_temperature*);

/* RX symbol RSSI (0.4.7). Relative dB from the AD9361 RSSI word, higher is
 * stronger; NOT calibrated input power (gain table and front end apply).
 * Reporting is off after SDK cold boot (enabled=0, both invalid) until
 * och_set_rx_rssi_enabled(1). valid=0 also while the ENSM is not receiving;
 * rssi_db is then 0. usb_rx_mask is the USB upload mask only: both receivers
 * measure in 2R2T. Query/switch only: active streams continue. Older SDKs
 * without rssi=1 have no switch (enabled=1, set returns UNSUPPORTED) and are
 * read from the RSSI lines of system_status?. */
typedef struct och_rx_rssi {
    uint32_t struct_size, enabled, usb_rx_mask, valid[2];
    double rssi_db[2];
    uint32_t reserved[4];
} och_rx_rssi;
OCH_API int och_get_rx_rssi(och_device*, och_rx_rssi*);
OCH_API int och_set_rx_rssi_enabled(och_device*, uint32_t enabled);

/* CH32 bridge firmware (0.5.0). Uses USB EP0 vendor requests only, so it
 * works without the ZYNQ SDK and while the bridge runs its bootloader.
 * Versions are major<<16 | minor<<8 | patch. LEGACY: bridge firmware without
 * the bootloader (before 1.0.0); flash Merge.bin once with WCH-Link. The
 * serial argument selects a device as in och_open (NULL: the only one); the
 * device must not be open by another handle or process (BUSY). */
enum { OCH_FW_MODE_LEGACY=0, OCH_FW_MODE_APP=1, OCH_FW_MODE_BOOTLOADER=2 };
enum { OCH_FW_APP_PROGRAMMED=1u, OCH_FW_APP_HEADER_OK=2u, OCH_FW_APP_IMAGE_OK=4u };
typedef struct och_firmware_info {
    uint32_t struct_size, mode;
    uint32_t running_version;    /* bridge application or bootloader answering */
    uint32_t bootloader_version; /* 0 = no bootloader */
    uint32_t app_version;        /* from the installed image header, 0 = none */
    uint32_t app_status;         /* OCH_FW_APP_* */
    uint32_t app_max_size;
    char serial[128], build_id[24];
    uint32_t reserved[8];
} och_firmware_info;
/* .ocfw update image (header + application), validated by och_firmware_read_image. */
typedef struct och_firmware_image {
    uint32_t struct_size, version, image_size, image_crc32, build_unix;
    char build_id[24];
    uint32_t reserved[4];
} och_firmware_image;
enum { OCH_FW_STAGE_BOOTLOADER=1, OCH_FW_STAGE_ERASE=2, OCH_FW_STAGE_WRITE=3,
       OCH_FW_STAGE_VERIFY=4, OCH_FW_STAGE_RESTART=5 };
/* done/total are bytes for OCH_FW_STAGE_WRITE, otherwise 0/0 or 1/1. */
typedef void (*och_firmware_progress)(void* user, uint32_t stage, uint32_t done, uint32_t total);
OCH_API int och_firmware_query(const char* serial, och_firmware_info*);
OCH_API int och_get_firmware_info(och_device*, och_firmware_info*); /* cached at och_open */
OCH_API int och_firmware_read_image(const char* path, och_firmware_image*);
/* Enters the bootloader if needed, erases, writes and verifies the image,
 * then restarts into it and confirms the new version (about 10-20 s). An
 * interrupted update leaves the device in the bootloader; run it again. */
OCH_API int och_firmware_update(const char* serial, const char* path, och_firmware_progress, void* user);
OCH_API int och_firmware_enter_bootloader(const char* serial);
OCH_API int och_firmware_start_app(const char* serial); /* leave the bootloader */

OCH_API const char* och_version(void);
OCH_API const char* och_last_error(void); /* thread-local; valid until next failing call */
OCH_API void och_default_config(och_config*);
OCH_API void och_default_stream_args(och_stream_args*, och_direction);
OCH_API int och_list(och_device_info* devices, size_t capacity, size_t* count);
OCH_API int och_open(const char* serial, och_device**); /* NULL selects only connected device */
OCH_API int och_close(och_device*); /* destroy streams first; otherwise BUSY */
OCH_API int och_get_info(och_device*, och_device_info*);
/* With streams: channel masks must stay unchanged; identical config is a no-op.
 * RX drops queued old samples and reports RECONFIGURED/START/DISCONTINUITY in
 * the next epoch. Running TX discards old queued/in-flight data (delivery of
 * cancelled transfers is unknown), prefills four zero blocks counted in
 * tx_padding_samples, then resumes. OCH_EVENT_RECONFIGURED reports the gap.
 * recv/send may return TIMEOUT during this transaction; retry normally.
 * Since 0.4.7 a change limited to rx/tx_frequency, rx_gain and tx_attenuation
 * keeps streams running (no queue drop, no zero prefill): the next RX block
 * carries RECONFIGURED|DISCONTINUITY at an approximate boundary (USB queue
 * latency) and samples near it may contain the PLL transient. A TX LO change
 * while a TX stream exists still uses the stopping transaction above. */
OCH_API int och_configure(och_device*, const och_config*);
OCH_API int och_get_config(och_device*, och_config*); /* last successfully applied request on this handle; sample_rate is the applied rate */
/* Raw SDK query or command, serialised with event reception. Mutations only while
 * no stream exists; configure again afterwards. Reply is UTF-8, nul terminated. */
OCH_API int och_command(och_device*, const char* command, char* reply, size_t capacity, uint32_t timeout_ms);
OCH_API int och_poll_event(och_device*, och_event*, uint32_t timeout_ms);
OCH_API int och_create_stream(och_device*, const och_stream_args*, och_stream**);
/* BUSY (outstanding RX lease) retains the handle. On OK or IO the handle is
 * destroyed; IO reports a hardware-stop error. Do not retry a freed handle.
 * close/destroy must not race other operations on that device/stream. */
OCH_API int och_destroy_stream(och_stream*);
OCH_API int och_start(och_stream*); /* TX: queue samples with send() before start */
OCH_API int och_stop(och_stream*, uint32_t timeout_ms);
/* Drain host/USB TX, padding the final wire block with zero IQ if required.
 * No concurrent send allowed. This is not an RF end-of-burst acknowledgement.
 * Finite EOF can report tail underflow/starvation before stop; events are retained. */
OCH_API int och_flush(och_stream*, uint32_t timeout_ms);
/* Sample counts are per channel. Layout: time-major interleaved channels.
 * sc16: signed I,Q left-aligned; fc32: normalized I,Q. In 8-bit wire mode,
 * sc16=sign8*256, fc32=sign8/128, packed12 expands with four zero low bits.
 * TX retains the high 8 bits after the existing 12-bit quantization.
 * sc8 is signed I,Q bytes (8-bit wire mode only); other formats keep their sizes.
 * One producer per TX stream and one consumer per RX stream. RX/TX concurrent.
 * recv never combines epochs or discontinuities into one returned buffer. */
OCH_API int och_recv(och_stream*, void* samples, size_t capacity_samples,
                     size_t* received, och_metadata*, uint32_t timeout_ms);
/* Hardware time base. when: 0 now, 1 at the next PPS edge
 * of the selected source. A sample-rate change invalidates the time. */
OCH_API int och_get_time(och_device*, och_time_status*);
OCH_API int och_set_time(och_device*, uint64_t ticks, uint32_t at_next_pps);
OCH_API int och_set_time_source(och_device*, uint32_t source);
/* Timed RX command on a started framed RX stream created with OCH_RX_TIMED:
 * flags OCH_RX_NOW / OCH_RX_STOP; num_samples 0 = continuous. */
OCH_API int och_rx_command(och_stream*, uint32_t flags, uint64_t time_ticks, uint32_t num_samples);
/* Framed TX: one call may carry a whole burst or part of one. Each
 * OCH_TX_START_OF_BURST or OCH_TX_HAS_TIME starts a new 16 KiB block;
 * OCH_TX_END_OF_BURST closes the block and submits it immediately. */
OCH_API int och_send_burst(och_stream*, const void* samples, size_t samples_per_channel,
                           const och_tx_metadata*, size_t* accepted, uint32_t timeout_ms);
OCH_API int och_send(och_stream*, const void* samples, size_t samples_per_channel,
                     size_t* accepted, uint32_t timeout_ms);
/* Zero-copy application view of current wire RX buffers; no conversion:
 * 3 bytes/complex sample for 12-bit, 2 for 8-bit, independent of args.format.
 * A single
 * lease per stream; release before recv/acquire/destroy. Core USB->ring copy
 * still occurs. Capacity/size measured in bytes. Lease invalid after release. */
OCH_API int och_rx_acquire(och_stream*, const void** data, size_t* bytes, och_metadata*, uint32_t timeout_ms);
OCH_API int och_rx_release(och_stream*);
OCH_API int och_get_stats(och_stream*, och_stats*);
OCH_API size_t och_sample_size(och_format format); /* per complex channel sample */
#ifdef __cplusplus
}
#endif
#endif
