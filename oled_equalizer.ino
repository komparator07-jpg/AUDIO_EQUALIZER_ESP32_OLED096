// ============================================================
// Анимация эквалайзера для OLED 0.96" 128x64 (SSD1306) + ESP32
// I2C: SDA -> GPIO21, SCL -> GPIO22
// Кнопка переключения режимов -> GPIO4 (на GND, INPUT_PULLUP)
//
// Библиотеки (Arduino Library Manager):
//   Adafruit GFX Library
//   Adafruit SSD1306
//
// Источник "музыки": реальные данные с компьютера по Serial (от
// Python-скрипта audio_stream.py), либо, если данных с ПК нет дольше
// 60 секунд — синтетическая генерация.
// ============================================================

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <math.h>

#define SCREEN_WIDTH   128
#define SCREEN_HEIGHT  64
#define OLED_RESET     -1
#define SCREEN_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

#define BTN_PIN 4

// ---------------- Настройки ----------------
#define NUM_BANDS 16
const int NUM_MODES = 7;
float BPM = 128.0;              // темп "трека" — влияет на частоту баса

// ---------------- Состояние полос ----------------
float bandLevel[NUM_BANDS];     // текущий уровень 0..1 (сглаженный)
float bandPeak[NUM_BANDS];      // "пик" с медленным спадом
float bandPhase[NUM_BANDS];
float bandFreq[NUM_BANDS];

int mode = 0;

unsigned long lastBeat = 0;
unsigned long beatInterval;
float beatPulse = 0;            // импульс "бас-бочки", 1.0 -> 0 после удара
bool beatJustHit = false;       // true ровно один кадр в момент удара баса

int lastRawState   = HIGH; // последнее "сырое" (недебаунсенное) чтение пина
int stableState     = HIGH; // последнее подтверждённое (стабильное) состояние
unsigned long lastDebounce = 0;

// ---------------- Режим 4: световая волна ----------------
#define NUM_WAVE_NODES 8
const int NET_BASELINE = 46;    // "нулевая" линия — куда всё стремится вернуться
const int NET_AMPLITUDE = 50;   // максимальная высота подъёма узла над базовой линией
const int NET_SUBLINES = 2;     // количество тонких линий под основной (не меняется)

float waveElevation[NUM_WAVE_NODES];   // текущая высота узла над базовой линией, 0..1

#define NUM_DUST 24
float dustX[NUM_DUST];
float dustY[NUM_DUST];
float dustVY[NUM_DUST];
bool dustReady = false;

// ---------------- Приём реального аудио с компьютера по Serial ----------------
#define REAL_AUDIO_TIMEOUT 60000UL // мс без пакетов с ПК — возврат к синтетике

float realBandLevel[NUM_BANDS];        // уровни 0..1, пришедшие с компьютера
unsigned long lastAudioPacketTime = 0; // millis() последнего валидного пакета
bool realAudioActive = false;          // сейчас используем реальные данные?

// ---------------- Коррекция чувствительности полос (только для РЕАЛЬНОГО аудио) ----------------
// Индексы 0-3 — бас, 4-10 — середина, 11-15 — верха.
// Значения <1.0 приглушают полосу, >1.0 — усиливают. Подбирается на слух под свою музыку —
// правь прямо тут и перезаливай скетч, это быстрее, чем менять Python-скрипт.
float bandGain[NUM_BANDS] = {
  0.45, 0.50, 0.55, 0.65,   // бас — заметно приглушаем, чтобы не "выстреливал" на весь экран
  0.80, 0.85, 1.9, 1.00,   // нижняя середина
  1.60, 1.70, 1.80, 1.95,   // верхняя середина
  2.70, 2.75, 2.80, 2.95    // верха — слегка усиливаем, чтобы были заметны
};

// ============================================================
void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  pinMode(BTN_PIN, INPUT_PULLUP);

  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("Ошибка инициализации SSD1306");
    for (;;) delay(1000);
  }
  display.clearDisplay();
  display.display();

  randomSeed(analogRead(34)); // любой не занятый ADC-пин

  for (int i = 0; i < NUM_BANDS; i++) {
    bandLevel[i] = 0;
    bandPeak[i]  = 0;
    bandPhase[i] = random(0, 628) / 100.0;
    // низкие полосы "дышат" медленнее (бас), высокие — быстрее (верха)
    bandFreq[i]  = 0.6 + i * 0.15 + random(-10, 10) / 100.0;
  }

  beatInterval = (unsigned long)(60000.0 / BPM);
}

// ---------------- Приём пакетов от Python-скрипта на компьютере ----------------
// Формат пакета: 0xAA 0x55 + 16 байт (0..255) — уровни полос спектра
void readSerialAudio() {
  static uint8_t state = 0; // 0 - ждём 0xAA, 1 - ждём 0x55, 2 - читаем 16 байт данных
  static uint8_t buf[NUM_BANDS];
  static uint8_t idx = 0;

  while (Serial.available() > 0) {
    uint8_t b = (uint8_t)Serial.read();

    switch (state) {
      case 0:
        if (b == 0xAA) state = 1;
        break;

      case 1:
        if (b == 0x55) {
          state = 2;
          idx = 0;
        } else if (b != 0xAA) {
          state = 0;
        }
        break;

      case 2:
        buf[idx++] = b;
        if (idx >= NUM_BANDS) {
          for (int i = 0; i < NUM_BANDS; i++) {
            realBandLevel[i] = buf[i] / 255.0;
          }
          lastAudioPacketTime = millis();
          realAudioActive = true;
          state = 0;
        }
        break;
    }
  }

  // если пакетов с компьютера нет дольше REAL_AUDIO_TIMEOUT — возвращаемся к синтетике
  if (realAudioActive && (millis() - lastAudioPacketTime > REAL_AUDIO_TIMEOUT)) {
    realAudioActive = false;
  }
}

// ---------------- Генератор "музыки" (реальной или синтетической) ----------------
void updateFakeAudio() {
  unsigned long now = millis();

  // удар баса по метроному BPM — художественный "пульс" для некоторых режимов
  beatJustHit = false;
  if (now - lastBeat >= beatInterval) {
    lastBeat = now;
    beatPulse = 1.0;
    beatJustHit = true;
  }
  beatPulse *= 0.90; // экспоненциальный спад импульса

  float t = now / 1000.0;

  for (int i = 0; i < NUM_BANDS; i++) {
    float target;

    if (realAudioActive) {
      // реальные данные, пришедшие с компьютера — с поправкой на чувствительность полосы
      target = constrain(realBandLevel[i] * bandGain[i], 0.0, 1.0);
    } else {
      // синтетическая "музыка" — как раньше, если с компьютера давно ничего не пришло
      float base   = sin(t * bandFreq[i] + bandPhase[i]) * 0.5 + 0.5;
      float wobble = sin(t * (bandFreq[i] * 2.7) + i) * 0.15;
      float noise  = random(-8, 8) / 100.0;

      float bassBoost = 0;
      if (i < 4) bassBoost = beatPulse * (1.0 - i * 0.2);

      target = constrain(base + wobble + noise + bassBoost, 0.0, 1.0);
    }

    // атака быстрее, спад медленнее — как в настоящем анализаторе спектра
    if (target > bandLevel[i]) bandLevel[i] += (target - bandLevel[i]) * 0.6;
    else                       bandLevel[i] += (target - bandLevel[i]) * 0.25;

    // пиковый маркер с медленным затуханием
    if (bandLevel[i] > bandPeak[i]) bandPeak[i] = bandLevel[i];
    else {
      bandPeak[i] -= 0.012;
      if (bandPeak[i] < 0) bandPeak[i] = 0;
    }
  }
}

// ---------------- Кнопка ----------------
void checkButton() {
  int reading = digitalRead(BTN_PIN);

  // если "сырое" чтение изменилось (дребезг или реальное нажатие) — сбрасываем таймер
  if (reading != lastRawState) {
    lastDebounce = millis();
  }

  // если состояние держится стабильно дольше 40 мс — считаем его подтверждённым
  if ((millis() - lastDebounce) > 40) {
    if (reading != stableState) {
      stableState = reading;
      if (stableState == LOW) {           // подтверждённое нажатие (кнопка на GND)
        mode = (mode + 1) % NUM_MODES;
        Serial.print("Режим: ");
        Serial.println(mode);
      }
    }
  }

  lastRawState = reading;
}

// ================= Режим 0: классические столбики =================
void drawClassicBars() {
  int barWidth = SCREEN_WIDTH / NUM_BANDS;
  int gap = 1;
  for (int i = 0; i < NUM_BANDS; i++) {
    int x = i * barWidth;
    int h = (int)(bandLevel[i] * (SCREEN_HEIGHT - 4));
    int y = SCREEN_HEIGHT - h;
    display.fillRect(x, y, barWidth - gap, h, SSD1306_WHITE);

    int peakY = SCREEN_HEIGHT - (int)(bandPeak[i] * (SCREEN_HEIGHT - 4)) - 2;
    display.drawFastHLine(x, peakY, barWidth - gap, SSD1306_WHITE);
  }
}

// ================= Режим 1: зеркальные столбики (клубный стиль) =================
void drawMirroredBars() {
  int barWidth = SCREEN_WIDTH / NUM_BANDS;
  int gap = 1;
  int centerY = SCREEN_HEIGHT / 2;
  for (int i = 0; i < NUM_BANDS; i++) {
    int x = i * barWidth;
    int h = (int)(bandLevel[i] * (centerY - 2));
    display.fillRect(x, centerY - h, barWidth - gap, h, SSD1306_WHITE);
    display.fillRect(x, centerY, barWidth - gap, h, SSD1306_WHITE);
  }
  if (beatPulse > 0.5) {
    display.drawFastHLine(0, centerY, SCREEN_WIDTH, SSD1306_WHITE); // вспышка на удар баса
  }
}

// ================= Режим 2: круговой (радиальный) спектр =================
void drawRadialSpectrum() {
  int cx = SCREEN_WIDTH / 2;
  int cy = SCREEN_HEIGHT / 2;
  int rBase = 8 + (int)(beatPulse * 5); // центральный пульсирующий диск

  int ringRadius = rBase + 1;          // тонкое кольцо вокруг диска
  int radialGap  = 3;                  // зазор между кольцом и лучами
  float r0 = ringRadius + radialGap;   // начало лучей
  float maxLen = (SCREEN_HEIGHT / 2) - r0 - 2; // чтобы не вылезать за экран
  if (maxLen < 4) maxLen = 4;

  float angleStep = (2 * PI) / NUM_BANDS;
  float halfWidth = angleStep * 0.32;  // чуть больше зазор между лучами, чем раньше

  for (int i = 0; i < NUM_BANDS; i++) {
    float angle = angleStep * i - PI / 2; // старт "сверху", как стрелки часов
    float len = 3 + bandLevel[i] * maxLen;
    float r1 = r0 + len;

    float aA = angle - halfWidth;
    float aB = angle + halfWidth;

    int x0a = cx + cos(aA) * r0, y0a = cy + sin(aA) * r0;
    int x0b = cx + cos(aB) * r0, y0b = cy + sin(aB) * r0;
    int x1a = cx + cos(aA) * r1, y1a = cy + sin(aA) * r1;
    int x1b = cx + cos(aB) * r1, y1b = cy + sin(aB) * r1;

    // трапеция из двух треугольников — толстый "луч"
    display.fillTriangle(x0a, y0a, x0b, y0b, x1a, y1a, SSD1306_WHITE);
    display.fillTriangle(x0b, y0b, x1a, y1a, x1b, y1b, SSD1306_WHITE);

    // "колпачок" на конце луча
    display.fillCircle((x1a + x1b) / 2, (y1a + y1b) / 2, 1, SSD1306_WHITE);
  }

  // пульсирующий залитый диск в центре + тонкое кольцо вокруг него
  display.fillCircle(cx, cy, rBase - 1, SSD1306_WHITE);
  display.drawCircle(cx, cy, ringRadius, SSD1306_WHITE);
}

// ================= Режим 3: сегментированный LED-эквалайзер =================
void drawSegmentedMeter() {
  int barWidth = SCREEN_WIDTH / NUM_BANDS;
  int gap = 2;
  int segH = 4;
  int segGap = 1;
  int maxSegs = SCREEN_HEIGHT / (segH + segGap);

  for (int i = 0; i < NUM_BANDS; i++) {
    int x = i * barWidth;
    int litSegs = (int)(bandLevel[i] * maxSegs);
    for (int s = 0; s < litSegs; s++) {
      int y = SCREEN_HEIGHT - (s + 1) * (segH + segGap);
      display.fillRect(x, y, barWidth - gap, segH, SSD1306_WHITE);
    }
    int peakSeg = (int)(bandPeak[i] * maxSegs);
    if (peakSeg > 0 && peakSeg <= maxSegs) {
      int y = SCREEN_HEIGHT - peakSeg * (segH + segGap);
      display.drawFastHLine(x, y, barWidth - gap, SSD1306_WHITE);
    }
  }
}

// ================= Режим 4: световая волна =================
void initDust() {
  for (int i = 0; i < NUM_DUST; i++) {
    dustX[i]  = random(0, SCREEN_WIDTH);
    dustY[i]  = random(NET_BASELINE, SCREEN_HEIGHT);
    dustVY[i] = 0.15 + random(0, 25) / 100.0; // скорость падения
  }
  dustReady = true;
}

void updateDust() {
  if (!dustReady) initDust();
  for (int i = 0; i < NUM_DUST; i++) {
    dustY[i] += dustVY[i];
    if (dustY[i] > SCREEN_HEIGHT) {
      // "искра" улетела вниз за экран — рождаем новую у самой базовой линии
      dustY[i]  = NET_BASELINE + random(0, 6);
      dustX[i]  = random(0, SCREEN_WIDTH);
      dustVY[i] = 0.15 + random(0, 25) / 100.0;
    }
  }
}

// узлы напрямую берут уровень из спектра bandLevel[], через одну полосу —
// 8 узлов покрывают все 16 полос (0, 2, 4, ... 14), поэтому волна реально
// соответствует спектру (реальному, если он сейчас приходит с компьютера)
void updateWaveNodes() {
  for (int i = 0; i < NUM_WAVE_NODES; i++) {
    waveElevation[i] = bandLevel[i * 2];
  }
}

void drawNetworkWave() {
  updateWaveNodes();
  updateDust();

  const int n = NUM_WAVE_NODES;
  int xs[NUM_WAVE_NODES];
  int ys[NUM_WAVE_NODES];

  for (int i = 0; i < n; i++) {
    xs[i] = (int)((float)i * (SCREEN_WIDTH - 1) / (n - 1));
    ys[i] = NET_BASELINE - (int)(waveElevation[i] * NET_AMPLITUDE);
  }

  // вертикальные "столбики" от базовой линии до текущего положения узла
  for (int i = 0; i < n; i++) {
    display.drawFastVLine(xs[i], ys[i], NET_BASELINE - ys[i] + 1, SSD1306_WHITE);
  }

  // NET_SUBLINES тонких линий под основной — те же узлы, но сжатые к базовой линии.
  // Расстояние между ними пропорционально высоте узла — чем выше узел, тем шире зазоры.
  for (int k = 1; k <= NET_SUBLINES; k++) {
    float frac = (float)k / (NET_SUBLINES + 1);
    for (int i = 0; i < n - 1; i++) {
      int y0 = NET_BASELINE - (int)(waveElevation[i]     * NET_AMPLITUDE * frac);
      int y1 = NET_BASELINE - (int)(waveElevation[i + 1] * NET_AMPLITUDE * frac);
      display.drawLine(xs[i], y0, xs[i + 1], y1, SSD1306_WHITE);
    }
  }

  // основная (верхняя) линия — рисуем жирной, дублируя со сдвигом на 1 px
  for (int i = 0; i < n - 1; i++) {
    display.drawLine(xs[i], ys[i],     xs[i + 1], ys[i + 1],     SSD1306_WHITE);
    display.drawLine(xs[i], ys[i] - 1, xs[i + 1], ys[i + 1] - 1, SSD1306_WHITE);
  }

  // яркие узлы поверх линии
  for (int i = 0; i < n; i++) {
    display.fillCircle(xs[i], ys[i], 2, SSD1306_WHITE);
  }

  // осыпающиеся искры под волной
  for (int i = 0; i < NUM_DUST; i++) {
    display.drawPixel((int)dustX[i], (int)dustY[i], SSD1306_WHITE);
  }
}

// ================= Режим 5: волна (по мотивам WAVE) =================
// Без центрального пульсирующего круга — сплошная синусоидальная линия,
// амплитуда которой в каждой точке x берётся из bandLevel[] (интерполяция
// между соседними полосами), плюс наша собственная генерация "музыки".
void drawWaveMode() {
  int prevX = 0;
  int prevY = SCREEN_HEIGHT / 2;

  for (int x = 0; x < SCREEN_WIDTH; x += 2) {
    float pos = x / (float)(SCREEN_WIDTH - 1) * (NUM_BANDS - 1);
    int i = (int)pos;
    float f = pos - i;

    float a = bandLevel[i];
    float b = bandLevel[min(i + 1, NUM_BANDS - 1)];
    float amp = a + (b - a) * f;

    float y = (SCREEN_HEIGHT / 2)
              + sinf(x * 0.23f + millis() * 0.012f) * amp * 17.0f
              + sinf(x * 0.071f - millis() * 0.006f) * amp * 7.0f;

    int iy = constrain((int)y, 4, SCREEN_HEIGHT - 2);

    display.drawLine(prevX, prevY, x, iy, SSD1306_WHITE);

    prevX = x;
    prevY = iy;
  }
}

// ================= Режим 6: точки-танцоры (по мотивам DOT DANCE) =================
// Концепция взята из референса (точки над базовой линией), но вместо
// разрывного "хвоста" из отдельных блоков — сплошная линия от базовой
// линии до текущего положения точки. Темп и уровни — наши собственные.
void drawDotDance() {
  const int bottom = SCREEN_HEIGHT - 2;
  const int colWidth = SCREEN_WIDTH / NUM_BANDS;

  for (int i = 0; i < NUM_BANDS; i++) {
    int x = colWidth / 2 + i * colWidth;
    int y = bottom - (int)(bandLevel[i] * (bottom - 6));

    // сплошная линия-стебель от базовой линии до точки, без разрывов
    display.drawFastVLine(x, y, bottom - y, SSD1306_WHITE);

    // сама "танцующая" точка
    display.fillCircle(x, y, 2, SSD1306_WHITE);
  }

  // базовая линия внизу
  display.drawFastHLine(0, bottom, SCREEN_WIDTH, SSD1306_WHITE);
}

// ============================================================
void loop() {
  checkButton();
  readSerialAudio();
  updateFakeAudio();

  display.clearDisplay();

  switch (mode) {
    case 0: drawClassicBars();     break;
    case 1: drawMirroredBars();    break;
    case 2: drawRadialSpectrum();  break;
    case 3: drawSegmentedMeter();  break;
    case 4: drawNetworkWave();     break;
    case 5: drawWaveMode();        break;
    case 6: drawDotDance();        break;
  }

  display.display();
  delay(30); // ~33 к/с — плавная анимация
}
