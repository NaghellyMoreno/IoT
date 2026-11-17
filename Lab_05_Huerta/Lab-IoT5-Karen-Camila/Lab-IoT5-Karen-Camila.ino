/*
 * ==========================================================================
 *  LABORATORIO IoT #5 - Nodo IoT supervisado por Telegram
 *  Caso de aplicación: huerta escolar supervisada a distancia
 *  Integrantes: Karen - Camila
 *  Placa: ESP32 (DevKit v1 / WROOM-32)
 * ==========================================================================
 *
 *  PAPELES DE LOS COMPONENTES
 *   Variable medida ........ Temperatura con DHT11 (DATA en GPIO4)
 *                            -> se reporta en /estado;
 *                               ALERTA cuando la temperatura SUPERA el umbral
 *                               (calor excesivo para las plantas)
 *   Entrada de evento ...... Nivel del tanque (HC-SR04) TRIG GPIO27, ECHO GPIO32
 *                            -> ALERTA inmediata cuando el tanque queda bajo
 *   Mando local ............ Lector RFID RC522 (SPI)
 *                            -> tarjeta autorizada: abre/cierra la válvula
 *                            -> tarjeta NO autorizada: ALERTA de acceso
 *   Salida 1 (remota) ...... Servomotor (válvula de llenado) GPIO13 + LED verde GPIO25
 *                            -> SOLO se abre si el tanque está bajo; se cierra
 *                               sola cuando el nivel deja de ser bajo
 *   Salida 2 (local) ....... Buzzer PASIVO GPIO33 (tono PWM) + LED rojo GPIO26
 *                            -> se activa sola mientras haya una alerta
 *
 *  COMUNICACIÓN POR WIFI (TCP)
 *   El ESP32 se conecta a la red WiFi y abre un servidor TCP en el puerto
 *   PUERTO_TCP. Python se conecta como cliente (IP que imprime el Monitor
 *   Serie al arrancar, o el nombre "huerta.local" por mDNS). Un solo cliente
 *   a la vez; si llega otro, reemplaza al anterior. Todo lo que se envía al
 *   cliente se copia también al Monitor Serie (solo para depurar).
 *   Las alertas generadas sin cliente conectado no se almacenan.
 *
 *  PROTOCOLO (una línea por mensaje, termina en \n; igual que por serie)
 *   Órdenes de Python (un carácter):
 *     1 = encender salida 1     0 = apagar salida 1     E = estado
 *     S = silenciar salida 2    R = resumen (mín./máx.)
 *     V = vaciar: borra el estado "tanque lleno" guardado y reactiva las
 *         alertas de nivel bajo (ERR;TANQUE_NO_LLENO si no estaba lleno)
 *   Respuestas:
 *     OK;TEMP=24;NIVEL=80;TANQUE=OK|BAJO|LLENO;EVENTO=0;S1=ON;S2=OFF;SIL=NO
 *     OK;RESUMEN;TEMP_MIN=..;TEMP_MAX=..;NIVEL_MIN=..;NIVEL_MAX=..
 *     ERR;ORDEN_DESCONOCIDA      (no cambia ninguna salida)
 *     ERR;TANQUE_NO_BAJO;S1=OFF  (la válvula solo abre si el tanque está bajo)
 *   Mensajes espontáneos (por iniciativa del ESP32):
 *     ALERTA;UMBRAL;TEMP=35
 *     ALERTA;TANQUE_BAJO;NIVEL=15
 *     ALERTA;TARJETA_NO_AUTORIZADA;UID=A1 B2 C3 D4
 *     AVISO;MANDO_LOCAL;S1=ON;UID=...     AVISO;S1_APAGADA_TANQUE_NO_BAJO
 *     AVISO;TANQUE_LLENO;NIVEL=95
 *     AVISO;INICIO
 *
 *  INTERRUPCIONES Y SERVO (igual que el laboratorio 4)
 *   - Timer por hardware cada 10 ms: marca los tiempos (sin delay()).
 *   - Interrupción en ECHO: mide el ultrasónico sin pulseIn().
 *   - ESP32Servo: pulso del servo por hardware (LEDC), sin jitter.
 *   - El tono del buzzer pasivo también sale por LEDC (clase ESP32PWM), en
 *     un temporizador distinto al del servo: ninguno afecta al otro.
 *
 *   - El DHT11 bloquea las interrupciones ~5 ms al leer: se lee cada 2 s,
 *     justo después de un eco del ultrasónico, para no dañar esa medición.
 *
 *  LIBRERÍAS (Gestor de Librerías): "ESP32Servo", "MFRC522" (GithubCommunity)
 *  y "DHT sensor library" de Adafruit (+ "Adafruit Unified Sensor")
 * ==========================================================================
 */

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <DHT.h>
#include <Preferences.h>
#include "driver/gpio.h"

// --------------------------------------------------------------------------
// Pines
// --------------------------------------------------------------------------
const int PIN_DHT        = 4;    // DATA del DHT11
const int PIN_TRIG       = 27;   // HC-SR04 TRIG
const int PIN_ECHO       = 32;   // HC-SR04 ECHO (¡con divisor 5V -> 3.3V!)
const int PIN_RFID_SS    = 5;    // RC522 SDA/SS
const int PIN_RFID_RST   = 22;   // RC522 RST
// RC522 por SPI (VSPI): SCK GPIO18, MISO GPIO19, MOSI GPIO23
const int PIN_SERVO      = 13;   // Salida 1: válvula
const int PIN_LED_S1     = 25;   // Salida 1: LED verde
const int PIN_BUZZER     = 33;   // Salida 2: buzzer pasivo (tono PWM)
const int PIN_LED_ALERTA = 26;   // Salida 2: LED rojo

const long BAUDIOS = 115200;   // solo Monitor Serie (depuración)

// WiFi (la red debe ser 2.4 GHz y estar en la misma LAN que el computador)
const char *WIFI_SSID = "Redmi Note 14 Pro";
const char *WIFI_PASS = "Karen123";
const uint16_t PUERTO_TCP = 8888;
const char *NOMBRE_MDNS = "huerta";   // -> huerta.local

// Buzzer pasivo: frecuencia del tono. Entre 2000 y 4000 Hz suena más fuerte;
// prueben valores y dejen el que se oiga mejor.
const int FREC_BUZZER_HZ = 2700;

// Nivel de reposo: true = silencio con el pin en LOW (buzzer suelto o
// módulo normal). Cambiar a false si el módulo se calienta o zumbea en
// reposo (módulos que se activan con LOW).
const bool BUZZER_ACTIVO_EN_ALTO = true;

// --------------------------------------------------------------------------
// PARÁMETROS (justificar con mediciones en el informe)
// --------------------------------------------------------------------------
// Variable medida: temperatura del DHT11 (°C)
// Calibración: midan la temperatura ambiente y la que alcanza al tocar el
// sensor con la mano. Sugerencia: ALTA ≈ ambiente + 4 °C, REARME ≈ ambiente + 2 °C.
const int UMBRAL_TEMP_ALTA   = 32;  // >= 32 °C: ALERTA de temperatura alta
const int UMBRAL_TEMP_REARME = 30;  // <= 30 °C: la alerta se rearma

// Entrada de evento: nivel del tanque
const float DIST_TANQUE_LLENO_CM = 5.0;
const float DIST_TANQUE_VACIO_CM = 30.0;
const int   NIVEL_MIN_PCT        = 20;   // <= 20 %: ALERTA de tanque bajo
const int   NIVEL_REARME_PCT     = 30;   // >= 30 %: el evento termina
const int   NIVEL_LLENO_PCT      = 90;   // >= 90 %: AVISO de tanque lleno
const int   NIVEL_LLENO_REARME   = 85;   // <= 85 %: tras vaciar, se permite marcar lleno de nuevo
const int   LECTURAS_LLENO_CONFIRMA = 5; // 5 lecturas seguidas (1 s) para dar el tanque por lleno

// Mando local: tarjetas autorizadas (UID en hexadecimal con espacios).
// Para conocer el UID de su tarjeta: acérquenla una vez; como no está en la
// lista, llegará "ALERTA;TARJETA_NO_AUTORIZADA;UID=..." -> copien ese UID aquí.
const char *UID_AUTORIZADAS[] = {
  "AD B4 AB 00",
};
const int N_UID = sizeof(UID_AUTORIZADAS) / sizeof(UID_AUTORIZADAS[0]);

const uint32_t DURACION_ALERTA_RFID_MS = 5000;  // salida 2 activa 5 s
const uint32_t REPETICION_TARJETA_MS   = 2000;  // ignora la misma tarjeta 2 s

// Servo
const int SERVO_CERRADO = 0;
const int SERVO_ABIERTO = 90;
const int SERVO_PASO_GRADOS = 3;      // grados por paso (cada 20 ms): 90° en 0.6 s

// Tiempos (múltiplos del tick de 10 ms)
const uint32_t TICK_US        = 10000;
const uint32_t TICKS_TRIGGER  = 6;    // ultrasónico cada 60 ms
const uint32_t TICKS_LECTURA  = 20;   // lectura y alertas cada 200 ms
const uint32_t TICKS_RFID     = 10;   // RFID cada 100 ms
const uint32_t TICKS_PARPADEO = 25;   // buzzer intermitente cada 250 ms
const uint32_t TICKS_SERVO    = 2;    // paso del servo cada 20 ms
const uint32_t TICKS_DHT      = 200;  // DHT11 cada 2 s (máx. 1 lectura/s)
const uint32_t TIMEOUT_ECO_MS = 500;
const uint32_t TIMEOUT_DHT_MS = 6000; // sin lectura del DHT11 -> TEMP=NA
const uint32_t ESPERA_MAX_DHT_MS = 200;

// --------------------------------------------------------------------------
// Objetos y variables
// --------------------------------------------------------------------------
Preferences memoria;   // memoria no volátil: recuerda si el tanque quedó lleno
Servo    valvula;
ESP32PWM buzzer;     // genera el tono del buzzer pasivo por hardware
MFRC522 rfid(PIN_RFID_SS, PIN_RFID_RST);
DHT     dht(PIN_DHT, DHT11);

// Enlace con Python: servidor TCP. Todo lo que se escribe aquí va al cliente
// conectado (si lo hay) y se copia al Monitor Serie.
WiFiServer servidor(PUERTO_TCP);
WiFiClient cliente;

class Enlace : public Print {
 public:
  size_t write(uint8_t b) override {
    Serial.write(b);
    if (cliente && cliente.connected()) cliente.write(b);
    return 1;
  }
  size_t write(const uint8_t *buf, size_t n) override {
    Serial.write(buf, n);
    if (cliente && cliente.connected()) cliente.write(buf, n);
    return n;
  }
};
Enlace enlace;

hw_timer_t  *timerControl = nullptr;
portMUX_TYPE muxTimer = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t ticks = 0;
volatile bool flagTrigger = false, flagLectura = false,
              flagRfid = false, flagParpadeo = false, flagDht = false,
              flagServo = false;

portMUX_TYPE muxEcho = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t tSubidaEco = 0, anchoEcoUs = 0;
volatile bool     ecoNuevo   = false;

// Mediciones
int   temperaturaC = 0;
bool  temperaturaValida = false;
uint32_t tUltimoDht = 0;
bool  dhtPendiente  = false;
uint32_t tDhtPedido = 0;
float distanciaCm = -1.0;
int   nivelPct    = 0;
bool  nivelValido = false;
uint32_t tUltimoEco = 0;

// Resumen (reto opcional)
int tempMin = 100, tempMax = -40, nivelMin = 100, nivelMax = 0;

// Alertas
bool alertaUmbral = false;     // temperatura alta
bool alertaTanque = false;     // tanque bajo (entrada de evento)
bool tanqueLleno  = false;     // tanque lleno: GUARDADO hasta la orden V (vaciar)
bool llenoArmado  = true;      // false tras vaciar, hasta que el nivel baje a ≤ 85 %
int  contLleno    = 0;         // lecturas seguidas ≥ 90 %
uint32_t tFinAlertaRfid = 0;   // tarjeta no autorizada (temporal)
bool silenciado = false;       // /silenciar

// Salidas
bool s1On = false;
bool s2On = false;
bool fasePitido = false;

// RFID
String   ultimoUid = "";
uint32_t tUltimaTarjeta = 0;

// ==========================================================================
// INTERRUPCIONES
// ==========================================================================
void IRAM_ATTR isrTimer() {
  portENTER_CRITICAL_ISR(&muxTimer);
  ticks++;
  if (ticks % TICKS_TRIGGER  == 0) flagTrigger  = true;
  if (ticks % TICKS_LECTURA  == 0) flagLectura  = true;
  if (ticks % TICKS_RFID     == 0) flagRfid     = true;
  if (ticks % TICKS_PARPADEO == 0) flagParpadeo = true;
  if (ticks % TICKS_SERVO    == 0) flagServo    = true;
  if (ticks % TICKS_DHT      == 0) flagDht      = true;
  portEXIT_CRITICAL_ISR(&muxTimer);
}

void IRAM_ATTR isrEcho() {
  uint32_t ahora = micros();
  portENTER_CRITICAL_ISR(&muxEcho);
  if (gpio_get_level((gpio_num_t)PIN_ECHO)) {
    tSubidaEco = ahora;
  } else {
    anchoEcoUs = ahora - tSubidaEco;
    ecoNuevo   = true;
  }
  portEXIT_CRITICAL_ISR(&muxEcho);
}

// ==========================================================================
// AUXILIARES
// ==========================================================================
bool tomarBandera(volatile bool &bandera) {
  portENTER_CRITICAL(&muxTimer);
  bool valor = bandera;
  bandera = false;
  portEXIT_CRITICAL(&muxTimer);
  return valor;
}

float aPorcentaje(float valor, float valor0, float valor100) {
  return constrain((valor - valor0) * 100.0 / (valor100 - valor0), 0.0, 100.0);
}

void dispararUltrasonico() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);
}

// Devuelve true si llegó un eco nuevo (ventana libre para leer el DHT11)
bool procesarEco() {
  portENTER_CRITICAL(&muxEcho);
  bool nuevo = ecoNuevo;
  uint32_t ancho = anchoEcoUs;
  ecoNuevo = false;
  portEXIT_CRITICAL(&muxEcho);
  if (!nuevo) return false;

  float d = ancho * 0.0343 / 2.0;
  if (d >= 2.0 && d <= 400.0) {
    distanciaCm = (distanciaCm < 0) ? d : (0.7 * distanciaCm + 0.3 * d);
    tUltimoEco  = millis();
  }
  return true;
}

// Lectura del DHT11 (~25 ms). Si falla, conserva el último valor válido.
void leerDht() {
  float t = dht.readTemperature();
  if (!isnan(t)) {
    temperaturaC = (int)(t + 0.5);
    tUltimoDht = millis();
  }
}

bool alertaRfidActiva() {
  return (tFinAlertaRfid != 0) && ((int32_t)(tFinAlertaRfid - millis()) > 0);
}

bool hayAlerta() {
  return alertaUmbral || alertaTanque || alertaRfidActiva();
}

// Guarda en flash (NVS) si el tanque quedó lleno; solo se escribe al cambiar.
void guardarLleno() {
  memoria.putBool("lleno", tanqueLleno);
}

// La válvula (llenado) solo puede estar abierta si el tanque está bajo y la
// lectura es confiable (sin lectura del ultrasónico no se abre, para no desbordar).
bool puedeAbrirValvula() {
  return alertaTanque && nivelValido;
}

// Salida 1: servo + LED verde (solo actúa cuando cambia).
// El servo NO se mueve de golpe: ponerS1() solo fija el ángulo objetivo y el
// timer (flagServo, cada 20 ms) lo acerca poco a poco desde el lazo principal.
// Así no hay delay(), el pulso lo genera el hardware (LEDC) y el pico de
// corriente del motor es menor (evita reinicios del ESP32 con WiFi activo).
int servoAngulo   = SERVO_CERRADO;
int servoObjetivo = SERVO_CERRADO;

void ponerS1(bool encender) {
  if (encender == s1On) return;
  s1On = encender;
  servoObjetivo = s1On ? SERVO_ABIERTO : SERVO_CERRADO;
  digitalWrite(PIN_LED_S1, s1On ? HIGH : LOW);
}

void moverServo() {
  if (servoAngulo == servoObjetivo) return;
  if (servoAngulo < servoObjetivo) servoAngulo = min(servoAngulo + SERVO_PASO_GRADOS, servoObjetivo);
  else                             servoAngulo = max(servoAngulo - SERVO_PASO_GRADOS, servoObjetivo);
  valvula.write(servoAngulo);
}

// Buzzer pasivo: onda cuadrada al 50 % = tono; ciclo fijo = silencio
bool buzzerSonando = false;
void buzzerSonar(bool sonar) {
  if (sonar == buzzerSonando) return;          // solo cambia si hace falta
  buzzerSonando = sonar;
  if (sonar) buzzer.writeScaled(0.5);
  else       buzzer.writeScaled(BUZZER_ACTIVO_EN_ALTO ? 0.0 : 1.0);
}

// Salida 2: LED rojo fijo + buzzer intermitente mientras haya alerta
void actualizarS2() {
  if (!hayAlerta()) silenciado = false;      // la próxima alerta vuelve a sonar
  s2On = hayAlerta() && !silenciado;
  digitalWrite(PIN_LED_ALERTA, s2On ? HIGH : LOW);
  buzzerSonar(s2On && fasePitido);
}

// ==========================================================================
// MENSAJES
// ==========================================================================
void enviarEstado() {
  enlace.print("OK;TEMP=");
  if (temperaturaValida) enlace.print(temperaturaC); else enlace.print("NA");
  enlace.print(";NIVEL=");
  if (nivelValido) enlace.print(nivelPct); else enlace.print("NA");
  enlace.print(";TANQUE=");    enlace.print(alertaTanque ? "BAJO" : (tanqueLleno ? "LLENO" : "OK"));
  enlace.print(";EVENTO=");    enlace.print(alertaTanque ? 1 : 0);
  enlace.print(";S1=");        enlace.print(s1On ? "ON" : "OFF");
  enlace.print(";S2=");        enlace.print(s2On ? "ON" : "OFF");
  enlace.print(";SIL=");       enlace.println(silenciado ? "SI" : "NO");
}

void enviarResumen() {
  enlace.print("OK;RESUMEN;TEMP_MIN="); enlace.print(tempMin);
  enlace.print(";TEMP_MAX=");           enlace.print(tempMax);
  enlace.print(";NIVEL_MIN=");          enlace.print(nivelMin);
  enlace.print(";NIVEL_MAX=");          enlace.println(nivelMax);
}

// ==========================================================================
// ÓRDENES DESDE PYTHON
// ==========================================================================
void atenderOrdenes() {
  // Cliente nuevo: reemplaza al anterior
  if (servidor.hasClient()) {
    WiFiClient nuevo = servidor.accept();
    if (cliente) cliente.stop();
    cliente = nuevo;
    cliente.setNoDelay(true);
    Serial.println("[wifi] cliente conectado");
  }
  if (!cliente || !cliente.connected()) return;

  while (cliente.available() > 0) {
    char orden = toupper(cliente.read());
    if (orden == '\n' || orden == '\r' || orden == ' ') continue;

    switch (orden) {
      case '1':
        if (!puedeAbrirValvula()) {          // solo abre si el tanque está bajo
          enlace.println("ERR;TANQUE_NO_BAJO;S1=OFF");
        } else {
          ponerS1(true);
          enviarEstado();
        }
        break;
      case '0':
        ponerS1(false);
        enviarEstado();
        break;
      case 'E':
        enviarEstado();
        break;
      case 'S':
        if (hayAlerta()) silenciado = true;
        actualizarS2();
        enviarEstado();
        break;
      case 'R':
        enviarResumen();
        break;
      case 'V':                              // vaciar: borra el estado LLENO
        if (!tanqueLleno) {
          enlace.println("ERR;TANQUE_NO_LLENO");
        } else {
          tanqueLleno = false;
          llenoArmado = false;               // no se re-marca lleno hasta bajar a ≤ 85 %
          contLleno   = 0;
          guardarLleno();
          leerYEvaluar();                    // reevalúa ya: puede haber alerta de nivel bajo
          enviarEstado();
        }
        break;
      default:
        enlace.println("ERR;ORDEN_DESCONOCIDA");  // no cambia nada
        break;
    }
  }
}

// ==========================================================================
// LECTURA PERIÓDICA Y ALERTAS (cada 200 ms)
// ==========================================================================
void leerYEvaluar() {
  // ---- Variable medida: temperatura (último valor leído del DHT11) ----
  temperaturaValida = (tUltimoDht != 0) && (millis() - tUltimoDht < TIMEOUT_DHT_MS);

  // ---- Entrada de evento: nivel del tanque ----
  nivelValido = (distanciaCm > 0) && (millis() - tUltimoEco < TIMEOUT_ECO_MS);
  if (nivelValido) {
    nivelPct = (int)aPorcentaje(distanciaCm, DIST_TANQUE_VACIO_CM, DIST_TANQUE_LLENO_CM);
  }

  // Resumen
  if (temperaturaValida) {
    tempMin = min(tempMin, temperaturaC);
    tempMax = max(tempMax, temperaturaC);
  }
  if (nivelValido) {
    nivelMin = min(nivelMin, nivelPct);
    nivelMax = max(nivelMax, nivelPct);
  }

  // ---- Alerta de umbral (una sola vez por evento, con histéresis) ----
  // (sin lectura válida del DHT11 la alerta conserva su estado)
  bool antesUmbral = alertaUmbral;
  if (temperaturaValida) {
    if (temperaturaC >= UMBRAL_TEMP_ALTA)        alertaUmbral = true;
    else if (temperaturaC <= UMBRAL_TEMP_REARME) alertaUmbral = false;
  }

  if (alertaUmbral && !antesUmbral) {
    enlace.print("ALERTA;UMBRAL;TEMP=");
    enlace.println(temperaturaC);
  }

  // ---- Tanque lleno: el estado se GUARDA (también en flash) ----
  // Una vez lleno queda así hasta que llegue la orden V (vaciar). Se exigen
  // varias lecturas seguidas ≥ 90 % para que un eco falso no lo marque lleno.
  // Tras vaciar, solo vuelve a marcarse lleno si antes el nivel bajó a ≤ 85 %.
  if (nivelValido && nivelPct <= NIVEL_LLENO_REARME) llenoArmado = true;
  if (!tanqueLleno && llenoArmado && nivelValido && nivelPct >= NIVEL_LLENO_PCT) {
    if (++contLleno >= LECTURAS_LLENO_CONFIRMA) {
      tanqueLleno = true;
      guardarLleno();
      enlace.print("AVISO;TANQUE_LLENO;NIVEL=");
      enlace.println(nivelPct);
    }
  } else {
    contLleno = 0;
  }

  // ---- Alerta de tanque bajo (sin lectura = tanque bajo, por seguridad) ----
  // Mientras el tanque esté guardado como LLENO no hay alertas de nivel bajo:
  // solo vuelven después de vaciarlo con la orden V.
  bool antesTanque = alertaTanque;
  if (tanqueLleno)                               alertaTanque = false;
  else if (!nivelValido || nivelPct <= NIVEL_MIN_PCT) alertaTanque = true;
  else if (nivelPct >= NIVEL_REARME_PCT)         alertaTanque = false;

  if (alertaTanque && !antesTanque) {
    enlace.print("ALERTA;TANQUE_BAJO;NIVEL=");
    if (nivelValido) enlace.println(nivelPct); else enlace.println("SIN_LECTURA");
  }

  // Protección: si el tanque deja de estar bajo (ya se llenó) o se pierde la
  // lectura, la válvula abierta se cierra sola
  if (s1On && !puedeAbrirValvula()) {
    ponerS1(false);
    enlace.println("AVISO;S1_APAGADA_TANQUE_NO_BAJO");
  }

  actualizarS2();
}

// ==========================================================================
// MANDO LOCAL: RFID
// ==========================================================================
String uidATexto(const MFRC522::Uid &uid) {
  String texto = "";
  for (byte i = 0; i < uid.size; i++) {
    if (i > 0) texto += " ";
    if (uid.uidByte[i] < 0x10) texto += "0";
    texto += String(uid.uidByte[i], HEX);
  }
  texto.toUpperCase();
  return texto;
}

bool uidAutorizada(const String &uid) {
  for (int i = 0; i < N_UID; i++) {
    if (uid.equalsIgnoreCase(UID_AUTORIZADAS[i])) return true;
  }
  return false;
}

void revisarRfid() {
  if (!rfid.PICC_IsNewCardPresent()) return;
  if (!rfid.PICC_ReadCardSerial())   return;

  String uid = uidATexto(rfid.uid);
  rfid.PICC_HaltA();                  // la tarjeta no se relee mientras siga ahí
  rfid.PCD_StopCrypto1();

  // Evita dobles lecturas de la misma tarjeta
  if (uid == ultimoUid && millis() - tUltimaTarjeta < REPETICION_TARJETA_MS) {
    tUltimaTarjeta = millis();
    return;
  }
  ultimoUid = uid;
  tUltimaTarjeta = millis();

  if (uidAutorizada(uid)) {
    // Mando local: conmuta la salida 1
    if (!s1On && !puedeAbrirValvula()) {
      enlace.print("AVISO;MANDO_LOCAL_RECHAZADO;TANQUE_NO_BAJO;UID=");
      enlace.println(uid);
    } else {
      ponerS1(!s1On);
      enlace.print("AVISO;MANDO_LOCAL;S1=");
      enlace.print(s1On ? "ON" : "OFF");
      enlace.print(";UID=");
      enlace.println(uid);
    }
  } else {
    // Tarjeta desconocida: alerta (cada pasada de tarjeta es un evento nuevo)
    tFinAlertaRfid = millis() + DURACION_ALERTA_RFID_MS;
    if (tFinAlertaRfid == 0) tFinAlertaRfid = 1;
    enlace.print("ALERTA;TARJETA_NO_AUTORIZADA;UID=");
    enlace.println(uid);
    actualizarS2();
  }
}

// ==========================================================================
// CONFIGURACIÓN
// ==========================================================================
void configurarTimer() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  timerControl = timerBegin(1000000);
  timerAttachInterrupt(timerControl, &isrTimer);
  timerAlarm(timerControl, TICK_US, true, 0);
#else
  timerControl = timerBegin(0, 80, true);
  timerAttachInterrupt(timerControl, &isrTimer, true);
  timerAlarmWrite(timerControl, TICK_US, true);
  timerAlarmEnable(timerControl);
#endif
}

void setup() {
  Serial.begin(BAUDIOS);

  // Recupera el estado guardado del tanque (sobrevive a reinicios)
  memoria.begin("huerta", false);
  tanqueLleno = memoria.getBool("lleno", false);
  Serial.print("[mem] tanque guardado como ");
  Serial.println(tanqueLleno ? "LLENO" : "no lleno");

  // WiFi + servidor TCP (espera hasta 20 s; luego sigue reintentando solo)
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[wifi] conectando a ");
  Serial.print(WIFI_SSID);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(500);
    Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\n[wifi] IP: ");
    Serial.println(WiFi.localIP());
    if (MDNS.begin(NOMBRE_MDNS)) MDNS.addService("huerta", "tcp", PUERTO_TCP);
  } else {
    Serial.println("\n[wifi] sin conexión (sigue intentando en segundo plano)");
  }
  servidor.begin();
  servidor.setNoDelay(true);

  // Salidas
  pinMode(PIN_LED_S1, OUTPUT);
  pinMode(PIN_LED_ALERTA, OUTPUT);
  digitalWrite(PIN_LED_S1, LOW);
  digitalWrite(PIN_LED_ALERTA, LOW);

  // DHT11 (variable medida)
  dht.begin();

  // PWM por hardware (LEDC): dos temporizadores, porque el servo (50 Hz)
  // y el buzzer (~2.7 kHz) necesitan frecuencias distintas
  ESP32PWM::allocateTimer(0);
  ESP32PWM::allocateTimer(1);

  // Servo
  valvula.setPeriodHertz(50);
  valvula.attach(PIN_SERVO, 500, 2400);
  valvula.write(SERVO_CERRADO);

  // Buzzer pasivo: arranca en silencio
  buzzer.attachPin(PIN_BUZZER, FREC_BUZZER_HZ, 10);
  buzzer.writeScaled(BUZZER_ACTIVO_EN_ALTO ? 0.0 : 1.0);

  // Ultrasónico con interrupción
  pinMode(PIN_TRIG, OUTPUT);
  digitalWrite(PIN_TRIG, LOW);
  pinMode(PIN_ECHO, INPUT);
  attachInterrupt(digitalPinToInterrupt(PIN_ECHO), isrEcho, CHANGE);

  // RFID por SPI
  SPI.begin();                 // SCK 18, MISO 19, MOSI 23
  rfid.PCD_Init();

  configurarTimer();

  delay(300);                  // solo al arrancar: primeras lecturas del eco
  enlace.println("AVISO;INICIO");
}

// ==========================================================================
// LAZO PRINCIPAL (no bloqueante)
// ==========================================================================
void loop() {
  atenderOrdenes();                                   // órdenes de Python

  if (tomarBandera(flagTrigger)) dispararUltrasonico();
  bool ecoRecibido = procesarEco();

  // DHT11 cada 2 s, justo después de un eco (o tras 200 ms si no hay eco)
  if (tomarBandera(flagDht)) {
    dhtPendiente = true;
    tDhtPedido   = millis();
  }
  if (dhtPendiente && (ecoRecibido || millis() - tDhtPedido > ESPERA_MAX_DHT_MS)) {
    leerDht();
    dhtPendiente = false;
  }

  if (tomarBandera(flagLectura)) leerYEvaluar();      // entradas + alertas
  if (tomarBandera(flagRfid))    revisarRfid();       // mando local

  if (tomarBandera(flagServo)) moverServo();          // servo gradual

  if (tomarBandera(flagParpadeo)) {                   // pitido intermitente
    fasePitido = !fasePitido;
    actualizarS2();
  }
}
