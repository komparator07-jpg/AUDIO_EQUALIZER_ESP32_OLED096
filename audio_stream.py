"""
audio_stream.py
Захват системного звука (WASAPI loopback, Windows) -> FFT -> 16 полос -> отправка по Serial на ESP32.
"""

import time

import numpy as np
import serial
import serial.tools.list_ports
import pyaudiowpatch as pyaudio

# ---------------- Настройки ----------------
NUM_BANDS = 16
FPS = 30
BAUD_RATE = 115200

# Ключевые слова для автопоиска COM-порта ESP32 (используется, только если FORCED_COM_PORT = None)
ESP32_PORT_HINTS = ["CP210", "CH340", "USB-SERIAL", "USB Serial", "Silicon Labs"]

# Порт явно задан — автопоиск не используется
FORCED_COM_PORT = "COM10"

FREQ_MIN = 60      # нижняя граница диапазона для полос эквалайзера, Гц
FREQ_MAX = 8000   # верхняя граница, Гц
CHUNK = 1024       # размер буфера чтения аудио
SYNC_BYTES = bytes([0xAA, 0x55])  # маркер начала пакета для ESP32

SILENCE_THRESHOLD = 12.0   # если пик всех полос ниже этого — считаем, что музыки нет
MIN_RUNNING_MAX   = 60.0   # "пол" нормализации — не даём ему провалиться в шум


def find_esp32_port():
    for p in serial.tools.list_ports.comports():
        desc = f"{p.description} {p.manufacturer or ''}"
        for hint in ESP32_PORT_HINTS:
            if hint.lower() in desc.lower():
                return p.device
    return None


def open_serial():
    port = FORCED_COM_PORT or find_esp32_port()
    if not port:
        return None
    try:
        ser = serial.Serial(port, BAUD_RATE, timeout=0)
        print(f"[serial] подключено к {port}")
        return ser
    except Exception as e:
        print(f"[serial] не удалось открыть {port}: {e}")
        return None


def get_loopback_device(pa):
    """Находит WASAPI loopback-устройство для устройства вывода по умолчанию."""
    wasapi_info = pa.get_host_api_info_by_type(pyaudio.paWASAPI)
    default_speakers = pa.get_device_info_by_index(wasapi_info["defaultOutputDevice"])

    if not default_speakers.get("isLoopbackDevice", False):
        for loopback in pa.get_loopback_device_info_generator():
            if default_speakers["name"] in loopback["name"]:
                return loopback
        raise RuntimeError("Не найдено loopback-устройство для вывода звука по умолчанию")

    return default_speakers


def make_log_band_edges(sr, fft_size, num_bands, fmin, fmax):
    """Границы частотных полос в бинах FFT, распределённые логарифмически (как слышит ухо)."""
    freqs = np.fft.rfftfreq(fft_size, 1.0 / sr)
    edges_hz = np.logspace(np.log10(fmin), np.log10(fmax), num_bands + 1)
    edges_bin = np.clip(np.searchsorted(freqs, edges_hz), 0, len(freqs) - 1)
    return edges_bin


def bands_from_fft(magnitude, edges_bin):
    bands = np.zeros(NUM_BANDS)
    for i in range(NUM_BANDS):
        lo, hi = edges_bin[i], max(edges_bin[i + 1], edges_bin[i] + 1)
        bands[i] = np.mean(magnitude[lo:hi]) if hi > lo else magnitude[lo]
    return bands


def main():
    pa = pyaudio.PyAudio()
    device = get_loopback_device(pa)

    sample_rate = int(device["defaultSampleRate"])
    channels = device["maxInputChannels"]
    print(f"[audio] захват с: {device['name']} ({sample_rate} Гц, {channels} кан.)")

    stream = pa.open(
        format=pyaudio.paInt16,
        channels=channels,
        rate=sample_rate,
        frames_per_buffer=CHUNK,
        input=True,
        input_device_index=device["index"],
    )

    edges_bin = make_log_band_edges(sample_rate, CHUNK, NUM_BANDS, FREQ_MIN, FREQ_MAX)
    window = np.hanning(CHUNK)

    ser = open_serial()
    last_reconnect_attempt = 0.0
    frame_interval = 1.0 / FPS
    next_frame_time = time.time()
    running_max = MIN_RUNNING_MAX  # общий (один на все полосы) плавающий максимум громкости

    print("Запущено. Ctrl+C для остановки.")

    try:
        while True:
            raw = stream.read(CHUNK, exception_on_overflow=False)
            samples = np.frombuffer(raw, dtype=np.int16)

            if channels > 1:
                samples = samples.reshape(-1, channels).mean(axis=1)

            samples = samples.astype(np.float32) * window
            spectrum = np.abs(np.fft.rfft(samples))
            bands = bands_from_fft(spectrum, edges_bin)

            overall_level = bands.max()

            if overall_level < SILENCE_THRESHOLD:
                # музыки почти нет — не подстраиваем нормализацию под шум, отдаём тишину
                levels = np.zeros(NUM_BANDS)
            else:
                running_max = max(bands.max(), running_max * 0.98, MIN_RUNNING_MAX)
                levels = np.clip(bands / running_max, 0.0, 1.0)

            levels_byte = (levels * 255).astype(np.uint8)

            now = time.time()
            if now >= next_frame_time:
                next_frame_time = now + frame_interval

                if ser is None or not ser.is_open:
                    if now - last_reconnect_attempt > 2.0:
                        last_reconnect_attempt = now
                        ser = open_serial()
                else:
                    try:
                        ser.write(SYNC_BYTES + levels_byte.tobytes())
                    except Exception as e:
                        print(f"[serial] ошибка записи: {e}")
                        try:
                            ser.close()
                        except Exception:
                            pass
                        ser = None

    except KeyboardInterrupt:
        pass
    finally:
        stream.stop_stream()
        stream.close()
        pa.terminate()
        if ser and ser.is_open:
            ser.close()


if __name__ == "__main__":
    main()
