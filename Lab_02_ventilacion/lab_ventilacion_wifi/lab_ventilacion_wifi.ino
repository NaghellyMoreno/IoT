/*
  Laboratorio IoT #2 - Sistema de ventilación inteligente (versión WiFi, ESP32)

  Igual que la versión serial, pero en lugar de enviar los datos por el cable
  USB, el ESP32 los publica en una ruta web:

      http://IP_DEL_ESP32/datos   ->   T:25.3,H:60.0,E:NORMAL,S:0

  El bot de Python consulta esa ruta cada pocos segundos.

  Librerías (Herramientas -> Administrar bibliotecas):
    - DHT sensor library (Adafruit)
    - Adafruit Unified Sensor
    - ESP32Servo
  (WiFi.h y WebServer.h vienen incluidas con el paquete de la placa ESP32)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DHT.h>
#include <ESP32Servo.h>

// ---------- Red WiFi (solo 2.4 GHz) ----------
const char* WIFI_SSID     = "Redmi Note 14 Pro";
const char* WIFI_PASSWORD = "Karen123";

// ---------- Pines ----------
const int PIN_DHT      = 4;
const int PIN_SERVO    = 18;
const int PIN_VERDE    = 25;
const int PIN_AMARILLO = 26;
const int PIN_ROJO     = 27;
const int PIN_BUZZER   = 14;

// ---------- Configuración ----------
#define TIPO_DHT DHT11            // cambia a DHT22 si es tu sensor

const float T_ELEVADA  = 30.0;    // °C
const float T_CRITICA  = 34.0;    // °C
const float HISTERESIS = 1.0;

const int ANGULO_CERRADO = 0;
const int ANGULO_MEDIO   = 90;
const int ANGULO_ABIERTO = 180;

const unsigned long INTERVALO_MS = 2000;

// ---------- Estados ----------
enum Estado { NORMAL = 0, ELEVADA = 1, CRITICA = 2 };
const char* NOMBRES[] = {"NORMAL", "ELEVADA", "CRITICA"};

DHT dht(PIN_DHT, TIPO_DHT);
Servo ventana;
WebServer server(80);

Estado estadoActual = NORMAL;
int anguloActual = ANGULO_CERRADO;
unsigned long ultimaLectura = 0;
String ultimaLinea = "";          // última lectura válida, lista para enviar

Estado clasificar(float t, Estado actual) {
  switch (actual) {
    case NORMAL:
      if (t >= T_CRITICA) return CRITICA;
      if (t >= T_ELEVADA) return ELEVADA;
      return NORMAL;
    case ELEVADA:
      if (t >= T_CRITICA) return CRITICA;
      if (t < T_ELEVADA - HISTERESIS) return NORMAL;
      return ELEVADA;
    case CRITICA:
      if (t < T_CRITICA - HISTERESIS) return clasificar(t, ELEVADA);
      return CRITICA;
  }
  return NORMAL;
}

void aplicarEstado(Estado e) {
  digitalWrite(PIN_VERDE,    e == NORMAL  ? HIGH : LOW);
  digitalWrite(PIN_AMARILLO, e == ELEVADA ? HIGH : LOW);
  digitalWrite(PIN_ROJO,     e == CRITICA ? HIGH : LOW);
  digitalWrite(PIN_BUZZER,   e == CRITICA ? HIGH : LOW);  // buzzer activo (opcional)

  if (e == NORMAL)       anguloActual = ANGULO_CERRADO;
  else if (e == ELEVADA) anguloActual = ANGULO_MEDIO;
  else                   anguloActual = ANGULO_ABIERTO;
  ventana.write(anguloActual);
}

// ---------- Rutas web ----------
void rutaDatos() {
  if (ultimaLinea.length() == 0) {
    server.send(503, "text/plain", "Sin lecturas todavia");
    return;
  }
  server.send(200, "text/plain", ultimaLinea);
}

void rutaInicio() {
  String html = "<h1>Ventilacion del aula</h1><p>Ultima lectura: ";
  html += (ultimaLinea.length() ? ultimaLinea : "sin datos");
  html += "</p><p><a href='/datos'>/datos</a></p>";
  server.send(200, "text/html", html);
}

void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Conectando a WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("IP del ESP32: ");
  Serial.println(WiFi.localIP());
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_VERDE, OUTPUT);
  pinMode(PIN_AMARILLO, OUTPUT);
  pinMode(PIN_ROJO, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  ventana.setPeriodHertz(50);
  ventana.attach(PIN_SERVO, 500, 2400);

  dht.begin();
  aplicarEstado(NORMAL);

  conectarWiFi();

  server.on("/", rutaInicio);
  server.on("/datos", rutaDatos);
  server.begin();
}

void loop() {
  // Si se cae el WiFi, intenta reconectar sin bloquear el control local
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
  }

  server.handleClient();

  if (millis() - ultimaLectura < INTERVALO_MS) return;
  ultimaLectura = millis();

  float h = dht.readHumidity();
  float t = dht.readTemperature();

  if (isnan(h) || isnan(t)) {
    Serial.println("ERROR:DHT");
    return;
  }

  Estado nuevo = clasificar(t, estadoActual);
  if (nuevo != estadoActual) {
    estadoActual = nuevo;
    aplicarEstado(estadoActual);
  }

  ultimaLinea = "T:" + String(t, 1) + ",H:" + String(h, 1) +
                ",E:" + String(NOMBRES[estadoActual]) +
                ",S:" + String(anguloActual);
  Serial.println(ultimaLinea);   // útil para depurar en el Monitor Serial
}
