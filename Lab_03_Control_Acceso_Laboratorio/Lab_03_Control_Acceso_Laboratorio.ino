// ============================================================
// LABORATORIO IoT #3 — Sistema de control de acceso
// Versión: ESP32 DevKit (ESP-WROOM-32) — Core 3.3.12
// Materia: Fundamentos de Internet de las Cosas (351G8F)
// ============================================================
// Sensor ultrasónico HC-SR04 + servo SG90 + LEDs + buzzer
// Comunica eventos por Serial (115200) al script Python
// que hace de puente hacia el bot de Telegram.
// ============================================================

#include <ESP32Servo.h>   // DIFERENCIA Uno/ESP32: ESP32Servo en lugar de Servo.h
// Instalar: Gestor de Librerías → "ESP32Servo" por Kevin Harrington, John K. Bennett

// ======================== PINES ==============================
// DIFERENCIA Uno/ESP32: pines distintos, lógica 3.3 V
const int PIN_TRIG       = 26;  // GPIO26 (D26) — disparo HC-SR04
const int PIN_ECHO       = 27;  // GPIO27 (D27) — eco HC-SR04 (con divisor 5V→3.3V)
const int PIN_SERVO      = 25;  // GPIO25 (D25) — señal servo (3.3 V válido para SG90)
const int PIN_LED_VERDE  = 4;   // GPIO4  (D4)  — LED verde
const int PIN_LED_ROJO   = 13;  // GPIO13 (D13) — LED rojo
const int PIN_BUZZER     = 33;  // GPIO33 (D33) — buzzer activo KY-012

// ==================== PARÁMETROS =============================
const float DIST_ACTIVACION   = 30.0;  // cm — distancia de detección
const unsigned long TIEMPO_CIERRE   = 3000;  // ms — espera antes de cerrar
const unsigned long TIEMPO_ALARMA   = 10000; // ms — puerta abierta → alarma
const unsigned long INTERVALO_MEDICION = 100; // ms entre mediciones
const int           NUM_MUESTRAS      = 3;    // promedio de lecturas

// Ángulos del servo
const int ANGULO_CERRADO = 0;
const int ANGULO_ABIERTO = 90;

// =================== MÁQUINA DE ESTADOS ======================
enum EstadoPuerta { CERRADA, ABIERTA, ALARMA };
EstadoPuerta estadoActual = CERRADA;

// ================== VARIABLES GLOBALES =======================
Servo actuador;   // DIFERENCIA Uno/ESP32: mismo nombre, distinta librería

unsigned long tiempoUltimaMedicion = 0;
unsigned long tiempoApertura       = 0;
unsigned long tiempoUltimaPersona  = 0;
float         distanciaActual      = 0.0;
int           contadorAccesos      = 0;

// ============================================================
// FUNCIONES AUXILIARES
// ============================================================

// Mide distancia con el HC-SR04 (devuelve cm; 0 si no hay eco)
float medirDistanciaUnica() {
  digitalWrite(PIN_TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(PIN_TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(PIN_TRIG, LOW);

  unsigned long duracion = pulseIn(PIN_ECHO, HIGH, 30000UL);
  if (duracion == 0) return 0.0;
  return duracion * 0.0343 / 2.0;
}

// Promedio de NUM_MUESTRAS lecturas para filtrar ruido
float medirDistanciaFiltrada() {
  float suma = 0;
  int   validas = 0;
  for (int i = 0; i < NUM_MUESTRAS; i++) {
    float d = medirDistanciaUnica();
    if (d > 0) {
      suma += d;
      validas++;
    }
    if (i < NUM_MUESTRAS - 1) delayMicroseconds(500);
  }
  if (validas == 0) return 0.0;
  return suma / validas;
}

// Detecta si hay una persona dentro del rango de activación
bool personaDetectada(float dist) {
  return (dist > 0 && dist < DIST_ACTIVACION);
}

// Abre la puerta (servo a 90°)
void abrirPuerta() {
  actuador.write(ANGULO_ABIERTO);
}

// Cierra la puerta (servo a 0°)
void cerrarPuerta() {
  actuador.write(ANGULO_CERRADO);
}

// Enciende indicador de acceso permitido
void indicadorAcceso(bool encendido) {
  digitalWrite(PIN_LED_VERDE, encendido ? HIGH : LOW);
}

// Enciende indicador de alarma (LED rojo + buzzer)
void indicadorAlarma(bool encendido) {
  digitalWrite(PIN_LED_ROJO, encendido ? HIGH : LOW);
  digitalWrite(PIN_BUZZER,   encendido ? HIGH : LOW);
}

// Envía un evento por serial (el script Python lo interpreta)
void enviarEvento(const char* evento) {
  Serial.println(evento);
}

// ============================================================
// MÁQUINA DE ESTADOS
// ============================================================

void actualizarEstado() {
  unsigned long ahora = millis();

  switch (estadoActual) {

    // ---------- CERRADA: esperando detección ----------
    case CERRADA:
      if (personaDetectada(distanciaActual)) {
        estadoActual = ABIERTA;
        tiempoApertura = ahora;
        tiempoUltimaPersona = ahora;
        contadorAccesos++;

        abrirPuerta();
        indicadorAcceso(true);
        indicadorAlarma(false);

        Serial.print("ACCESO:");
        Serial.print(distanciaActual, 1);
        Serial.print(":");
        Serial.println(contadorAccesos);
      }
      break;

    // ---------- ABIERTA: puerta abierta, persona cerca ----------
    case ABIERTA:
      if (personaDetectada(distanciaActual)) {
        tiempoUltimaPersona = ahora;
      }

      if (ahora - tiempoApertura > TIEMPO_ALARMA) {
        estadoActual = ALARMA;
        indicadorAcceso(false);
        indicadorAlarma(true);
        enviarEvento("ALARMA");
      }
      else if (ahora - tiempoUltimaPersona > TIEMPO_CIERRE) {
        estadoActual = CERRADA;
        cerrarPuerta();
        indicadorAcceso(false);
        indicadorAlarma(false);
        enviarEvento("CIERRE");
      }
      break;

    // ---------- ALARMA: puerta abierta demasiado tiempo ----------
    case ALARMA:
      if (personaDetectada(distanciaActual)) {
        tiempoUltimaPersona = ahora;
      }

      if (ahora - tiempoUltimaPersona > TIEMPO_CIERRE) {
        estadoActual = CERRADA;
        cerrarPuerta();
        indicadorAcceso(false);
        indicadorAlarma(false);
        enviarEvento("CIERRE");
      }
      break;
  }
}

// ============================================================
// SETUP Y LOOP
// ============================================================

void setup() {
  Serial.begin(115200);  // DIFERENCIA Uno/ESP32: 115200 vs 9600

  pinMode(PIN_TRIG, OUTPUT);
  pinMode(PIN_ECHO, INPUT);
  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_ROJO, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  // DIFERENCIA Uno/ESP32: ESP32Servo usa internamente ledcAttach
  actuador.attach(PIN_SERVO);
  cerrarPuerta();
  indicadorAcceso(false);
  indicadorAlarma(false);

  Serial.println("SISTEMA_LISTO");
  Serial.println("Dist. activacion: " + String(DIST_ACTIVACION) + " cm");
}

void loop() {
  unsigned long ahora = millis();

  // Medir distancia a intervalos regulares (no bloqueante)
  if (ahora - tiempoUltimaMedicion >= INTERVALO_MEDICION) {
    tiempoUltimaMedicion = ahora;
    distanciaActual = medirDistanciaFiltrada();

    // Depuración: imprimir distancia cada 500 ms
    static unsigned long ultimaImpresion = 0;
    if (ahora - ultimaImpresion >= 500) {
      ultimaImpresion = ahora;
      Serial.print("DIST:");
      Serial.println(distanciaActual, 1);
    }
  }

  // Actualizar la máquina de estados
  actualizarEstado();
}
