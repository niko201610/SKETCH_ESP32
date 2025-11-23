/* Integracion: MiniUI (touch UI) + WebServer / JPG render
   Fusiona:
     - MiniUI_HTTP_Touch_UI_Revamp_Tighter_FixedLayout_v6b.ino
     - ESP32 Render LaTeX + Automatizacion Web/API (JPG upload & show)
   Resultado: UI inicial (Home/Counter/Calibrate). Cuando llega un JPG via /upload
   o se solicita /show?file=... se muestra en pantalla usando la rutina JPEG.
*/

#include <Arduino.h>
#include <SPI.h>

// IMPORTANT: FS/SPIFFS must be included BEFORE WebServer
#include <FS.h>
#include <SPIFFS.h>
typedef fs::FS FS;   // hace visible FS en el scope global para compatibilidad
#include <WebServer.h>


#include <TFT_eSPI.h>
#include <JPEGDecoder.h>
#include <XPT2046_Touchscreen.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ESPmDNS.h>


// -------- CONFIG --------
const char* WIFI_SSID = "Nicolas";
const char* WIFI_PASS = "nico2016";

// POST endpoints used earlier
const char* URL_RUN_POST     = "https://selena-soutenu-guy.ngrok-free.dev/run";
// AutoRemote personal URL (tuya)
const char* URL_POST_TRIGGER = "https://autoremotejoaomgcd.appspot.com/MJ35kx5K";



// -------- TOUCH PINS (match your hardware) --------
#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

SPIClass mySpi = SPIClass(VSPI);
XPT2046_Touchscreen ts(XPT2046_CS, XPT2046_IRQ);
TFT_eSPI tft = TFT_eSPI();
Preferences prefs;
WebServer server(80);
int currentPageIndex = 1;

// -------- CASIO-inspired theme (tweakable) --------
// LCD-like palette and spacing for quick styling updates.
const uint16_t COLOR_BG         = TFT_BLACK;                      // main background
const uint16_t COLOR_GRID       = TFT_DARKGREY;                   // separators / matrix feel
const uint16_t COLOR_TEXT_MAIN  = TFT_GREEN;                      // primary mono-like text
const uint16_t COLOR_TEXT_SOFT  = tft.color565(120, 210, 180);    // secondary text
const uint16_t COLOR_ACCENT     = tft.color565(0, 170, 120);      // buttons / highlights
const uint16_t COLOR_ACCENT_2   = tft.color565(0, 120, 180);      // alternate accent
const uint16_t COLOR_FRAME      = tft.color565(30, 60, 60);       // subtle frame lines
const uint16_t COLOR_BUTTON_BG  = tft.color565(10, 30, 30);
const uint16_t COLOR_STATUS_BG  = tft.color565(8, 16, 16);
const int THEME_RADIUS = 6;
const int THEME_PADDING = 6;


// virtual canvas
const int V_W = 320;
const int V_H = 115;
int displayDefaultY = -70; // solicitado
int displayDefaultX = 0;
int displayBaseMode = 0; // 0=center, 1=top-left

// escala hardware <- virtual (calculadas en setup)
float scaleX = 1.0f, scaleY = 1.0f;

// Mapping lineal: REAL = scale * RAW + offset
float map_scale_x = (float)320.0f / 4096.0f;
float map_offset_x = 0.0f;
float map_scale_y = (float)115.0f / 4096.0f;
float map_offset_y = 0.0f;
bool calibrated = false;

enum Screen { HOME, COUNTER, IMAGE_VIEWER };
Screen screen = HOME;
int counterValue = 0;

// --- Long-press progress globals (insertar una vez con otros globals) ---
float _lp_lastFrac = -1.0;          // fraccion previa para evitar redibujos innecesarios
int   _lp_px = 0, _lp_py = 0;       // area actual del progress (para borrar)
int   _lp_pw = 0, _lp_ph = 0;
uint16_t _lp_bgColor = TFT_BLACK;   // fondo (ajusta si tu fondo no es negro)
uint16_t _lp_barBgColor = 0;        // inicializados en setup si quieres
uint16_t _lp_barFgColor = 0;
unsigned long _touch_lastSeenMs = 0;
const unsigned long TOUCH_HYST_MS = 120;      // mayor histéresis, evita "touch perdido" por breves caídas. :contentReference[oaicite:8]{index=8}
const int TOUCH_SAMPLE_COUNT = 7;             // más muestras para promedio/mediana -> menos ruido. :contentReference[oaicite:9]{index=9}
const int TOUCH_SAMPLE_DELAY_MS = 8;          // pequeño retardo entre samples
int  _touch_lastX = 0, _touch_lastY = 0;   // ultima coordenada valida
float _touch_x_ema = 0.0, _touch_y_ema = 0.0; // EMA smoothing
const float TOUCH_EMA_ALPHA = 0.45;        // 0..1 (menor valor -> mas suave)
int touchPressureThreshold = 0; // si tu TS devuelve z, puedes poner >0 para filtrar; 0 = ignorar
// --- end globals ---

// --- fin globals ---

struct Rect { int x, y, w, h; };
Rect btnRun, btnCounter, btnPlus, btnMinus, btnNumber, btnCalibrate; // --- Pegar UNA SOLA VEZ junto a las otras declaraciones Rect (btnPlus/btnMinus/btnRun etc.) ---
Rect btnLeftZone;
Rect btnRightZone;
Rect btnImageMode;
Rect btnImgBack, btnImgReset;

// ---- Control de bloqueo / desbloqueo y long-press para volver a HOME ----
bool homeButtonsLocked = false; // Ya no usamos "mantener presionado" para desbloquear
const unsigned long HOME_UNLOCK_LONGPRESS_MS = 800; // tiempo para mantener presionado y "desbloquear" HOME
unsigned long homeUnlockPressMs = 0;

// --- Hit-area tuning ---
const int HOME_HIT_INSET = 6;     // px a recortar dentro del rect de RUN/Contador para que haya menos "aciertos" accidentales
const int COUNTER_HIT_INSET = 4;  // px inset para botones + / - / number
const int RELEASE_CONFIRM_COUNT = 3;              // numero de muestras consecutivas sin touch para considerar liberacion
const int TOUCH_PAD_PIXELS = 8;                   // padding extra para hit tests tolerantes

// (ya existian: SIDE_SEND_DEBOUNCE_MS, lastSideSendMillis) -- si no existen, asegurarse que estan definidas
// const unsigned long SIDE_SEND_DEBOUNCE_MS = 600;
// unsigned long lastSideSendMillis = 0;

int activeButtonId = -1;
Rect activeButtonRect;
unsigned long activePressMs = 0;
const unsigned long TAP_MAX_DURATION_MS = 700;
const int TAP_PAD_PIXELS = 10;

// --- Debounce para las acciones de las zonas laterales ---
const unsigned long SIDE_SEND_DEBOUNCE_MS = 600;
unsigned long lastSideSendMillis = 0;



// store colors so redraw knows how to paint the button exactly
uint16_t colRun, colCounter, colCalibrate, colPlus, colMinus, colNumberBg;

// debug cursor disabled to avoid intrusive painting (prevents flicker)
bool showTouchCursor = false;
int lastCursorX = -1, lastCursorY = -1;
unsigned long lastCursorAt = 0;
const unsigned long CURSOR_TTL = 450; // ms

// long-press detection (raw)
const unsigned long LONG_PRESS_MS = 1500;

// Image viewer state
String currentImagePath = "/page_001.jpg"; // default fallback
int lastImgW = 0, lastImgH = 0;
int imgOffsetX = 0, imgOffsetY = 0;
unsigned long lastPanRedrawMs = 0;
bool isPanningImage = false;
int panStartX = 0, panStartY = 0;
int panLastX = 0, panLastY = 0;

// ---------- helpers virtual -> real ----------
int vX(int vx) {
  int rx = (int)round(vx * scaleX);
  rx = constrain(rx, 0, tft.width() - 1);
  return rx;
}
int vY(int vy) {
  float vy_offset = (float)(vy + displayDefaultY);
  int ry = (int)round(vy_offset * scaleY);
  ry = constrain(ry, 0, tft.height() - 1);
  return ry;
}
Rect vRectMin(int vx, int vy, int vw, int vh, int minPhysPx = 56) {
  int rx = vX(vx);
  int ry = vY(vy);
  int rw = max(minPhysPx, (int)round(vw * scaleX));
  int rh = max(minPhysPx, (int)round(vh * scaleY));
  if (rx + rw > tft.width()) rw = tft.width() - rx;
  if (ry + rh > tft.height()) rh = tft.height() - ry;
  if (rw < 1) rw = 1;
  if (rh < 1) rh = 1;
  return { rx, ry, rw, rh };
}
Rect vRect(int vx, int vy, int vw, int vh) { return vRectMin(vx, vy, vw, vh, 40); }

// ---------- utilidades ----------
int medianFilter(int *arr, int n) {
  int tmp[30];
  if (n > 30) n = 30;
  for (int i=0;i<n;i++) tmp[i]=arr[i];
  for (int i=1;i<n;i++) {
    int key = tmp[i];
    int j = i-1;
    while (j>=0 && tmp[j] > key) { tmp[j+1] = tmp[j]; j--; }
    tmp[j+1] = key;
  }
  return tmp[n/2];
}

// save/load mapping
void saveMapping() {
  prefs.begin("touchcal", false);
  prefs.putBool("calibrated", true);
  prefs.putFloat("sx", map_scale_x);
  prefs.putFloat("ox", map_offset_x);
  prefs.putFloat("sy", map_scale_y);
  prefs.putFloat("oy", map_offset_y);
  prefs.end();
}
void loadMapping() {
  prefs.begin("touchcal", true);
  calibrated = prefs.getBool("calibrated", false);
  if (calibrated) {
    map_scale_x = prefs.getFloat("sx", map_scale_x);
    map_offset_x = prefs.getFloat("ox", map_offset_x);
    map_scale_y = prefs.getFloat("sy", map_scale_y);
    map_offset_y = prefs.getFloat("oy", map_offset_y);
    Serial.printf("Mapping cargado: sx=%f ox=%f sy=%f oy=%f\n", map_scale_x, map_offset_x, map_scale_y, map_offset_y);
  } else {
    Serial.println("Sin calibración previa");
  }
  prefs.end();
}

bool validateMapping() {
  if (!calibrated) return false;
  if (!isfinite(map_scale_x) || !isfinite(map_scale_y)) return false;
  if (map_scale_x < 0.0001f || map_scale_x > 1.0f) return false;
  if (map_scale_y < 0.0001f || map_scale_y > 1.0f) return false;
  int testRx = 2048, testRy = 2048;
  int sx = constrain((int)round(map_scale_x * testRx + map_offset_x), 0, tft.width()-1);
  int sy = constrain((int)round(map_scale_y * testRy + map_offset_y), 0, tft.height()-1);
  if (sx <= 1 || sx >= tft.width()-1) return false;
  if (sy <= 1 || sy >= tft.height()-1) return false;
  return true;
}

bool computeLinearFromCalibration(const float rawX[3], const float rawY[3], const float scrX[3], const float scrY[3]) {
  float minRawX = rawX[0], maxRawX = rawX[0], minRawY = rawY[0], maxRawY = rawY[0];
  float minScrX = scrX[0], maxScrX = scrX[0], minScrY = scrY[0], maxScrY = scrY[0];
  for (int i=1;i<3;i++) {
    if (rawX[i] < minRawX) minRawX = rawX[i];
    if (rawX[i] > maxRawX) maxRawX = rawX[i];
    if (rawY[i] < minRawY) minRawY = rawY[i];
    if (rawY[i] > maxRawY) maxRawY = rawY[i];
    if (scrX[i] < minScrX) minScrX = scrX[i];
    if (scrX[i] > maxScrX) maxScrX = scrX[i];
    if (scrY[i] < minScrY) minScrY = scrY[i];
    if (scrY[i] > maxScrY) maxScrY = scrY[i];
  }
  float rawWidthX = maxRawX - minRawX; if (rawWidthX < 1.0f) rawWidthX = 1.0f;
  float rawWidthY = maxRawY - minRawY; if (rawWidthY < 1.0f) rawWidthY = 1.0f;
  float sx = (maxScrX - minScrX) / rawWidthX;
  float sy = (maxScrY - minScrY) / rawWidthY;
  if (!isfinite(sx) || fabs(sx) < 1e-6) sx = (float)tft.width() / 4096.0f;
  if (!isfinite(sy) || fabs(sy) < 1e-6) sy = (float)tft.height() / 4096.0f;
  float ox = minScrX - sx * minRawX;
  float oy = minScrY - sy * minRawY;
  float maxErr = 0;
  for (int i=0;i<3;i++) {
    float mx = sx * rawX[i] + ox;
    float my = sy * rawY[i] + oy;
    float err = sqrt((mx - scrX[i])*(mx - scrX[i]) + (my - scrY[i])*(my - scrY[i]));
    if (err > maxErr) maxErr = err;
    Serial.printf("LIN VALID P%d raw=(%d,%d) -> mapped=(%.1f,%.1f) target=(%.1f,%.1f) err=%.2f\n",
      i, (int)rawX[i], (int)rawY[i], mx, my, scrX[i], scrY[i], err);
  }
  if (maxErr > 24.0f) {
    Serial.printf("Linear mapping failed validation (maxErr=%.2f)\n", maxErr);
    return false;
  }
  map_scale_x = sx; map_offset_x = ox;
  map_scale_y = sy; map_offset_y = oy;
  Serial.printf("Linear mapping SET sx=%.6f ox=%.2f sy=%.6f oy=%.2f (maxErr=%.2f)\n", sx, ox, sy, oy, maxErr);
  return true;
}

// calibration helpers (unchanged behaviour)
void drawCalibrationTarget(int vx, int vy) {
  int x = vX(vx);
  int y = vY(vy);
  int r = max(3, (int)round(3*scaleX));
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(1);
  tft.drawCentreString("Toca el punto grande", tft.width()/2, 6, 1);
  tft.fillCircle(x, y, r+6, TFT_WHITE);
  tft.drawCircle(x, y, r+8, TFT_WHITE);
  tft.fillCircle(x, y, r, TFT_RED);
}
bool waitForTouchSamples(int samples, int *outX, int *outY, unsigned long timeoutMs=4000) {
  unsigned long start = millis();
  int i = 0;
  while (i < samples && millis() - start < timeoutMs) {
    if (ts.touched()) {
      TS_Point p = ts.getPoint();
      outX[i] = p.x; outY[i] = p.y; i++;
      delay(80);
      while (ts.touched()) delay(6);
      delay(20);
    } else delay(6);
  }
  return (i == samples);
}
void doCalibrationInteractive() {
  const int MAX_ATTEMPTS_PER_POINT = 3;
  const int ptsVx[3] = { 30, V_W/2, V_W-30 };
  const int ptsVy[3] = { 30, V_H/2, V_H-30 };
  float rawX[3], rawY[3], scrX[3], scrY[3];
  while (ts.touched()) { TS_Point p = ts.getPoint(); (void)p; delay(6); }
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);
  tft.drawCentreString("Calibración táctil", tft.width()/2, 6, 1);
  delay(400);
  for (int i=0;i<3;i++) {
    bool pointOK = false;
    for (int attempt=1; attempt<=MAX_ATTEMPTS_PER_POINT; attempt++) {
      drawCalibrationTarget(ptsVx[i], ptsVy[i]);
      Serial.printf("Cal: P%d intento %d\n", i, attempt);
      const int SAMPLES = 7;
      int sx[SAMPLES], sy[SAMPLES];
      bool ok = waitForTouchSamples(SAMPLES, sx, sy, 6000);
      if (!ok) {
        Serial.printf("No touch detectado para punto %d intento %d\n", i, attempt);
        tft.fillScreen(TFT_BLACK);
        tft.drawCentreString("No se detecto toque. Reintentando...", tft.width()/2, tft.height()/2 - 6, 1);
        delay(700);
        while (ts.touched()) { TS_Point p = ts.getPoint(); (void)p; delay(6); }
        continue;
      }
      int medx = medianFilter(sx, SAMPLES);
      int medy = medianFilter(sy, SAMPLES);
      rawX[i] = medx; rawY[i] = medy;
      scrX[i] = (float)vX(ptsVx[i]); scrY[i] = (float)vY(ptsVy[i]);
      tft.fillCircle((int)scrX[i], (int)scrY[i], (int)round(3*scaleX), TFT_GREEN);
      delay(250);
      Serial.printf("CAL SAMP P%d raw=(%d,%d) scr=(%d,%d)\n", i, (int)rawX[i], (int)rawY[i], (int)scrX[i], (int)scrY[i]);
      pointOK = true;
      break;
    }
    if (!pointOK) {
      Serial.printf("Punto %d fallo tras %d intentos -> abortando calibracion y aplicando fallback\n", i, MAX_ATTEMPTS_PER_POINT);
      map_scale_x = (float)tft.width() / 4096.0f; map_offset_x = 0.0f;
      map_scale_y = (float)tft.height() / 4096.0f; map_offset_y = 0.0f;
      calibrated = true;
      saveMapping();
      tft.fillScreen(TFT_BLACK);
      tft.setTextSize(1); tft.setTextColor(TFT_WHITE);
      tft.drawCentreString("Calibracion abortada (fallback)", tft.width()/2, tft.height()/2 - 6, 1);
      delay(800);
      return;
    }
  }
  bool ok = computeLinearFromCalibration(rawX, rawY, scrX, scrY);
  if (!ok) {
    Serial.println("Validacion lineal fallo -> aplicando fallback trivial");
    map_scale_x = (float)tft.width() / 4096.0f; map_offset_x = 0.0f;
    map_scale_y = (float)tft.height() / 4096.0f; map_offset_y = 0.0f;
  }
  calibrated = true;
  saveMapping();
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(1); tft.setTextColor(TFT_WHITE);
  tft.drawCentreString("Calibracion guardada", tft.width()/2, tft.height()/2 - 6, 1);
  delay(700);
}
// --- Touch helpers: lectura filtrada y tolerante ---
// devuelve true si hay "touch considerado activo" y devuelve coordenadas mapeadas (sx,sy)
bool readFilteredTouch(int &sx_out, int &sy_out) {
  // Primer intento: tomar una muestra
  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    // si tu controlador tiene 'z' (presion), puedes comprobarlo:
    #ifdef TS_PRESSURE_FIELD
    if (p.z && touchPressureThreshold > 0 && p.z < touchPressureThreshold) {
      // presión insuficiente, ignorar este sample
    } else {
      // mapeo y actualizar filtros
    #endif
      int sx, sy;
      mapRawToScreen(p.x, p.y, sx, sy);

      // actualizar EMA
      if (_touch_x_ema == 0.0f && _touch_y_ema == 0.0f) {
        _touch_x_ema = sx;
        _touch_y_ema = sy;
      } else {
        _touch_x_ema = TOUCH_EMA_ALPHA * sx + (1.0f - TOUCH_EMA_ALPHA) * _touch_x_ema;
        _touch_y_ema = TOUCH_EMA_ALPHA * sy + (1.0f - TOUCH_EMA_ALPHA) * _touch_y_ema;
      }
      _touch_lastX = (int)(_touch_x_ema + 0.5f);
      _touch_lastY = (int)(_touch_y_ema + 0.5f);
      _touch_lastSeenMs = millis();
      sx_out = _touch_lastX; sy_out = _touch_lastY;
      return true;
    #ifdef TS_PRESSURE_FIELD
    }
    #endif
  }

  // Si no hay sample actual, pero hubo un touch hace poco, consideramos que sigue tocado
  if ((millis() - _touch_lastSeenMs) <= TOUCH_HYST_MS) {
    sx_out = _touch_lastX;
    sy_out = _touch_lastY;
    return true;
  }

  // Intentar muestrear unos cuantos valores para detectar toques "flojos"
  int samples = 0;
  int sx_acc = 0, sy_acc = 0;
  for (int i = 0; i < TOUCH_SAMPLE_COUNT; ++i) {
    if (ts.touched()) {
      TS_Point p2 = ts.getPoint();
      int sx2, sy2; mapRawToScreen(p2.x, p2.y, sx2, sy2);
      sx_acc += sx2; sy_acc += sy2; samples++;
    }
    delay(TOUCH_SAMPLE_DELAY_MS);
  }
  if (samples > 0) {
    int sxm = sx_acc / samples;
    int sym = sy_acc / samples;
    // actualizar EMA con el promedio
    if (_touch_x_ema == 0.0f && _touch_y_ema == 0.0f) {
      _touch_x_ema = sxm; _touch_y_ema = sym;
    } else {
      _touch_x_ema = TOUCH_EMA_ALPHA * sxm + (1.0f - TOUCH_EMA_ALPHA) * _touch_x_ema;
      _touch_y_ema = TOUCH_EMA_ALPHA * sym + (1.0f - TOUCH_EMA_ALPHA) * _touch_y_ema;
    }
    _touch_lastX = (int)(_touch_x_ema + 0.5f);
    _touch_lastY = (int)(_touch_y_ema + 0.5f);
    _touch_lastSeenMs = millis();
    sx_out = _touch_lastX; sy_out = _touch_lastY;
    return true;
  }

  // no touch detectado
  return false;
}
// raw->real mapping (no intrusive cursor)
void mapRawToScreen(int rx, int ry, int &sx, int &sy) {
  float fx = map_scale_x * (float)rx + map_offset_x;
  float fy = map_scale_y * (float)ry + map_offset_y;
  if (!isfinite(fx) || !isfinite(fy)) {
    sx = constrain((int)round((float)rx / 4096.0f * tft.width()), 0, tft.width()-1);
    sy = constrain((int)round((float)ry / 4096.0f * tft.height()), 0, tft.height()-1);
    //Serial.printf("MAP INVALID -> trivial(%d,%d)\n", sx, sy);
    return;
  }
  sx = constrain((int)round(fx), 0, tft.width()-1);
  sy = constrain((int)round(fy), 0, tft.height()-1);
  //Serial.printf("MAP raw(%d,%d) -> real(%d,%d)\n", rx, ry, sx, sy);
}

// ---------- TEXT FIT helper ----------
int chooseTextSizeToFit(const char* label, Rect r, int maxSize = 4, int margin = 8) {
  if (label == NULL) return 1;
  int baseW = tft.textWidth(label, 1);
  if (baseW <= 0) baseW = strlen(label) * 6; // fallback
  for (int tsz = maxSize; tsz >= 1; tsz--) {
    long scaledW = (long)baseW * tsz;
    if ((long)r.w - margin >= scaledW) return tsz;
  }
  return 1;
}

// shrink a rect from all sides by 'inset' pixels. If inset too grande, returns empty rect.
Rect insetRect(Rect r, int inset) {
  if (r.w <= 0 || r.h <= 0) return {0,0,0,0};
  int nx = r.x + inset;
  int ny = r.y + inset;
  int nw = r.w - inset * 2;
  int nh = r.h - inset * 2;
  if (nw <= 0 || nh <= 0) return {0,0,0,0};
  return { nx, ny, nw, nh };
}

// hit test against the inset rect
bool ptInRectInner(int x, int y, Rect r, int inset) {
  Rect ri = insetRect(r, inset);
  if (ri.w == 0 || ri.h == 0) return false;
  return ptInRect(x, y, ri);
}

// ---------- UI helpers (minimalist) ----------
bool ptInRect(int x, int y, Rect r) {
  // use half-open interval to avoid off-by-one issues
  return x >= r.x && x < (r.x + r.w) && y >= r.y && y < (r.y + r.h);
}
// hit test tolerante con padding
bool ptInRectPad(int x, int y, Rect r, int pad) {
  return x >= (r.x - pad) && x < (r.x + r.w + pad)
      && y >= (r.y - pad) && y < (r.y + r.h + pad);
}

uint16_t blendColor(uint16_t c, int delta) {
  int r = ((c >> 11) & 0x1F) << 3;
  int g = ((c >> 5) & 0x3F) << 2;
  int b = (c & 0x1F) << 3;
  r = constrain(r + delta, 0, 255);
  g = constrain(g + delta, 0, 255);
  b = constrain(b + delta, 0, 255);
  return tft.color565(r, g, b);
}

void drawMiniSpinner(int cx, int cy, int radius, int step) {
  const int segs = 8;
  tft.fillCircle(cx, cy, radius + 2, COLOR_BG);
  for (int i = 0; i < segs; ++i) {
    float ang = (float)(i + step) / segs * TWO_PI;
    int x0 = cx + cos(ang) * (radius - 4);
    int y0 = cy + sin(ang) * (radius - 4);
    int x1 = cx + cos(ang) * radius;
    int y1 = cy + sin(ang) * radius;
    uint16_t c = (i == (step % segs)) ? COLOR_ACCENT : COLOR_FRAME;
    tft.drawLine(x0, y0, x1, y1, c);
  }
}

void drawCasioFrame(Rect area, const char* title = NULL) {
  tft.fillRect(area.x, area.y, area.w, area.h, COLOR_BG);
  tft.drawRect(area.x, area.y, area.w, area.h, COLOR_FRAME);
  for (int y = area.y + 18; y < area.y + area.h; y += 18) {
    tft.drawLine(area.x + 1, y, area.x + area.w - 2, y, COLOR_GRID);
  }
  if (title) {
    tft.setTextColor(COLOR_TEXT_SOFT, COLOR_BG);
    tft.setTextSize(1);
    tft.drawCentreString(title, area.x + area.w / 2, area.y + 2, 1);
  }
}

void drawStyledButton(Rect r, const char* label, int radius, uint16_t bgColor, uint16_t textColor) {
  tft.fillRoundRect(r.x, r.y, r.w, r.h, radius, bgColor);
  tft.drawRoundRect(r.x, r.y, r.w, r.h, radius, COLOR_FRAME);
  int tsz = chooseTextSizeToFit(label, r, 3, 8);
  tft.setTextColor(textColor, bgColor);
  tft.setTextSize(tsz);
  int yoff = (tsz >= 2) ? 6 : 4;
  tft.drawCentreString(label, r.x + r.w/2, r.y + r.h/2 - yoff, 1);
}
void drawCircleButton(Rect r, const char* label, uint16_t bgColor) {
  int cx = r.x + r.w/2;
  int cy = r.y + r.h/2;
  int rad = min(r.w, r.h)/2 - 2;
  if (rad < 8) rad = min(r.w, r.h)/2;
  tft.fillCircle(cx, cy, rad, bgColor);
  tft.drawCircle(cx, cy, rad, tft.color565(255,255,255));
  Rect fake = { r.x, r.y, rad*2, rad*2 };
  int tsz = chooseTextSizeToFit(label, fake, 2, 6);
  tft.setTextColor(TFT_WHITE, bgColor);
  tft.setTextSize(tsz);
  tft.drawCentreString(label, cx, cy - 6, 1);
}
void drawPillButton(Rect r, const char* label, uint16_t bgColor, uint16_t textColor) {
  int radius = r.h/2;
  tft.fillRoundRect(r.x, r.y, r.w, r.h, radius, bgColor);
  tft.drawRoundRect(r.x, r.y, r.w, r.h, radius, tft.color565(255,255,255));
  int tsz = chooseTextSizeToFit(label, r, 3, 8);
  tft.setTextColor(textColor, bgColor);
  tft.setTextSize(tsz);
  int yoff = (tsz>=2) ? 6 : 4;
  tft.drawCentreString(label, r.x + r.w/2, r.y + r.h/2 - yoff, 1);
}

// redraw a single button using the stored color - avoids full-screen redraws
void redrawButton(Rect r, const char* label, uint16_t color) {
  // Heuristica para elegir estilo segun tamano
  if (r.w >= 90 && r.h >= 22) {
    drawPillButton(r, label, color, TFT_WHITE);
  } else if (r.w >= 40 && r.h >= 40) {
    drawCircleButton(r, label, color);
  } else {
    drawStyledButton(r, label, 6, color, TFT_WHITE);
  }
}

// flash feedback local and shape-correct:
void flashButtonLocal(Rect r, const char* label, uint16_t origColor) {
  bool isCircle = (r.w >= 40 && r.h >= 40);
  if (isCircle) {
    int cx = r.x + r.w/2;
    int cy = r.y + r.h/2;
    int rad = min(r.w, r.h)/2 - 2;
    if (rad < 8) rad = min(r.w, r.h)/2;
    uint16_t hl = blendColor(origColor, 40);
    tft.fillCircle(cx, cy, rad, hl);
    tft.setTextSize(1);
    tft.setTextColor(TFT_BLACK, hl);
    tft.drawCentreString(label, cx, cy - 6, 1);
    delay(40);
    drawCircleButton(r, label, origColor);
  } else {
    int radius = min(12, r.h/2);
    uint16_t hl = blendColor(origColor, 40);
    tft.fillRoundRect(r.x, r.y, r.w, r.h, radius, hl);
    tft.setTextSize(1);
    tft.setTextColor(TFT_BLACK, hl);
    tft.drawCentreString(label, r.x + r.w/2, r.y + r.h/2 - 4, 1);
    delay(40);
    redrawButton(r, label, origColor);
  }
}
// Prototipos: asegurarnos de que podemos llamar a los handlers desde cualquier sitio
void handleNext();   // handler que ya registraste con server.on("/next", ...)
void handlePrev();   // handler que ya registraste con server.on("/prev", ...)


void connectWiFi() {
  tft.fillScreen(COLOR_BG);
  tft.setTextColor(COLOR_TEXT_MAIN); tft.setTextSize(1);
  tft.drawCentreString("Conectando WiFi...", tft.width()/2, tft.height()/2 - 10, 1);
  WiFi.mode(WIFI_STA); WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  int step = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - start < 12000) {
    delay(250); Serial.print("."); server.handleClient();
    drawMiniSpinner(tft.width()/2, tft.height()/2 + 12, 12, step++);
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi conectado!");
    tft.fillScreen(COLOR_BG);
    tft.drawCentreString("WiFi conectado!", tft.width()/2, tft.height()/2 - 10, 1);
        // justo después de WiFi conectado:
    sendIpToAutoRemote();   // envía la IP a MacroDroid/AutoRemote al conectarse

  } else {
    Serial.println("WiFi fallo");
    tft.fillScreen(COLOR_BG);
    tft.drawCentreString("WiFi fallo", tft.width()/2, tft.height()/2 - 10, 1);
  }
  delay(600);
}


// drawHome con estilo Casio
void drawHome() {
  screen = HOME;
  tft.fillScreen(COLOR_BG);

  Rect titleArea = {0, 0, tft.width(), 26};
  drawCasioFrame(titleArea, "HOME");
  tft.setTextColor(COLOR_TEXT_SOFT, COLOR_BG);
  tft.setTextSize(1);
  tft.drawString("MODE", THEME_PADDING, 4);
  tft.setTextColor(COLOR_TEXT_MAIN, COLOR_BG);
  tft.drawRightString(WiFi.localIP().toString(), tft.width() - THEME_PADDING, 4, 1);

  Rect gridArea = {THEME_PADDING, titleArea.h + 4, tft.width() - THEME_PADDING * 2, tft.height() - titleArea.h - 16};
  drawCasioFrame(gridArea, "SELECT");

  redrawButton(btnRun, "PC", COLOR_ACCENT);
  redrawButton(btnCounter, "PREG", COLOR_ACCENT_2);
  redrawButton(btnImageMode, "IMG", COLOR_BUTTON_BG);

  tft.setTextSize(1);
  tft.setTextColor(COLOR_TEXT_SOFT, COLOR_BG);
  tft.drawString("Tap to enter mode", THEME_PADDING + 2, gridArea.y + gridArea.h - 14, 1);
}


// --- Long-press progress drawing helpers ---
void initLongPressColors() {
  // llama esto desde setup() o al principio si quieres configurar colores dinámicamente
  _lp_barBgColor = tft.color565(30, 30, 30);     // fondo del track (track background)
  _lp_barFgColor = tft.color565(0, 160, 80);     // color de la barra (foreground)
  _lp_bgColor    = getBackgroundColorForScreen(); // inicializamos con el color actual de pantalla
  _lp_lastFrac = -1.0;
  _lp_px = _lp_py = _lp_pw = _lp_ph = 0;
}

void drawLongPressProgressAt(int sx, int sy, float frac) {
  // frac en [0..1]
  if (frac < 0) frac = 0;
  if (frac > 1) frac = 1;

  // posiciona la barra un poco por encima del dedo (para no tapar el punto de toque)
  int w = 120; int h = 14;
  int px = sx;
  int py = sy - 48; // ajusta si quieres mas/menos separacion del punto de toque
  if (py < 10) py = sy + 48; // si queda muy arriba, dibujar abajo del toque

  int x = px - w/2;
  int y = py - h/2;

  // si la posicion cambio (por ejemplo moviste el dedo) debemos limpiar la antigua area con el fondo correcto
  uint16_t bgNow = getBackgroundColorForScreen();
  if (_lp_pw > 0 && _lp_ph > 0 && (_lp_px != x || _lp_py != y || bgNow != _lp_bgColor)) {
    // limpiar la antigua area con el color de fondo que habia en aquel momento
    tft.fillRect(_lp_px, _lp_py, _lp_pw, _lp_ph, _lp_bgColor);
    // Si prefieres redibujar la zona subyacente con elementos (en vez de fillRect), lo puedes hacer
    // aqui llamando a la funcion correspondiente (ver clearLongPressProgress).
  }

  // Solo redibujar si la fraccion cambio lo suficiente (reduciendo parpadeo)
  if (fabs(frac - _lp_lastFrac) < 0.02 && _lp_px == x && _lp_py == y) return;

  // actualizar color de borrado actual
  _lp_bgColor = bgNow;

  // fondo del track (track background)
  tft.fillRoundRect(x, y, w, h, 6, _lp_barBgColor);
  tft.drawRoundRect(x, y, w, h, 6, TFT_WHITE);

  // fill proportional (pequeño padding interior)
  int innerX = x + 3;
  int innerY = y + 3;
  int innerW = w - 6;
  int innerH = h - 6;
  int fw = max(2, (int)(innerW * frac));
  // dibuja el fragmento de progreso
  tft.fillRoundRect(innerX, innerY, fw, innerH, 4, _lp_barFgColor);

  // opcional: porcentaje dentro de la barra (pequeño texto)
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);
  tft.setTextDatum(MC_DATUM);
  char pct[8]; snprintf(pct, sizeof(pct), "%d%%", (int)(frac*100));
  tft.drawCentreString(String(pct), x + w/2, y + (h/2) - 4, 1);

  // guarda info para borrado/chequeo
  _lp_px = x; _lp_py = y; _lp_pw = w; _lp_ph = h;
  _lp_lastFrac = frac;
}

uint16_t getBackgroundColorForScreen() {
  // devuelve un color razonable para borrar la barra dependiendo de la pantalla
  // Ajusta estos colores segun tu paleta y otras pantallas que tengas.
  if (screen == HOME) return COLOR_BG;           // ajusta si tu HOME tiene otro fondo
  if (screen == COUNTER) return COLOR_STATUS_BG;      // usa color de la caja numerica
  if (screen == IMAGE_VIEWER) return COLOR_BG;
  // Ejemplo si tienes una pantalla RENDER con fondo claro:
  // if (screen == RENDER) return tft.color565(245,245,245);
  // Añade condiciones para otras pantallas con fondos especificos

  // fallback:
  return TFT_BLACK;
}

void clearLongPressProgress() {
  // en lugar de rellenar con un color fijo, usamos el color adecuado para la pantalla actual
  uint16_t bg = getBackgroundColorForScreen();
  if (_lp_pw > 0 && _lp_ph > 0) {
    // llenar con el color de fondo detectado (limpia residuos)
    tft.fillRect(_lp_px, _lp_py, _lp_pw, _lp_ph, bg);

    // Para mayor robustez, redibujamos la UI que estaba debajo de la barra.
    // Esto evita "barritas" negras si la UI tiene elementos complejos.
    if (screen == HOME) {
      // redibujar HOME completo (si es barato en tu caso).
      drawHome();
    } else if (screen == COUNTER) {
      // redibujar la pantalla COUNTER completa
      drawCounterFull();
    } else {
      // Si tienes una funcion que redibuja la pagina/render actual, llama aqui:
      // (Descomenta y ajusta el nombre si existe en tu sketch)
      // drawRenderPage(currentRenderIndex);
      // Si no existe, puedes redibujar un background o no hacer nada.
    }
  }
  // reset de estado
  _lp_lastFrac = -1.0;
  _lp_px = _lp_py = _lp_pw = _lp_ph = 0;
}

// DRAW COUNTER FULL (una sola vez cuando entramos)
void updateNumberDisplay(); // forward

void drawCounterFull() {
  screen = COUNTER;
  tft.fillScreen(COLOR_BG);

  Rect titleArea = {0, 0, tft.width(), 26};
  drawCasioFrame(titleArea, "COUNTER");
  tft.setTextColor(COLOR_TEXT_SOFT, COLOR_BG);
  tft.setTextSize(1);
  tft.drawString("CONT", THEME_PADDING, 4);

  redrawButton(btnMinus, "-", COLOR_BUTTON_BG);
  redrawButton(btnPlus, "+", COLOR_BUTTON_BG);

  tft.fillRoundRect(btnNumber.x, btnNumber.y, btnNumber.w, btnNumber.h, THEME_RADIUS, COLOR_STATUS_BG);
  tft.drawRoundRect(btnNumber.x, btnNumber.y, btnNumber.w, btnNumber.h, THEME_RADIUS, COLOR_FRAME);

  updateNumberDisplay();
}

// ACTUALIZA SOLO LA CAJA DEL NUMERO (evita redraw completo)
void updateNumberDisplay() {
  tft.fillRoundRect(btnNumber.x + 1, btnNumber.y + 1, btnNumber.w - 2, btnNumber.h - 2, THEME_RADIUS, COLOR_STATUS_BG);
  char numbuf[32];
  snprintf(numbuf, sizeof(numbuf), "%d", counterValue);
  int tsz = chooseTextSizeToFit(numbuf, btnNumber, 4, 12);
  tft.setTextSize(tsz);
  tft.setTextColor(COLOR_TEXT_MAIN, COLOR_STATUS_BG);
  int cx = btnNumber.x + btnNumber.w/2;
  int cy = btnNumber.y + btnNumber.h/2;
  int approxCharHeight = tsz * 8;
  int yOff = (approxCharHeight / 2) - 2;
  tft.drawCentreString(String(counterValue), cx, cy - yOff, 1);
}

// IMAGE VIEWER UI
void drawImageViewer(bool showHint = true) {
  screen = IMAGE_VIEWER;
  clampImageOffsets();
  displayJpgFile(currentImagePath.c_str(), imgOffsetX, imgOffsetY, false);

  // overlay UI
  tft.fillRoundRect(btnImgBack.x, btnImgBack.y, btnImgBack.w, btnImgBack.h, THEME_RADIUS, COLOR_BUTTON_BG);
  tft.drawRoundRect(btnImgBack.x, btnImgBack.y, btnImgBack.w, btnImgBack.h, THEME_RADIUS, COLOR_FRAME);
  tft.setTextColor(COLOR_TEXT_SOFT, COLOR_BUTTON_BG); tft.setTextSize(1);
  tft.drawCentreString("HOME", btnImgBack.x + btnImgBack.w/2, btnImgBack.y + 6, 1);

  tft.fillRoundRect(btnImgReset.x, btnImgReset.y, btnImgReset.w, btnImgReset.h, THEME_RADIUS, COLOR_BUTTON_BG);
  tft.drawRoundRect(btnImgReset.x, btnImgReset.y, btnImgReset.w, btnImgReset.h, THEME_RADIUS, COLOR_FRAME);
  tft.setTextColor(COLOR_TEXT_SOFT, COLOR_BUTTON_BG);
  tft.drawCentreString("CENTER", btnImgReset.x + btnImgReset.w/2, btnImgReset.y + 6, 1);

  if (showHint) {
    tft.setTextColor(COLOR_TEXT_SOFT, COLOR_BG);
    tft.setTextSize(1);
    tft.drawCentreString("Arrastra para mover", tft.width()/2, tft.height() - 18, 1);
  }

  tft.setTextColor(COLOR_TEXT_MAIN, COLOR_BG);
  tft.drawCentreString(String(imgOffsetX) + "," + String(imgOffsetY), tft.width()/2, 4, 1);
}

void handleImageViewerTouch() {
  server.handleClient();
  if (!ts.touched()) { isPanningImage = false; delay(6); return; }
  TS_Point p = ts.getPoint();
  int sx, sy; mapRawToScreen(p.x, p.y, sx, sy);

  if (ptInRectInner(sx, sy, btnImgBack, 4)) {
    flashButtonLocal(btnImgBack, "HOME", COLOR_BUTTON_BG);
    drawHome();
    while (ts.touched()) { server.handleClient(); delay(6); }
    return;
  }
  if (ptInRectInner(sx, sy, btnImgReset, 4)) {
    imgOffsetX = imgOffsetY = 0; clampImageOffsets();
    flashButtonLocal(btnImgReset, "CENTER", COLOR_BUTTON_BG);
    drawImageViewer();
    while (ts.touched()) { server.handleClient(); delay(6); }
    return;
  }

  if (!isPanningImage) {
    isPanningImage = true;
    panStartX = panLastX = sx;
    panStartY = panLastY = sy;
  }

  int dx = sx - panLastX;
  int dy = sy - panLastY;
  panLastX = sx; panLastY = sy;
  if (dx != 0 || dy != 0) {
    imgOffsetX += dx;
    imgOffsetY += dy;
    clampImageOffsets();
    unsigned long now = millis();
    if (now - lastPanRedrawMs > 35) { // ajusta para velocidad de arrastre
      drawImageViewer(false);
      lastPanRedrawMs = now;
    }
  }
}

// ---------- HTTP helpers ----------
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

int doHttpsPostSimple(const char* url, const String &body) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("doHttpsPostSimple: no WiFi");
    return -2;
  }
  Serial.printf("doHttpsPostSimple: POST -> %s\n", url);

  // helpers
  auto parseUrl = [](const String &sUrl, String &host, int &port, String &path) {
    port = 443; path = "/";
    int protoSep = sUrl.indexOf("://");
    int start = (protoSep == -1) ? 0 : protoSep + 3;
    int slash = sUrl.indexOf('/', start);
    if (slash == -1) host = sUrl.substring(start);
    else { host = sUrl.substring(start, slash); path = sUrl.substring(slash); }
    int colon = host.indexOf(':');
    if (colon != -1) { port = host.substring(colon + 1).toInt(); host = host.substring(0, colon); }
  };
  auto urlEncode = [](const String &s)->String {
    String out=""; char c;
    const char *p = s.c_str();
    for (size_t i=0;i<s.length();++i){ c=p[i];
      if (('a'<=c && c<='z')||('A'<=c && c<='Z')||('0'<=c && c<='9')||c=='-'||c=='_'||c=='.'||c=='~') out+=c;
      else if (c==' ') out+='+';
      else { char buf[8]; sprintf(buf,"%%%02X",(uint8_t)c); out+=buf; }
    }
    return out;
  };

  // Variantes a intentar (content-type, bodyToSend)
  struct TrySpec { const char* ctype; String body; };
  String b = body;
  if (b.length() == 0) b = "1";
  TrySpec tries[] = {
    { "text/plain", b },
    { "application/x-www-form-urlencoded", String("value=") + urlEncode(b) },
    { "application/json", String("{\"value\":") + b + String("}") }
  };

  // 1) Intentos con HTTPClient usando begin(url) y begin(host,port,path)
  for (int pass = 0; pass < 2; ++pass) {
    for (auto &t : tries) {
      WiFiClientSecure client;
      client.setInsecure();
      HTTPClient https;
      bool started = false;
      if (pass == 0) {
        started = https.begin(client, String(url));
        if (started) Serial.println("doHttpsPostSimple: begin(url) ok (direct)");
      } else {
        String host, path; int port;
        parseUrl(String(url), host, port, path);
        Serial.printf("doHttpsPostSimple: parsed host=%s port=%d path=%s\n", host.c_str(), port, path.c_str());
        started = https.begin(client, host.c_str(), port, path.c_str(), true);
        if (started) Serial.println("doHttpsPostSimple: begin(host,port,path) OK");
      }
      if (!started) { if (pass==0) Serial.println("doHttpsPostSimple: begin(url) fallo"); else Serial.println("doHttpsPostSimple: begin(host,port,path) fallo"); continue; }

      https.addHeader("Content-Type", t.ctype);
      https.addHeader("Connection", "close");
      https.addHeader("Accept", "*/*");
      https.addHeader("User-Agent", "ESP32-HTTPClient");
      https.setTimeout(15000);
      Serial.printf("doHttpsPostSimple: trying POST ctype=%s bodyLen=%d\n", t.ctype, (int)t.body.length());
      int code = https.POST(t.body);
      String payload = https.getString();
      Serial.printf("doHttpsPostSimple: POST result code=%d payloadLen=%d\n", code, (int)payload.length());
      if (payload.length()>0) Serial.printf("doHttpsPostSimple: payload preview: %.200s\n", payload.c_str());
      https.end();
      if (code > 0) return code;
      // si code <= 0, probamos siguiente variante
    }
  }

  // 2) Intento RAW TLS: conectar TLS y escribir petición HTTP "a mano".
  {
    String host, path; int port;
    parseUrl(String(url), host, port, path);
    IPAddress resolved;
    bool hasIp = WiFi.hostByName(host.c_str(), resolved);
    if (hasIp) Serial.printf("doHttpsPostSimple: DNS -> %s\n", resolved.toString().c_str());
    WiFiClientSecure sclient;
    sclient.setInsecure();
    // try connect using hostname first (so SNI works)
    Serial.printf("doHttpsPostSimple: intentando raw TLS connect a %s:%d ...\n", host.c_str(), port);
    bool okConnect = sclient.connect(host.c_str(), port);
    if (!okConnect && hasIp) {
      Serial.printf("doHttpsPostSimple: connect(host) fallo, intentando conectar a IP %s\n", resolved.toString().c_str());
      okConnect = sclient.connect(resolved, port);
    }
    if (!okConnect) {
      Serial.println("doHttpsPostSimple: raw connect fallo");
    } else {
      // Build simple RAW request (try text/plain content first)
      String bodyToSend = b;
      String req = String("POST ") + path + " HTTP/1.1\r\n";
      req += "Host: " + host + "\r\n";
      req += "User-Agent: ESP32-HTTPClient\r\n";
      req += "Accept: */*\r\n";
      req += "Content-Type: text/plain\r\n";
      req += "Content-Length: " + String(bodyToSend.length()) + "\r\n";
      req += "Connection: close\r\n\r\n";
      req += bodyToSend;
      Serial.println("doHttpsPostSimple: sending raw POST...");
      sclient.print(req);
      unsigned long start = millis();
      while (!sclient.available() && (millis() - start) < 8000) delay(5);
      if (!sclient.available()) {
        Serial.println("doHttpsPostSimple: no response after raw POST (timeout)");
      } else {
        String statusLine = sclient.readStringUntil('\n'); statusLine.trim();
        Serial.printf("doHttpsPostSimple: raw response statusLine: %s\n", statusLine.c_str());
        int code = -1;
        if (statusLine.startsWith("HTTP/1.") || statusLine.startsWith("HTTP/2")) {
          int sp1 = statusLine.indexOf(' ');
          if (sp1 > 0) {
            int sp2 = statusLine.indexOf(' ', sp1 + 1);
            String codeStr = (sp2 > sp1) ? statusLine.substring(sp1 + 1, sp2) : statusLine.substring(sp1 + 1);
            code = codeStr.toInt();
            Serial.printf("doHttpsPostSimple: parsed HTTP code=%d\n", code);
          }
        }
        // drain small payload
        String payload=""; start = millis();
        while (sclient.available() && (millis() - start) < 1200 && payload.length()<1200) payload += (char)sclient.read();
        if (payload.length()>0) Serial.printf("doHttpsPostSimple: raw payload preview: %.400s\n", payload.c_str());
        sclient.stop();
        if (code > 0) return code;
      }
    }
  }

  // 3) GET fallback (algunos webhooks aceptan GET)
  {
    String qurl = String(url);
    qurl += (qurl.indexOf('?') == -1) ? "?" : "&";
    qurl += "value=" + urlEncode(b);
    Serial.printf("doHttpsPostSimple: trying GET fallback -> %s\n", qurl.c_str());
    WiFiClientSecure client4; client4.setInsecure();
    HTTPClient http4;
    if (http4.begin(client4, qurl)) {
      http4.setTimeout(12000);
      int code = http4.GET();
      String payload = http4.getString();
      Serial.printf("doHttpsPostSimple: GET fallback code=%d payloadLen=%d\n", code, (int)payload.length());
      if (payload.length()>0) Serial.printf("doHttpsPostSimple: GET payload preview: %.200s\n", payload.c_str());
      http4.end();
      if (code > 0) return code;
    } else {
      Serial.println("doHttpsPostSimple: GET fallback begin failed");
    }
  }

  Serial.println("doHttpsPostSimple: todos los intentos fallaron -> devolviendo -1");
  return -1;
}

// ----------------------
// Helper: URL-encode simple
// ----------------------

// ----------------------
// Simple HTTPS GET helper
// ----------------------
int doHttpsGetSimple(const char* url) {
  if (WiFi.status() != WL_CONNECTED) return -2;
  static WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  Serial.printf("doHttpsGetSimple: GET -> %s\n", url);
  if (!https.begin(client, url)) {
    Serial.println("doHttpsGetSimple: begin failed");
    client.stop();
    return -1;
  }
  https.setTimeout(15000);
  int code = https.GET();
  String payload = https.getString();
  Serial.printf("doHttpsGetSimple: GET code=%d payloadLen=%d\n", code, (int)payload.length());
  https.end();
  return code;
}

// --- overload que acepta timeout (wrapper sencillo) ---
int doHttpsGetSimple(const char* url, unsigned long timeoutMs) {
  // Si quieres implementar el timeout real aqui, reemplaza el cuerpo.
  // Por ahora hacemos wrapper al metodo existente (sin timeout) para eliminar errores de compilacion.
  (void) timeoutMs; // silencia warning si no se usa
  return doHttpsGetSimple(url);
}



// ----------------------
// Modified doPostTrigger: special-case AutoRemote (GET?message=...)
// ----------------------
// --- NEW doPostTrigger: reemplaza la antigua implementacion ---
// helper: simple url-encode para mensajes
String urlEncode(const String &s) {
  String encoded = "";
  char buf[4];
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    // chars "unreserved" segun RFC3986
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      sprintf(buf, "%%%02X", (uint8_t)c);
      encoded += buf;
    }
  }
  return encoded;
}

// --- Enviar la IP actual al AutoRemote / MacroDroid ---
void sendIpToAutoRemote() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("sendIpToAutoRemote: no WiFi, no se envia IP");
    return;
  }

  // Obtiene IP local y forma URL (incluye slash final por si usas http://IP/)
  IPAddress ip = WiFi.localIP();
  String ipStr = ip.toString();
  String urlForm = String("http://") + ipStr + String("/");

  // Mensaje que recibirá MacroDroid (formato consistente con tus otros mensajes)
  String payload = "ESP32_IP:=" + urlForm;

  // Base key AutoRemote (usa la que ya tienes en tu sketch)
  const char* baseKey =
    "https://autoremotejoaomgcd.appspot.com/sendmessage?key="
    "cPSnb3h00yo:APA91bE_nssEZJI0vXpBPFfugOY3OLnKbLEjFqQhn4w6xVvviMeEUR-"
    "lfWECjy9wei7moeWFgfVUTLVbGFk0FRw8gnLSiYeYLIj2T2n_suMjFJks_v5uNjs";

  // usa tu urlEncode() si ya existe en el sketch; si no, abajo dejo una version simple
  String fullUrl;
  #ifdef urlEncode
    fullUrl = String(baseKey) + "&message=" + urlEncode(payload);
  #else
    // simple urlencode (compatibilidad)
    auto simpleUrlEncode = [](const String &s)->String {
      String out="";
      char buf[8];
      for (size_t i=0;i<s.length();++i) {
        char c = s[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || c=='-' || c=='_' || c=='.' || c=='~') {
          out += c;
        } else {
          sprintf(buf,"%%%02X",(uint8_t)c);
          out += buf;
        }
      }
      return out;
    };
    fullUrl = String(baseKey) + "&message=" + simpleUrlEncode(payload);
  #endif

  Serial.printf("Enviando IP -> %s\n", fullUrl.c_str());
  int code = doHttpsGetSimple(fullUrl.c_str());
  if (code > 0) {
    Serial.printf("sendIpToAutoRemote: OK HTTP code=%d\n", code);
  } else if (code == -2) {
    Serial.println("sendIpToAutoRemote: no WiFi");
  } else {
    Serial.println("sendIpToAutoRemote: fallo envio");
  }
}

// ----------------------------------------------------------------
// BASE64 (simple) + URL-SAFE helper
// ----------------------------------------------------------------
String base64Encode(const String &input) {
  static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  const uint8_t *data = (const uint8_t*)input.c_str();
  int len = input.length();
  String out;
  int i = 0;
  while (i + 2 < len) {
    uint8_t a = data[i++], b = data[i++], c = data[i++];
    out += b64chars[(a >> 2) & 0x3F];
    out += b64chars[((a & 0x3) << 4) | ((b >> 4) & 0xF)];
    out += b64chars[((b & 0xF) << 2) | ((c >> 6) & 0x3)];
    out += b64chars[c & 0x3F];
  }
  int rem = len - i;
  if (rem == 1) {
    uint8_t a = data[i++];
    out += b64chars[(a >> 2) & 0x3F];
    out += b64chars[( (a & 0x3) << 4 ) & 0x3F];
    out += '=';
    out += '=';
  } else if (rem == 2) {
    uint8_t a = data[i++], b = data[i++];
    out += b64chars[(a >> 2) & 0x3F];
    out += b64chars[((a & 0x3) << 4) | ((b >> 4) & 0xF)];
    out += b64chars[((b & 0xF) << 2) & 0x3F];
    out += '=';
  }
  return out;
}

String base64UrlSafe(const String &in) {
  String s = base64Encode(in);
  s.replace("+", "-");
  s.replace("/", "_");
  // quitar padding '=' (JS acepta sin padding)
  while (s.endsWith("=")) s.remove(s.length() - 1);
  return s;
}

// ----------------------------------------------------------------
// Enviar a AutoRemote el LINK que abre la UI del ESP con ?a=1&b64=...
// ----------------------------------------------------------------
void sendRenderLinkToAutoRemote(const String &rawText) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("sendRenderLinkToAutoRemote: no WiFi");
    return;
  }

  // construir link objetivo al ESP
  IPAddress ip = WiFi.localIP();
  String target = String("http://") + ip.toString() + String("/?a=1&b64=") + base64UrlSafe(rawText);

  // baseKey usado en tu sketch (reusa el mismo que en sendIpToAutoRemote)
  const char* baseKey =
    "https://autoremotejoaomgcd.appspot.com/sendmessage?key="
    "cPSnb3h00yo:APA91bE_nssEZJI0vXpBPFfugOY3OLnKbLEjFqQhn4w6xVvviMeEUR-"
    "lfWECjy9wei7moeWFgfVUTLVbGFk0FRw8gnLSiYeYLIj2T2n_suMjFJks_v5uNjs";

  // encode del mensaje (URL-encode minimal: codificamos espacios y caracteres especiales)
  auto tinyUrlEncode = [](const String &s)->String {
    String out; char buf[8];
    for (size_t i=0;i<s.length();++i) {
      char c = s[i];
      if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
          (c >= 'A' && c <= 'Z') || c=='-' || c=='_' || c=='.' || c=='~') out += c;
      else {
        sprintf(buf, "%%%02X", (uint8_t)c);
        out += buf;
      }
    }
    return out;
  };

  String fullUrl = String(baseKey) + "&message=" + tinyUrlEncode(target);
  Serial.printf("sendRenderLinkToAutoRemote -> %s\n", fullUrl.c_str());

  int code = doHttpsGetSimple(fullUrl.c_str());
  if (code > 0) Serial.printf("AutoRemote send OK code=%d\n", code);
  else Serial.printf("AutoRemote send fallo code=%d\n", code);
}

// Reemplaza la funcion doPostTrigger por esta version que usa AutoRemote sendmessage (GET)
// Reemplaza ENTIRE la definicion existente de doPostTrigger por esta
void doPostTrigger(int value) {
  showTemporaryStatus("Enviando numero...");

  if (WiFi.status() != WL_CONNECTED) {
    showTemporaryStatus("POST: no WiFi");
    drawCounterFull();
    return;
  }

  // --- Construye URL (usa tu key publica; mantenla privada) ---
  const char* baseKey =
    "https://autoremotejoaomgcd.appspot.com/sendmessage?key="
    "cPSnb3h00yo:APA91bE_nssEZJI0vXpBPFfugOY3OLnKbLEjFqQhn4w6xVvviMeEUR-"
    "lfWECjy9wei7moeWFgfVUTLVbGFk0FRw8gnLSiYeYLIj2T2n_suMjFJks_v5uNjs";

  // Mensaje en el formato que MacroDroid espera
  String payload = "ESP32_TRIGGER:=" + String(value);

  // Si ya tienes urlEncode en tu sketch, usalo. Si no, quita urlEncode() aqui.
  String url = String(baseKey) + "&message=" + urlEncode(payload);

  Serial.printf("doPostTrigger: GET -> %s\n", url.c_str());

  // doHttpsGetSimple debe existir en tu sketch y devolver codigo HTTP o -1/-2
  int code = doHttpsGetSimple(url.c_str());
  if (code > 0) {
    Serial.printf("doPostTrigger: GET code=%d\n", code);
    showTemporaryStatus(String("POST code: " + String(code)).c_str());
  } else if (code == -2) {
    Serial.println("doPostTrigger: no WiFi");
    showTemporaryStatus("POST: no WiFi");
  } else {
    Serial.println("doPostTrigger: fallo");
    showTemporaryStatus("POST fallo");
  }

  // vuelve a pantalla contador
  drawCounterFull();
}



void showTemporaryStatus(const char* msg) {
  int w = tft.textWidth(msg, 1) + 20;
  if (w < 100) w = 100;
  int h = 20;
  int x = (tft.width()-w)/2;
  int y = tft.height() - 28;
  tft.fillRoundRect(x, y, w, h, 6, tft.color565(0,0,0));
  tft.drawRoundRect(x, y, w, h, 6, tft.color565(255,255,255));
  tft.setTextSize(1); tft.setTextColor(TFT_WHITE);
  tft.drawCentreString(msg, x + w/2, y + 4, 1);
  delay(700);
  tft.fillRoundRect(x, y, w, h, 6, TFT_BLACK);
}

void doPostRun() {
  showTemporaryStatus("Enviando RUN...");
  int code = doHttpsPostSimple(URL_RUN_POST, "");
  if (code > 0) showTemporaryStatus(String("RUN code: " + String(code)).c_str());
  else if (code == -2) showTemporaryStatus("RUN: no WiFi");
  else showTemporaryStatus("RUN fallo");
  drawHome();
}

// --- INSERT: Acciones internas en lugar de llamar via HTTP ---
// Adapta el contenido de estas funciones a lo que hacia /next y /prev en tu servidor.
// --- Acciones internas: llaman a los mismos handlers HTTP que ya registraste ---
// Llamar a handleNext()/handlePrev() ejecuta la misma lógica que GET /next y /prev.
void doNextPage() {
  Serial.println("doNextPage(): llamando a handleNext()");
  // Llamada directa al handler que registraste con server.on("/next",...)
  handleNext();
}

void doPrevPage() {
  Serial.println("doPrevPage(): llamando a handlePrev()");
  handlePrev();
}

// Reemplaza la funcion original por esta (GET directo a AutoRemote)

// ---------- RECTS helper ----------
void recomputeRects() {
  // Layout: dos botones centrados (RUN | Contador) con menor tamaño
  const int btnW = 82;    // ancho reducido
  const int btnH = 28;    // alto reducido
  const int spacing = 12; // separacion entre botones

  int totalW = btnW * 3 + spacing * 2;
  int startX = (V_W - totalW) / 2;

  // Posicion vertical (igual que antes para mantener diseño)
  int vyBtns = 74;

  // Botones principales centrados y mas pequeños
  btnRun       = vRectMin(startX, vyBtns, btnW, btnH, 48);
  btnCounter   = vRectMin(startX + btnW + spacing, vyBtns, btnW, btnH, 48);
  btnImageMode = vRectMin(startX + (btnW + spacing) * 2, vyBtns, btnW, btnH, 48);

  // Ocultar/eliminar Calibrar
  btnCalibrate = { 0, 0, 0, 0 };

  // Mantener controles del contador e indicadores en sus posiciones (sin cambios)
  btnMinus  = vRectMin(12, 56, 38, 38, 48);
  btnPlus   = vRectMin(270, 56, 38, 38, 48);
  btnNumber = vRectMin(110, 72, 100, 44, 48);

  // Zonas laterales (puedes aumentarlas si quieres más margen)
  const int widthSide = 72;
  btnLeftZone  = vRectMin(0, 0, widthSide, V_H, 28);
  btnRightZone = vRectMin(V_W - widthSide, 0, widthSide, V_H, 28);

  // Controles del visor de imagen
  btnImgBack  = vRectMin(10, 10, 68, 26, 40);
  btnImgReset = vRectMin(V_W - 78, 10, 68, 26, 40);
}



// ---------- SPIFFS / JPG display (from your second sketch) ----------
uint16_t hexTo565(const char *hex) {
  unsigned long val = strtoul(hex, NULL, 16);
  uint8_t r = (val >> 16) & 0xFF;
  uint8_t g = (val >> 8) & 0xFF;
  uint8_t b = val & 0xFF;
  return (uint16_t)((r & 0xF8) << 8) | (uint16_t)((g & 0xFC) << 3) | (uint16_t)(b >> 3);
}

void clampImageOffsets() {
  if (lastImgW == 0 || lastImgH == 0) return;
  int baseX = (displayBaseMode == 0) ? (tft.width() - lastImgW) / 2 : 0;
  int baseY = (displayBaseMode == 0) ? (tft.height() - lastImgH) / 2 : 0;
  int minX = tft.width() - (baseX + lastImgW);
  int maxX = -baseX;
  int minY = tft.height() - (baseY + lastImgH);
  int maxY = -baseY;
  if (minX > maxX) { minX = maxX = 0; }
  if (minY > maxY) { minY = maxY = 0; }
  imgOffsetX = constrain(imgOffsetX, minX, maxX);
  imgOffsetY = constrain(imgOffsetY, minY, maxY);
}

void displayJpgFile(const char *path, int xOffset = 0, int yOffset = 0, bool useDefaults = true) {
  if (!SPIFFS.exists(path)) {
    Serial.printf("⚠️ No existe el archivo JPG: %s\n", path);
    return;
  }
  if (!JpegDec.decodeFsFile(path)) {
    Serial.println("❌ Error al decodificar el JPG");
    return;
  }
  int imgW = JpegDec.width;
  int imgH = JpegDec.height;
  lastImgW = imgW; lastImgH = imgH; currentImagePath = path;
  Serial.printf("✅ Imagen decodificada: %dx%d. Mostrando (swapBytes=true)...\n", imgW, imgH);

  int finalX = xOffset;
  int finalY = yOffset;
  if (useDefaults) {
    finalX = displayDefaultX;
    finalY = displayDefaultY;
  }
  clampImageOffsets();

  uint16_t screenBg = COLOR_BG;
  tft.fillScreen(screenBg);
  tft.setSwapBytes(true);

  int baseX, baseY;
  if (displayBaseMode == 0) {
    baseX = (tft.width()  - imgW) / 2;
    baseY = (tft.height() - imgH) / 2;
  } else {
    baseX = 0;
    baseY = 0;
  }
  int xpos = baseX + finalX;
  int ypos = baseY + finalY;

  while (JpegDec.read()) {
    uint16_t *pImg = JpegDec.pImage;
    if (!pImg) continue;

    int mcu_x = JpegDec.MCUx * JpegDec.MCUWidth + xpos;
    int mcu_y = JpegDec.MCUy * JpegDec.MCUHeight + ypos;
    int mcu_w = JpegDec.MCUWidth;
    int mcu_h = JpegDec.MCUHeight;

    int drawX = mcu_x;
    int drawY = mcu_y;
    int srcX = 0;
    int srcY = 0;
    int drawW = mcu_w;
    int drawH = mcu_h;

    if (drawX < 0) {
      srcX = -drawX;
      drawW -= srcX;
      drawX = 0;
    }
    if (drawY < 0) {
      srcY = -drawY;
      drawH -= srcY;
      drawY = 0;
    }
    if (drawX + drawW > tft.width()) {
      drawW = tft.width() - drawX;
    }
    if (drawY + drawH > tft.height()) {
      drawH = tft.height() - drawY;
    }
    if (drawW <= 0 || drawH <= 0) continue;

    uint16_t *srcPtr = pImg + (srcY * mcu_w) + srcX;
    tft.pushImage(drawX, drawY, drawW, drawH, srcPtr);
  }

  Serial.println("✅ Imagen mostrada (con defaults/clipping).");
}

// ---------------------- WebServer handlers (upload / show / pushText / nextText) ----------------------
String currentFilenameUpload;
File uploadFile;

void handleWhoAmI() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  IPAddress ip = WiFi.localIP();
  String ipstr = ip.toString();
  String body = String("{\"ip\":\"") + ipstr + String("\",\"origin\":\"http://") + ipstr + String("/\"}");
  server.send(200, "application/json", body);
}

void handleRoot() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }

  // Construimos la IP actual del ESP (incluye slash final)
  String ipStr = String("http://") + WiFi.localIP().toString();
  if (!ipStr.endsWith("/")) ipStr += "/";

  // HTML como template con placeholder __ESP_URL__ que luego reemplazamos
    // HTML como template con placeholder __ESP_URL__ que luego reemplazamos
  String html = R"rawliteral(
<!doctype html>
<html lang="es">
<head><meta charset="utf-8" /><meta name="viewport" content="width=device-width,initial-scale=1" />
<title>ESP32 — LaTeX Render (optimizado)</title>
<script src="https://cdn.jsdelivr.net/npm/html2canvas@1.4.1/dist/html2canvas.min.js"></script>
<style>
:root{--bg:#0e0f11;--card:#141517;--accent:#6be26b;--muted:#9aa0a6;color-scheme:dark}
html,body{height:100%;margin:0;font-family:Inter, system-ui, -apple-system, "Segoe UI", Roboto, "Helvetica Neue", Arial;background:linear-gradient(180deg,#060606 0%, #0b0c0d 100%);color:#ddd;padding:12px}
.layout{max-width:1000px;margin:8px auto;display:grid;grid-template-columns:1fr 380px;gap:12px}
header{grid-column:1/-1;display:flex;gap:12px;align-items:center}
.main{background:var(--card);padding:12px;border-radius:10px;box-shadow:0 6px 18px rgba(0,0,0,0.6)}
textarea{width:100%;height:260px;background:#0b0b0c;color:#eee;border:1px solid #222;padding:10px;border-radius:8px;resize:vertical;font-family:monospace}
.preview-wrap{background:#000;border-radius:6px;padding:8px;display:flex;flex-direction:column;gap:8px}
.preview{width:100%;height:260px;overflow:hidden;background:#111;border-radius:6px;display:flex;align-items:flex-start;justify-content:center}
.preview-inner{width:100%;box-sizing:border-box;padding:6px;overflow:auto}
.status{padding:8px;border-radius:6px;background:#0e0f10;color:var(--muted);font-size:0.9rem}
.progress{height:10px;background:#222;border-radius:6px;overflow:hidden}
.log{height:160px;overflow:auto;background:#070707;border-radius:6px;padding:8px;font-family:monospace;color:#cdd}
/* evitar que elementos de fórmula se partan entre páginas/columnas */

.no-break, .math-block {
  page-break-inside: avoid;
  break-inside: avoid;
  -webkit-column-break-inside: avoid;
  -webkit-page-break-inside: avoid;
  display: block;
}

/* Cada "página" (tile) */
.page-tile {
  width: 100%;
  height: 100%;
  display: none; /* se mostrará una a la vez */
  overflow: hidden;
}

/* estilo para imagenes LaTeX */
.latex-img{
  vertical-align: middle;
  max-width: 100%;
  height: auto;
  display: block;
  margin-top: 2px;
  margin-bottom: 2px;
}

button{background:linear-gradient(180deg,#20b020,#0a8f0a);border:0;padding:9px 12px;color:#021;border-radius:8px;font-weight:700;cursor:pointer}
button.secondary{background:linear-gradient(180deg,#2b2b2b,#111);color:#ddd}
.controls-row{display:flex;gap:8px;flex-wrap:wrap}
.small{font-size:0.85rem;padding:6px 8px}
footer{grid-column:1/-1;color:#9aa0a6}
</style>
</head>
<body>
<div class="layout">
<header><h1>ESP32 — LaTeX Render (optimizado)</h1><div style="margin-left:10px;color:#9aa0a6">Modo: paginado o scroll continuo · POST /pushText</div></header>
<div class="main">
<label for="src">Pega aquí la respuesta (texto + LaTeX):</label>
<textarea id="src" placeholder="Pega la respuesta de ChatGPT (texto + LaTeX)."></textarea>
<div class="controls-row" style="margin-top:8px">
<button id="renderBtn">Renderizar + Generar</button>
<button id="sendBtn" class="secondary" disabled>Enviar Página</button>
<button id="sendAllBtn" class="secondary" disabled>Enviar Todo</button>
<button id="clearBtn" class="secondary">Limpiar</button>
<label style="margin-left:8px"><input type="checkbox" id="scrollMode"> Scroll continuo</label>
</div>
<div style="display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:8px">
<div><label>IP / URL del ESP32</label><input id="espUrl" type="text" value="__ESP_URL__"></div>
<div><label>Ruta de subida</label><input id="uploadPath" type="text" value="/upload"></div>
</div>
<div style="display:grid;grid-template-columns:repeat(3,1fr);gap:8px;margin-top:8px">
<div><label>Ancho (px)</label><input id="screenW" type="number" value="160" min="1"></div>
<div><label>Alto (px)</label><input id="screenH" type="number" value="50" min="1"></div>
<div><label>JPEG calidad</label><input id="jpgQuality" type="number" step="0.05" min="0.1" max="1" value="1"></div>
</div>
<div style="display:flex;gap:8px;margin-top:8px">
  <div><label>BG hex (sin #)</label><input id="bgHex" type="text" value="001a00" maxlength="6" pattern="[A-Fa-f0-9]{6}"></div>
  <div><label>FG hex (sin #)</label><input id="fgHex" type="text" value="000000" maxlength="6" pattern="[A-Fa-f0-9]{6}"></div>
</div>
<div style="margin-top:10px" class="status" id="status">Estado: listo</div>
<div style="margin-top:10px" class="progress"><div id="progressBar" style="width:0%;height:100%;background:#6be26b;border-radius:6px"></div></div>
<div style="margin-top:8px" class="log" id="log"></div>
</div>
<aside class="main controls">
<div class="preview-wrap">
<div style="display:flex;justify-content:space-between;align-items:center">
<div style="color:#9aa0a6">Vista previa</div>
<div style="color:#9aa0a6">Páginas: <span id="pageCount">0</span></div>
</div>
<div class="preview" id="previewViewport"><div class="preview-inner" id="previewInner"></div></div>
<div style="display:flex;gap:8px;margin-top:8px">
<button id="prevPage" class="secondary" disabled>◀ Prev</button>
<button id="nextPage" class="secondary" disabled>Next ▶</button>
<div style="flex:1;text-align:right"><label style="color:#9aa0a6">Retries: <span id="retries">3</span></label></div>
</div>
</div>
<div style="margin-top:12px;color:#9aa0a6">Consejo: activa "Scroll continuo" para desplazamiento en un espacio reducido. Usa "Enviar Todo" para subir todas las páginas en secuencia.</div>
</aside>
<footer>Hecho para ESP32 · CodeCogs + html2canvas</footer>
</div>
<script>
/* ---------- estado y utilidades ---------- */
const src = document.getElementById('src'),
      renderBtn = document.getElementById('renderBtn'),
      sendBtn = document.getElementById('sendBtn'),
      sendAllBtn = document.getElementById('sendAllBtn'),
      clearBtn = document.getElementById('clearBtn'),
      scrollCheckbox = document.getElementById('scrollMode'),
      espUrl = document.getElementById('espUrl'),
      uploadPath = document.getElementById('uploadPath'),
      screenW = document.getElementById('screenW'),
      screenH = document.getElementById('screenH'),
      jpgQuality = document.getElementById('jpgQuality'),
      bgHex = document.getElementById('bgHex'),
      fgHex = document.getElementById('fgHex'),
      status = document.getElementById('status'),
      logEl = document.getElementById('log'),
      progressBar = document.getElementById('progressBar'),
      previewInner = document.getElementById('previewInner'),
      pageCountEl = document.getElementById('pageCount'),
      prevPageBtn = document.getElementById('prevPage'),
      nextPageBtn = document.getElementById('nextPage');

let tiles = [], currentPreview = 0, RETRIES = 3;
let objectUrlList = []; // para revocar URLs creadas con createObjectURL
let currentBg = '001a00', currentFg = '000000'; // guardan el color en uso (sin '#')
const latexCache = new Map(); // cache para imágenes LaTeX

function log(...args){ const t = new Date().toLocaleTimeString(); logEl.innerText = `${t} ${args.join(' ')}\n` + logEl.innerText; }
function setStatus(s){ status.innerText = "Estado: " + s; }
function setProgress(p){ progressBar.style.width = `${p}%`; }

/* mostrar errores JS en UI y consola para depuracion */
window.addEventListener('error', (ev) => {
  console.error('JS ERROR:', ev.message, ev.filename + ':' + ev.lineno);
  try { log('JS ERROR: ' + ev.message + ' (' + ev.filename + ':' + ev.lineno + ')'); } catch(e){}
});
window.addEventListener('unhandledrejection', (ev) => {
  console.error('Promise rejection:', ev.reason);
  try { log('Promise rejection: ' + (ev.reason && ev.reason.message ? ev.reason.message : ev.reason)); } catch(e){}
});

/* ---------- LaTeX -> imágenes (con cache) ---------- */
function escapeForAttr(s){ return s.replace(/&/g,'&amp;').replace(/"/g,'&quot;').replace(/</g,'&lt;').replace(/>/g,'&gt;'); }
async function latexToDataURL(latex, dpi = 150, bg = '001a00', fg = '000000') {
  const key = dpi + '::' + bg + '::' + fg + '::' + latex;
  if (latexCache.has(key)) return latexCache.get(key);
  const payload = `\\dpi{${dpi}}\\bg{${bg}}\\fg{${fg}} ${latex}`;
  const url = 'https://latex.codecogs.com/png.latex?' + encodeURIComponent(payload);
  const res = await fetch(url);
  if(!res.ok) throw new Error('HTTP ' + res.status);
  const blob = await res.blob();
  const dataUrl = await new Promise((resolve,reject)=>{
    const fr=new FileReader();
    fr.onloadend = ()=>resolve(fr.result);
    fr.onerror = reject;
    fr.readAsDataURL(blob);
  });
  latexCache.set(key, dataUrl);
  return dataUrl;
}

async function replaceLatexWithImages(html) {
    // --- NUEVO: colapsar secuencias $$\text{...}$$ en un bloque de texto simple ---
  // Convierte dos o más líneas consecutivas del tipo $$\text{\...}$$
  // en un solo <div> con saltos de línea, para evitar muchos bloques display.
  const seqTextRegex = /((?:\$\$\s*\\text\{[^\}]*\}\s*\$\$\s*){2,})/g;
  html = html.replace(seqTextRegex, (m) => {
    const matches = [...m.matchAll(/\$\$\s*\\text\{([^\}]*)\}\s*\$\$/g)];
    const lines = matches.map(a => a[1].trim());
    const htmlText = lines.map(l => escapeForAttr(l)).join('<br>');
    return `<div style="margin:2px 0; line-height:1.05">${htmlText}</div>`;
  });

  const blockRegex = /\$\$([\s\S]+?)\$\$/g;
  let match;
  while ((match = blockRegex.exec(html)) !== null) {
    const raw = match[0], latex = match[1].trim();
    try {
      setStatus('Renderizando fórmula (bloque)...');
      const bgVal = (bgHex && /^[A-Fa-f0-9]{6}$/.test(bgHex.value)) ? bgHex.value : '001a00';
      const fgVal = (fgHex && /^[A-Fa-f0-9]{6}$/.test(fgHex.value)) ? fgHex.value : '000000';
      const dataUrl = await latexToDataURL(latex, 150, bgVal, fgVal);
      const imgTag = `<div class="math-block no-break" style="text-align:center;margin:2px 0;display:block;break-inside:avoid;page-break-inside:avoid;-webkit-column-break-inside:avoid;">` +
               `<img class="latex-img" src="${dataUrl}" alt="${escapeForAttr(latex)}" style="display:block;max-width:100%;height:auto;margin:2px 0;">` +
               `</div>`;
      html = html.replace(raw, imgTag);
    } catch (err) {
      html = html.replace(raw, `<pre style="color:#900;background:#fee;padding:6px;border-radius:6px">LaTeX render error</pre>`);
    }
  }
  const inlineRegex = /(^|[^$])\$([^\$\n]+?)\$([^$]|$)/g;
  html = html.replace(inlineRegex, function(full, prefix, latex, suffix) {
    const token = '___LATEX_TOKEN_' + Math.random().toString(36).slice(2) + '___';
    if (!window._latexInlineMap) window._latexInlineMap = {};
    window._latexInlineMap[token] = { latex: latex.trim(), prefix: prefix, suffix: suffix };
    return prefix + token + suffix;
  });
  if (window._latexInlineMap) {
    const keys = Object.keys(window._latexInlineMap);
    for (let k of keys) {
      const info = window._latexInlineMap[k];
      try {
        setStatus('Renderizando fórmula (inline)...');
        const bgVal = (bgHex && /^[A-Fa-f0-9]{6}$/.test(bgHex.value)) ? bgHex.value : '001a00';
        const fgVal = (fgHex && /^[A-Fa-f0-9]{6}$/.test(fgHex.value)) ? fgHex.value : '000000';
        const dataUrl = await latexToDataURL(info.latex, 120, bgVal, fgVal);
        const imgTag = `<img class="latex-img" src="${dataUrl}" alt="${escapeForAttr(info.latex)}" style="display:inline-block">`;
        html = html.replace(k, imgTag);
      } catch (err) {
        html = html.replace(k, `<code style="color:#900">${escapeForAttr(info.latex)}</code>`);
      }
    }
    window._latexInlineMap = null;
  }
  setStatus('Fórmulas convertidas a imágenes');
  return html;
}

/* helpers: hex->rgb y limpieza de casi-blancos (una sola copia) */
function hexToRgb(h) {
  const hex = (h||'').replace(/^#/, '');
  return {
    r: parseInt(hex.substring(0,2) || '00', 16),
    g: parseInt(hex.substring(2,4) || '00', 16),
    b: parseInt(hex.substring(4,6) || '00', 16)
  };
}
function replaceNearWhiteWithBg(canvas, bgHex) {
  const ctx = canvas.getContext('2d');
  try {
    const imgd = ctx.getImageData(0, 0, canvas.width, canvas.height);
    const data = imgd.data;
    const bg = hexToRgb(bgHex || 'ffffff');
    for (let i = 0; i < data.length; i += 4) {
      const r = data[i], g = data[i+1], b = data[i+2], a = data[i+3];
      if (a > 10 && r > 240 && g > 240 && b > 240) {
        data[i]   = bg.r;
        data[i+1] = bg.g;
        data[i+2] = bg.b;
        data[i+3] = 255;
      }
    }
    ctx.putImageData(imgd, 0, 0);
  } catch (e) {
    console.warn('replaceNearWhiteWithBg falla (posible CORS):', e);
  }
}

/* ---------- Generación de tiles ---------- */
async function makeRenderableElement(htmlContent, widthPx, bgHex, fgHex) {
  const old = document.getElementById('mj_render_area'); if (old) old.remove();
  const area = document.createElement('div');
  area.id = 'mj_render_area';
  area.style.position = 'fixed';
  area.style.left = '-10000px';
  area.style.top = '-10000px';
  area.style.width = widthPx + 'px';
  area.style.boxSizing = 'border-box';
    area.style.padding = '0'; // sin padding para que el contenido ocupe exactamente widthPx x heightPx
  area.style.background = '#' + bgHex;
  area.style.color = '#' + fgHex;
  area.style.fontFamily = 'Arial, Helvetica, sans-serif';
  // fontsize adaptado si no viene en el HTML (fallback)
  const fallbackFs = Math.max(10, Math.round(widthPx / 16));
  area.style.fontSize = fallbackFs + 'px';
  area.style.lineHeight = '1.05';
  setStatus('Procesando contenido (reemplazo LaTeX)...');
  const processed = await replaceLatexWithImages(htmlContent);
  area.innerHTML = processed;
  document.body.appendChild(area);
  await new Promise(resolve => setTimeout(resolve, 250));
  area.offsetHeight;
  setStatus('Contenido listo para captura');
  return area;
}

function createViewportForCapture(renderArea, vw, vh, bgHex){
  const wrapperId = 'capture_viewport_wrapper';
  let wrapper = document.getElementById(wrapperId);
  if(wrapper) wrapper.remove();
  wrapper = document.createElement('div');
  wrapper.id = wrapperId;
  wrapper.style.position = 'fixed';
  wrapper.style.left = '-10000px';
  wrapper.style.top = '-10000px';
  wrapper.style.width = vw + 'px';
  wrapper.style.height = vh + 'px';
  wrapper.style.overflow = 'hidden';
  wrapper.style.background = '#' + bgHex;
  wrapper.style.boxSizing = 'border-box';
  wrapper.appendChild(renderArea);
  document.body.appendChild(wrapper);
  return wrapper;
}
// --- Auxiliar: ajustar scroll para que no-break no quede cortado ---
function adjustScrollForNoBreak(container, desiredScrollY, tileHeight) {
  const maxScroll = container.scrollHeight - tileHeight;
  let newScroll = Math.max(0, Math.min(desiredScrollY, maxScroll));
  const elems = container.querySelectorAll('.no-break');

  // si algún elemento cruza la línea inferior del tile, desplazar la captura para que quepa entera
  for (let i = 0; i < elems.length; i++) {
    const el = elems[i];
    const top = el.offsetTop;
    const bottom = top + el.offsetHeight;
    const tileBottom = newScroll + tileHeight;

    // si el elemento empieza antes del bottom y termina después del bottom -> cruza
    if (top < tileBottom && bottom > tileBottom - 2) {
      // desplazar el scroll para que el elemento quede visible en su totalidad
      const candidate = bottom - tileHeight + 4; // pequeño buffer
      newScroll = Math.max(0, Math.min(candidate, maxScroll));
      // re-evaluamos desde el principio por si con el nuevo scroll otro elemento quedara cortado
      // reiniciamos el bucle para comprobar todos con el nuevo newScroll
      i = -1;
    }
  }
  return newScroll;
}
async function generateTiles(htmlContent, w, h, quality=1, bgHex = '001a00', fgHex = '000000'){
  setStatus('Generando páginas...');
  tiles = [];
  currentBg = bgHex;
  currentFg = fgHex;

  const renderArea = await makeRenderableElement(htmlContent, w, bgHex, fgHex);
  const totalH = renderArea.scrollHeight;
  const pageCount = Math.max(1, Math.ceil(totalH / h));
  pageCountEl.innerText = pageCount;
  const wrapper = createViewportForCapture(renderArea, w, h, bgHex);
  renderArea.style.position = 'relative';
  renderArea.style.left = '0';
  renderArea.style.top = '0';
  renderArea.style.margin = '0';
  renderArea.style.padding = '4px';
  renderArea.style.background = '#' + bgHex;

  for(let i=0;i<pageCount;i++){
    const translateY = -i * h;
    renderArea.style.transform = `translateY(${translateY}px)`;
    await new Promise(r=>setTimeout(r, 60));
    setStatus(`Capturando página ${i+1}/${pageCount} ...`);
    // Forzamos escala 1 para consistencia entre dispositivos.
    // Si quieres más resolución, cámbialo manualmente a 2 (pero será idéntico en todos los dispositivos).
    const scale = 2;
    const canvas = await html2canvas(wrapper, {
      backgroundColor: '#' + bgHex,
      width: w,
      height: h,
      useCORS: true,
      logging: false,
      scale: scale,
      allowTaint: false,
      imageTimeout: 0
    });
    try {
      replaceNearWhiteWithBg(canvas, bgHex);
    } catch(e) {
      console.warn('replaceNearWhiteWithBg fallo o no permitido por CORS', e);
    }
    const jpegQuality = Math.min(1.0, Math.max(0.1, quality));
    const blob = await new Promise(resolve => canvas.toBlob(resolve, 'image/jpeg', jpegQuality));
    tiles.push({blob, index: i, width: w, height: h});
    setProgress(Math.round(((i+1)/pageCount)*100));
    log(`Página ${i+1} generada — bytes: ${blob.size}`);
  }

  wrapper.remove();
  const ra = document.getElementById('mj_render_area'); if(ra) ra.remove();
  setStatus('Generación completa');
  return tiles;
}

/* ---------- Previsualización: revocar URLs y mostrar ---------- */
function revokeObjectUrls() {
  objectUrlList.forEach(u => {
    try { URL.revokeObjectURL(u); } catch(e){}
  });
  objectUrlList = [];
}

function updatePreview(){
  const isScroll = scrollCheckbox.checked;
  if(tiles.length === 0){
    revokeObjectUrls();
    previewInner.innerHTML = '<div style="color:#888;padding:20px">Sin páginas</div>';
    prevPageBtn.disabled = true;
    nextPageBtn.disabled = true;
    sendBtn.disabled = true;
    sendAllBtn.disabled = true;
    return;
  }
  pageCountEl.innerText = tiles.length;
  revokeObjectUrls();

  if(isScroll){
    const bg = currentBg || '001a00';
    const fg = currentFg || '000000';
    previewInner.style.overflow = 'auto';
    previewInner.style.background = '#' + bg;
    previewInner.style.color = '#' + fg;
    const h = parseInt(screenH.value,10) || 240;
    previewInner.style.maxHeight = h + 'px';
    let html = '';
    for(let i=0;i<tiles.length;i++){
      const url = URL.createObjectURL(tiles[i].blob);
      objectUrlList.push(url);
      html += `<img src="${url}" style="width:100%;display:block;margin-bottom:6px;background:transparent" data-page="${i}">`;
    }
    previewInner.innerHTML = html;
    prevPageBtn.disabled = false; nextPageBtn.disabled = false;
    sendBtn.disabled = false; sendAllBtn.disabled = false;
    const imgs = previewInner.querySelectorAll('img');
    if(imgs[currentPreview]) imgs[currentPreview].scrollIntoView({behavior:'smooth',block:'start'});
  } else {
    const bg = currentBg || '001a00';
    const fg = currentFg || '000000';
    previewInner.style.overflow = 'hidden';
    previewInner.style.background = '#' + bg;
    previewInner.style.color = '#' + fg;
    const t = tiles[currentPreview];
    const url = URL.createObjectURL(t.blob);
    objectUrlList.push(url);
    previewInner.innerHTML = `<img src="${url}" style="width:100%;height:auto;display:block;background:transparent">`;
    prevPageBtn.disabled = (currentPreview === 0);
    nextPageBtn.disabled = (currentPreview === tiles.length-1);
    sendBtn.disabled = false; sendAllBtn.disabled = false;
  }
}

/* ---------- Navegación (prev/next) ---------- */
prevPageBtn.addEventListener('click', async () => {
  const isScroll = scrollCheckbox.checked;
  if(isScroll){
    const h = parseInt(screenH.value,10) || 240;
    previewInner.scrollBy({top: -h, left:0, behavior:'smooth'});
    currentPreview = Math.max(0, currentPreview-1);
    await sendCurrentPage();
  } else {
    if (currentPreview > 0) { currentPreview--; updatePreview(); await sendCurrentPage(); }
  }
});
nextPageBtn.addEventListener('click', async () => {
  const isScroll = scrollCheckbox.checked;
  if(isScroll){
    const h = parseInt(screenH.value,10) || 240;
    previewInner.scrollBy({top: h, left:0, behavior:'smooth'});
    currentPreview = Math.min(tiles.length-1, currentPreview+1);
    await sendCurrentPage();
  } else {
    if (currentPreview < tiles.length - 1) { currentPreview++; updatePreview(); await sendCurrentPage(); }
  }
});

/* ---------- Upload ---------- */
async function uploadTile(tile, pageNumber, baseUrl, path, maxRetries=3, timeoutMs=15000){
  const url = baseUrl.replace(/\/+$/,'') + path + '?page=' + pageNumber;
  let attempt=0;
  while(attempt < maxRetries){
    attempt++;
    try{
      const form = new FormData();
      const filename = `page_${String(pageNumber).padStart(3,'0')}.jpg`;
      form.append('file', tile.blob, filename);
      setStatus(`Subiendo página ${pageNumber} (intento ${attempt})`);
      const controller = new AbortController();
      const id = setTimeout(()=>controller.abort(), timeoutMs);
      const res = await fetch(url, { method: 'POST', body: form, signal: controller.signal });
      clearTimeout(id);
      if(!res.ok) throw new Error(`HTTP ${res.status}`);
      const txt = await res.text().catch(()=>null);
      log(`Subida OK página ${pageNumber}: ${res.status} ${txt ? '- ' + txt : ''}`);
      try { localStorage.setItem('espUrl', baseUrl.replace(/\/+$/, '/') ); } catch(e){}
      return true;
    }catch(err){
      log(`Error subida página ${pageNumber} intento ${attempt}:`, err.message || err);
      await new Promise(r=>setTimeout(r, 400 * attempt));
    }
  }
  return false;
}

async function sendCurrentPage() {
  if (tiles.length === 0) { log('No hay páginas generadas para enviar'); return; }
  const base = espUrl.value.trim(); if (!base) { alert('Escribe la IP/URL del ESP32'); return; }
  const path = uploadPath.value.trim() || '/upload';
  const tile = tiles[currentPreview];
  const pageNo = currentPreview + 1;
  sendBtn.disabled = true; renderBtn.disabled = true; prevPageBtn.disabled = true; nextPageBtn.disabled = true;
  setStatus(`Enviando página ${pageNo}/${tiles.length}...`);
  setProgress(0);
  const ok = await uploadTile(tile, pageNo, base, path, RETRIES);
  if (ok) { setStatus(`Página ${pageNo} enviada.`); setProgress(100); }
  else { setStatus(`Error al enviar página ${pageNo}.`); setProgress(0); }
  renderBtn.disabled = false; if (tiles.length > 0) sendBtn.disabled = false;
  updatePreview();
}

async function sendAllPages() {
  if (tiles.length === 0) return;
  const base = espUrl.value.trim(); if (!base) { alert('Escribe la IP/URL del ESP32'); return; }
  const path = uploadPath.value.trim() || '/upload';
  renderBtn.disabled = true; sendAllBtn.disabled = true; sendBtn.disabled = true;
  for(let i=0;i<tiles.length;i++){
    currentPreview = i; updatePreview();
    const ok = await uploadTile(tiles[i], i+1, base, path, RETRIES);
    if(!ok){ log(`Fallo al enviar página ${i+1}, abortando envío masivo.`); break; }
    await new Promise(r=>setTimeout(r, 200));
  }
  setStatus('Envio masivo finalizado');
  renderBtn.disabled = false; sendAllBtn.disabled = false; sendBtn.disabled = false;
}

/* Handlers: renderBtn / sendBtn / clearBtn */
renderBtn.addEventListener('click', async ()=>{
  try{
    setStatus('Preparando contenido...');
    const html = src.value.trim();
    if(!html){ alert('Pega la respuesta (texto + LaTeX) en el área de texto.'); return; }
    let content = html;
    if(!(/<\/?[a-z][\s\S]*>/i.test(content)) ){
      content = content.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');
      content = content.split(/\n{2,}/).map(p=>`<p>${p.replace(/\n/g,'<br>')}</p>`).join('');
    }
       renderBtn.disabled = true; sendBtn.disabled = true; sendAllBtn.disabled = true; setProgress(0);
    const w = parseInt(screenW.value,10) || 320;
    const h = parseInt(screenH.value,10) || 240;
    // Calculo de fontsize relativo al ancho (ajusta el divisor si quieres mas/menos grande)
    const fontSize = Math.max(10, Math.round(w / 16)); // por ejemplo: w=320 => 20px, w=524 => 33px
    const lineHeight = 1.25;
    const fullHtml = `<div style="color:#000;font-family:Arial, Helvetica, sans-serif;font-size:${fontSize}px;line-height:${lineHeight}">${content}</div>`;

    const q = parseFloat(jpgQuality.value) || 1;
    const bgVal = (bgHex && /^[A-Fa-f0-9]{6}$/.test(bgHex.value)) ? bgHex.value : '001a00';
    const fgVal = (fgHex && /^[A-Fa-f0-9]{6}$/.test(fgHex.value)) ? fgHex.value : '000000';
    await new Promise(r => setTimeout(r, 150));
    tiles = await generateTiles(fullHtml, w, h, q, bgVal, fgVal);

    if(tiles.length>0){ currentPreview = 0; updatePreview(); sendBtn.disabled = false; sendAllBtn.disabled = false; log(`Generadas ${tiles.length} páginas.`); setStatus('Listo para enviar'); }
    else setStatus('No se generaron páginas');
  }catch(err){ log('Error en render:', err); setStatus('Error generando páginas'); }
  finally { renderBtn.disabled = false; }
});
sendBtn.addEventListener('click', sendCurrentPage);
sendAllBtn.addEventListener('click', sendAllPages);
clearBtn.addEventListener('click', ()=>{ src.value = ''; tiles = []; currentPreview = 0; revokeObjectUrls(); updatePreview(); setProgress(0); pageCountEl.innerText = '0'; setStatus('Listo'); log('Contenido limpiado'); });

/* ===== Autodetect ESP URL: intenta origin -> mDNS -> lastKnown ===== */
async function probeUrl(url, timeout = 2500) {
  try {
    const controller = new AbortController();
    const id = setTimeout(() => controller.abort(), timeout);
    const res = await fetch(url.replace(/\/+$/, '') + '/whoami', { method: 'GET', mode: 'cors', signal: controller.signal });
    clearTimeout(id);
    if (!res.ok) return null;
    const j = await res.json().catch(()=>null);
    return (j && j.origin) ? j.origin : (j && j.ip) ? ('http://' + j.ip + '/') : url;
  } catch (e) {
    return null;
  }
}

async function autoDetectEspUrl() {
  try {
    const origin = window.location.origin;
    const ok = await probeUrl(origin);
    if (ok) { espUrl.value = ok; localStorage.setItem('espUrl', ok); setStatus('ESP autodetectado (origin)'); return; }
  } catch(e){}
  try {
    const m = await probeUrl('http://esp32.local');
    if (m) { espUrl.value = m; localStorage.setItem('espUrl', m); setStatus('ESP autodetectado (mDNS esp32.local)'); return; }
  } catch(e){}
  const last = localStorage.getItem('espUrl');
  if (last) {
    const ok = await probeUrl(last);
    if (ok) { espUrl.value = ok; localStorage.setItem('espUrl', ok); setStatus('ESP autodetectado (lastKnown)'); return; }
  }
  espUrl.placeholder = 'http://<ip_del_esp>/';
  setStatus('No se detecto ESP automaticamente — ingresa IP si es necesario');
}

document.addEventListener('DOMContentLoaded', () => {
  autoDetectEspUrl().catch(e => console.warn('autoDetect error', e));
});

/* Inicialización local */
(function initLocal(){ RETRIES = 3; document.getElementById('retries').innerText = RETRIES; updatePreview(); setStatus('Listo'); })();

/* Polling /nextText */
async function pollPending() {
  try {
    const res = await fetch('/nextText');
    if (res.status === 200) {
      const txt = await res.text();
      if (txt && txt.trim().length > 0) {
        log('Recibido texto remoto, renderizando...');
        src.value = txt; renderBtn.click(); setStatus('Polling: esperando renderizar y enviar...');
        while (sendBtn.disabled) { if (!renderBtn.disabled && sendBtn.disabled) { setStatus('Polling: no se generaron páginas'); return; } await new Promise(r => setTimeout(r, 300)); }
        sendAllBtn.click();
        setStatus('Polling: proceso remoto completado');
      }
    }
  } catch (e) { /* silencioso */ }
}
setInterval(pollPending, 2000);

function decodeSafeB64(s) { if (!s) return null; try { const safe = s.replace(/-/g, '+').replace(/_/g, '/'); const pad = safe.length % 4; let padded = safe; if (pad === 2) padded += '=='; else if (pad === 3) padded += '='; else if (pad === 1) padded += '==='; const bin = atob(padded); let out = ''; for (let i = 0; i < bin.length; i++) out += String.fromCharCode(bin.charCodeAt(i)); try { return decodeURIComponent(escape(out)); } catch (e) { return out; } } catch (e) { console.error('b64 decode failed', e); return null; } }

(function(){
  function decodeSafeB64(s){
    try { s = s.replace(/-/g,'+').replace(/_/g,'/'); while(s.length % 4) s += '='; return decodeURIComponent(escape(atob(s))); }
    catch(e){ console.warn('decodeSafeB64 fallo', e); return null; }
  }
  function setStatus(s){ try{ const el=document.getElementById('status'); if(el) el.innerText=s; } catch(e){} console.log('AUTO:', s); }

  document.addEventListener('DOMContentLoaded', async function(){
    try {
      const params = new URLSearchParams(window.location.search);
      if (!(params.get('a')==='1' || params.get('auto')==='1')) return;

      const b64 = params.get('b64');
      const texto = params.get('texto');

      if (!b64 && !texto) { setStatus('Auto: falta texto'); return; }

      const src = document.getElementById('src');
      const renderBtn = document.getElementById('renderBtn');
      const sendAllBtn = document.getElementById('sendAllBtn');
      const logEl = document.getElementById('log');

      if (!src || !renderBtn) { setStatus('Auto: elementos no encontrados'); return; }

      let final = null;
      if (b64) {
        final = decodeSafeB64(b64);
      } else {
        try { final = decodeURIComponent(texto); } catch(e) { final = texto; }
      }

      if (!final) { setStatus('Auto: decodificacion fallida'); return; }

      src.value = final;
      setStatus('Automatizacion: texto pegado (' + final.length + ' chars)');
      renderBtn.click();
      setStatus('Automatizacion: renderizando...');

      // esperar indicio en log o timeout
      const start = Date.now();
      while (true) {
        if (logEl && (logEl.textContent||'').includes('Generadas')) break;
        if (Date.now() - start > 45000) break;
        await new Promise(r => setTimeout(r, 250));
      }

      // intentar enviar con sendAllBtn si existe y se habilita
      if (sendAllBtn) {
        const t0 = Date.now();
        while (sendAllBtn.disabled && (Date.now() - t0) < 12000) await new Promise(r => setTimeout(r, 250));
        if (!sendAllBtn.disabled) {
          sendAllBtn.click();
          setStatus('Automatizacion: Enviado');
        } else {
          setStatus('Automatizacion: sendAll no habilitado');
        }
      } else {
        setStatus('Automatizacion: boton sendAll no existe');
      }
    } catch (err) {
      console.error('Auto error', err);
      setStatus('Auto: error en consola');
    }
  });
})();
</script>
</body>
</html>
)rawliteral";

  // Reemplazamos el placeholder por la IP actual
  html.replace("__ESP_URL__", ipStr);

  // Evitar cache antiguo (opcional)
  server.sendHeader("Cache-Control","no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma","no-cache");
  server.sendHeader("Expires","0");

  // Enviamos la página ya con la IP actual incrustada
  server.send(200, "text/html", html);


  // Reemplazamos el placeholder por la IP actual
  html.replace("__ESP_URL__", ipStr);

  // Evitar cache antiguo (opcional)
  server.sendHeader("Cache-Control","no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma","no-cache");
  server.sendHeader("Expires","0");

  // Enviamos la página ya con la IP actual incrustada
  server.send(200, "text/html", html);

}

void handleUpload() {
  HTTPUpload& upload = server.upload();
  switch (upload.status) {
    case UPLOAD_FILE_START:
      currentFilenameUpload = "/" + upload.filename;
      Serial.printf("📁 Iniciando subida: %s\n", currentFilenameUpload.c_str());
      uploadFile = SPIFFS.open(currentFilenameUpload, FILE_WRITE);
      if (!uploadFile) { Serial.println("❌ No se pudo crear el archivo en SPIFFS"); }
      break;
    case UPLOAD_FILE_WRITE:
      if (uploadFile) uploadFile.write(upload.buf, upload.currentSize);
      break;
    case UPLOAD_FILE_END:
      if (uploadFile) {
        uploadFile.close();
        Serial.printf("✅ Archivo %s recibido, mostrando...\n", currentFilenameUpload.c_str());
        imgOffsetX = imgOffsetY = 0; clampImageOffsets();
        displayJpgFile(currentFilenameUpload.c_str());
      }
      server.sendHeader("Location", "/");
      server.send(303);
      break;
    case UPLOAD_FILE_ABORTED:
      Serial.printf("⚠️ Subida abortada: %s\n", currentFilenameUpload.c_str());
      if (uploadFile) uploadFile.close();
      if (currentFilenameUpload.length() && SPIFFS.exists(currentFilenameUpload)) {
        SPIFFS.remove(currentFilenameUpload);
        Serial.println("🧹 Archivo parcial eliminado");
      }
      break;
    default:
      break;
  }
}

void handleApiRender() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "POST, OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
  if (server.method() == HTTP_OPTIONS) { server.send(200); return; }
  if (server.method() != HTTP_POST) { server.send(405, "application/json", "{\"status\":\"error\",\"message\":\"Método no permitido\"}"); return; }
  String payload = "";
  if (server.hasArg("plain") && server.arg("plain").length() > 0) { payload = server.arg("plain"); }
  else if (server.hasArg("text") && server.arg("text").length() > 0) { payload = server.arg("text"); }
  else if (server.args() > 0) { payload = server.arg(0); }
  else { WiFiClient client = server.client(); unsigned long start = millis(); while (!client.available() && (millis() - start) < 200) delay(1); if (client.available()) { payload = client.readString(); } }
  if (payload.length() == 0) { server.send(400, "application/json", "{\"status\":\"error\",\"message\":\"No se recibió texto\"}"); return; }
  File f = SPIFFS.open("/pending.html", FILE_WRITE);
  if (!f) { server.send(500, "application/json", "{\"status\":\"error\",\"message\":\"SPIFFS open failed\"}"); return; }
  f.print(payload); f.close(); server.send(200, "application/json", "{\"status\":\"ok\",\"message\":\"Texto recibido\"}");
}

void handleNextText() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!SPIFFS.exists("/pending.html")) { server.send(204, "text/plain", ""); return; }
  File f = SPIFFS.open("/pending.html", FILE_READ);
  if (!f) { server.send(500, "text/plain", "SPIFFS read failed"); return; }
  String txt = f.readString(); f.close(); SPIFFS.remove("/pending.html"); server.send(200, "text/plain", txt);
}

void handleShow() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (!server.hasArg("file")) {
    server.send(400, "text/plain", "Use ?file=page_001.jpg");
    return;
  }
  String fname = "/" + server.arg("file");
  if (!SPIFFS.exists(fname)) {
    server.send(404, "text/plain", "not found");
    return;
  }
  bool useDefaults = true;
  int xOff = 0;
  int yOff = 0;
  if (server.hasArg("x") || server.hasArg("y")) {
    useDefaults = false;
    if (server.hasArg("x")) xOff = server.arg("x").toInt();
    if (server.hasArg("y")) yOff = server.arg("y").toInt();
  }
  displayJpgFile(fname.c_str(), xOff, yOff, useDefaults);
  server.send(200, "text/plain", "Shown " + fname + (useDefaults ? " (using defaults)" : " (using provided x/y)"));
}

void handleNext() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  currentPageIndex++; char fname[32]; sprintf(fname, "/page_%03d.jpg", currentPageIndex);
  if (!SPIFFS.exists(fname)) { currentPageIndex--; server.send(404, "text/plain", "No hay más páginas"); return; }
  displayJpgFile(fname); server.send(200, "text/plain", String("Página ") + currentPageIndex);
}

void handlePrev() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (currentPageIndex > 1) currentPageIndex--;
  char fname[32]; sprintf(fname, "/page_%03d.jpg", currentPageIndex);
  if (!SPIFFS.exists(fname)) { server.send(404, "text/plain", "Página no encontrada"); return; }
  displayJpgFile(fname); server.send(200, "text/plain", String("Página ") + currentPageIndex);
}

void handleClear() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  tft.fillScreen(TFT_BLACK);
  server.send(200, "text/plain", "Pantalla limpia");
}

void handleSetOffset() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  if (server.hasArg("x")) displayDefaultX = server.arg("x").toInt();
  if (server.hasArg("y")) displayDefaultY = server.arg("y").toInt();
  if (server.hasArg("base")) {
    String b = server.arg("base");
    b.toLowerCase();
    if (b == "topleft" || b == "top" || b == "0") displayBaseMode = 1;
    else if (b == "center" || b == "centre" || b == "1") displayBaseMode = 0;
    if (b == "0") displayBaseMode = 1;
    if (b == "1") displayBaseMode = 0;
  }
  File f = SPIFFS.open("/display.cfg", FILE_WRITE);
  if (f) { f.printf("%d,%d,%d", displayDefaultX, displayDefaultY, displayBaseMode); f.close(); }
  String json = "{\"status\":\"ok\",\"x\":" + String(displayDefaultX) + ",\"y\":" + String(displayDefaultY) + ",\"base\":" + String(displayBaseMode) + "}";
  server.send(200, "application/json", json);
}

void handleGetOffset() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  String json = "{\"x\":" + String(displayDefaultX) + ",\"y\":" + String(displayDefaultY) + ",\"base\":" + String(displayBaseMode) + "}";
  server.send(200, "application/json", json);
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  delay(250);
  mySpi.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  ts.begin(mySpi); ts.setRotation(1);
  tft.init(); tft.setRotation(1); tft.fillScreen(TFT_BLACK);
  scaleX = (float)tft.width() / (float)V_W; scaleY = (float)tft.height() / (float)V_H;
  Serial.printf("Display real: %dx%d  scaleX=%f scaleY=%f  displayDefaultY=%d\n", tft.width(), tft.height(), scaleX, scaleY, displayDefaultY);

  // SPIFFS
  if (!SPIFFS.begin(true)) {
    Serial.println("❌ SPIFFS mount failed");
  } else {
    Serial.println("✅ SPIFFS mounted");
  }

  loadMapping();
  if (!validateMapping()) {
    Serial.println("Mapping invalido o ausente -> intentando calibracion segura.");
    doCalibrationInteractive();
  }

  // Colores: uniforme gris oscuro para botones (sobrio)
  colRun = COLOR_ACCENT;
  colCounter = COLOR_ACCENT_2;
  colCalibrate = COLOR_BUTTON_BG;
  colPlus = colMinus = COLOR_BUTTON_BG;
  colNumberBg = COLOR_STATUS_BG; // caja numero mas oscura

  recomputeRects();

  connectWiFi();

  // mDNS
  if (MDNS.begin("esp32")) {
    Serial.println("mDNS iniciado: esp32.local");
    MDNS.addService("http", "tcp", 80);
  } else {
    Serial.println("mDNS fallo");
  }

  // server routes
  server.on("/", HTTP_GET, handleRoot);
  server.on("/whoami", HTTP_GET, handleWhoAmI);
  server.on("/upload", HTTP_POST, [](){ server.send(200); }, handleUpload);
  server.on("/pushText", HTTP_POST, handleApiRender);
  server.on("/pushText", HTTP_OPTIONS, [](){ server.sendHeader("Access-Control-Allow-Origin","*"); server.sendHeader("Access-Control-Allow-Methods","POST,OPTIONS"); server.sendHeader("Access-Control-Allow-Headers","Content-Type"); server.send(200); });
  server.on("/nextText", HTTP_GET, handleNextText);
  server.on("/show", HTTP_GET, handleShow);
  server.on("/next", HTTP_GET, handleNext);
  server.on("/prev", HTTP_GET, handlePrev);
  server.on("/clear", HTTP_GET, handleClear);
  server.on("/setOffset", HTTP_GET, handleSetOffset);
  server.on("/getOffset", HTTP_GET, handleGetOffset);

  server.begin();
  Serial.println("HTTP server started");

  drawHome();
}

// --- REEMPLAZA el cuerpo de handleSideTouches por esto ---
// --- REEMPLAZAR: handler lateral (desactivado en COUNTER) ---

// ---- Versión canonical y segura de handleSideTouches ----
bool handleSideTouches(int sx, int sy) {
  // Evita navegación lateral mientras estamos en el contador
  if (screen == COUNTER) return false;

  unsigned long now = millis();
  // debounce para evitar múltiples activaciones rápidas
  if (now - lastSideSendMillis < SIDE_SEND_DEBOUNCE_MS) return false;

  // Zona izquierda -> pagina anterior
  if (ptInRect(sx, sy, btnLeftZone)) {
    lastSideSendMillis = now;
    Serial.println("Side touch: PREV -> accion interna");
    doPrevPage(); // ya existe en el sketch y llama al handler /prev internamente
    // espera liberacion para evitar reentradas accidentales
    while (ts.touched()) { server.handleClient(); delay(6); }
    return true;
  }

  // Zona derecha -> pagina siguiente
  if (ptInRect(sx, sy, btnRightZone)) {
    lastSideSendMillis = now;
    Serial.println("Side touch: NEXT -> accion interna");
    doNextPage(); // ya existe en el sketch y llama al handler /next internamente
    while (ts.touched()) { server.handleClient(); delay(6); }
    return true;
  }

  return false;
}


void loop() {
  // keep serving clients
  server.handleClient();

  if (screen == IMAGE_VIEWER) {
    handleImageViewerTouch();
    return;
  }

  // Si no hay toque, nada que hacer
  if (!ts.touched()) { delay(6); return; }

  // Lectura puntual del touch y mapeo a pantalla
  TS_Point p = ts.getPoint();
  int sx, sy;
  mapRawToScreen(p.x, p.y, sx, sy);
  Serial.printf("Touch rawX=%d rawY=%d -> sx=%d sy=%d\n", p.x, p.y, sx, sy);

  // comprobar zonas laterales primero
  if (handleSideTouches(sx, sy)) {
    delay(140);
    while (ts.touched()) { server.handleClient(); delay(6); }
    return;
  }

  // Determinar si el punto inicial esta sobre algun control (usando pad para ser tolerante)
 // Determinar si el punto inicial esta sobre algun control (hit-area interior para evitar accidentes)
int touchedButton = -1;

if (screen == HOME) {
  // Para HOME usamos un rect *interior* (inset) — solo clicks bien centrados activan el boton.
  if (ptInRectInner(sx, sy, btnRun, HOME_HIT_INSET)) touchedButton = 0;
  else if (ptInRectInner(sx, sy, btnCounter, HOME_HIT_INSET)) touchedButton = 1;
  else if (ptInRectInner(sx, sy, btnImageMode, HOME_HIT_INSET)) touchedButton = 2;
} else {
  // En pantalla COUNTER usamos un inset menor para +, -, number
  if (ptInRectInner(sx, sy, btnPlus, COUNTER_HIT_INSET)) touchedButton = 3;
  else if (ptInRectInner(sx, sy, btnMinus, COUNTER_HIT_INSET)) touchedButton = 4;
  else if (ptInRectInner(sx, sy, btnNumber, COUNTER_HIT_INSET)) touchedButton = 5;
}


  if (touchedButton == -1) {
    // No era un boton: espera a que termine el toque y vuelve
    delay(140);
    while (ts.touched()) { server.handleClient(); delay(6); }
    return;
  }

  // Registramos estado de presion inicial y damos feedback visual (no ejecutamos accion aun)
  activeButtonId = touchedButton;
  activeButtonRect = (activeButtonId == 0) ? btnRun
                 : (activeButtonId == 1) ? btnCounter
                 : (activeButtonId == 2) ? btnImageMode
                 : (activeButtonId == 3) ? btnPlus
                 : (activeButtonId == 4) ? btnMinus
                 : btnNumber;
  activePressMs = millis();

  // feedback visual inmediato (no activar la accion)
  switch (activeButtonId) {
    case 0: flashButtonLocal(activeButtonRect, "RUN", colRun); break;
    case 1: flashButtonLocal(activeButtonRect, "Contador", colCounter); break;
    case 2: flashButtonLocal(activeButtonRect, "IMG", colCalibrate); break;
    case 3: flashButtonLocal(activeButtonRect, "+", colPlus); break;
    case 4: flashButtonLocal(activeButtonRect, "-", colMinus); break;
    case 5: flashButtonLocal(activeButtonRect, "Enviar", colNumberBg); break;
  }

  // Esperar a que termine el toque (usando readFilteredTouch con confirmacion multiple)
  int last_sx = sx, last_sy = sy;
  int releaseConfirm = 0;
  while (true) {
    bool touchingNow = readFilteredTouch(last_sx, last_sy);
    if (touchingNow) {
      releaseConfirm = 0;
    } else {
      releaseConfirm++;
      if (releaseConfirm >= RELEASE_CONFIRM_COUNT) break;
    }
    server.handleClient();
    delay(8);
  }

  // Al soltarse (confirmado), chequear duracion y ubicacion final
  unsigned long dur = millis() - activePressMs;
  int relx = _touch_lastX;
  int rely = _touch_lastY;

  // Si la liberacion cae dentro del rect (con padding) y no fue un long-press -> activar tap
  bool releasedInside = ptInRectPad(relx, rely, activeButtonRect, TAP_PAD_PIXELS);
  if (releasedInside && dur < TAP_MAX_DURATION_MS) {
    // Ejecutar la accion segun el boton
    switch (activeButtonId) {
      case 0: doPostRun(); break;
      case 1: screen = COUNTER; drawCounterFull(); break;
      case 2: imgOffsetX = imgOffsetY = 0; clampImageOffsets(); drawImageViewer(); break;
      case 3: counterValue++; updateNumberDisplay(); break;
      case 4: counterValue--; updateNumberDisplay(); break;
      case 5: doPostTrigger(counterValue); delay(180); screen = HOME; drawHome(); break;
    }
  }

  // limpiar estado y asegurar que no quede touch activo
  activeButtonId = -1;
  activePressMs = 0;

  delay(40); // pequeño retraso para estabilidad
  while (ts.touched()) { server.handleClient(); delay(6); }
}
