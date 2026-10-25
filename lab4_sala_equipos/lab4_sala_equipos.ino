// Laboratorio IoT #4 - Version WiFi con MODO AUTOMATICO
// El ESP32 decide por si solo (aunque no haya WiFi, Python ni Telegram):
//   - Presencia (HC-SR04 < DIST_ALERTA): enciende la iluminacion; la apaga tras LUZ_AUTO_MS sin presencia.
//   - Agua (sensor de humedad MOJADO): cierra la rejilla.
// Telegram sigue sirviendo para control manual, consulta de estado y recibir avisos.
// Las ordenes tambien se pueden escribir en el Monitor Serie (115200) para probar sin Telegram.
//
// Placa: "DOIT ESP32 DEVKIT V1". Libreria necesaria: ESP32Servo.
// Conexiones: LED 25/26, servo 18, TRIG 27, ECHO 14 (con divisor), humedad 34.
//
// PROTOCOLO (una linea por mensaje terminada en \n)
//   Ordenes (1 caracter): L l V v A a E  y  M (modo automatico ON) / m (OFF)
//   OK;LUZ=1;VENT=0;REJ=90;DIST=45;HUM=2850;NIVEL=SECO;AUTO=1   respuesta a una orden valida
//   ERR;ORDEN_DESCONOCIDA                                       orden invalida
//   ALERTA;PROXIMIDAD;DIST=18 | ALERTA;HUMEDAD;HUM=1000;NIVEL=MOJADO   (una vez por evento)
//   AUTO;LUZ_ON;MOTIVO=PRESENCIA | AUTO;LUZ_OFF;MOTIVO=SIN_PRESENCIA
//   AUTO;REJILLA_CERRADA;MOTIVO=FILTRACION                      decisiones tomadas por el ESP32
//   READY                                                       al conectarse un cliente

#include <WiFi.h>
#include <ESP32Servo.h>

// ---------- Red (CAMBIAR) ----------
const char* WIFI_SSID = "Redmi Note 14 Pro";
const char* WIFI_PASS = "Karen123";
const uint16_t PUERTO_TCP = 5000;

// ---------- Pines ----------
const int PIN_LUZ   = 25;
const int PIN_VENT  = 26;
const int PIN_SERVO = 18;
const int TRIG = 27;
const int ECHO = 14;
const int PIN_HUM = 34;

const int REJ_ABIERTA = 90;
const int REJ_CERRADA = 0;

// ---------- Umbrales (calibrar con el sensor fisico) ----------
const int DIST_ALERTA = 30;
const int DIST_REARME = 35;
const int HUM_SECO_MIN   = 2800;
const int HUM_MOJADO_MAX = 1600;
const int HUM_REARME     = 1800;

const unsigned long PERIODO_LECTURA = 500;
const unsigned long LUZ_AUTO_MS = 10000;   // segundos sin presencia antes de apagar la luz (10 s)

WiFiServer servidor(PUERTO_TCP);
WiFiClient cliente;
Servo rejilla;

bool luzOn = false, ventOn = false;
bool modoAuto = true;            // modo automatico activo por defecto
bool luzPorAuto = false;         // la luz fue encendida por el modo automatico
bool presencia = false, mojado = false;       // estado de los sensores (con histeresis)
bool alertaProx = false, alertaHum = false;   // alerta ya enviada a Telegram
unsigned long ultimaPresencia = 0, ultimaLectura = 0, ultimoIntentoWiFi = 0;
bool servidorIniciado = false;

bool hayCliente() { return cliente && cliente.connected(); }

// Envia una linea al cliente TCP (si lo hay) y al Monitor Serie
void enviar(const String& linea) {
  Serial.println(linea);
  if (hayCliente()) cliente.println(linea);
}

long medirDistancia() {
  digitalWrite(TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  long duracion = pulseIn(ECHO, HIGH, 30000);
  if (duracion == 0) return -1;
  return duracion / 58;
}

const char* clasificarHumedad(int h) {
  Serial.println("Humedad:");
  Serial.println(h);
  if (h >= HUM_SECO_MIN)   return "SECO";
  if (h >= HUM_MOJADO_MAX) return "HUMEDO";
  return "MOJADO";
}

void ponerLuz(bool on)  { luzOn = on;  digitalWrite(PIN_LUZ, on ? HIGH : LOW); }
void ponerVent(bool on) { ventOn = on; digitalWrite(PIN_VENT, on ? HIGH : LOW); }

void enviarEstado(const char* prefijo) {
  long d = medirDistancia();
  int h = analogRead(PIN_HUM);
  String s = String(prefijo);
  s += ";LUZ=" + String(luzOn ? 1 : 0);
  s += ";VENT=" + String(ventOn ? 1 : 0);
  s += ";REJ=" + String(rejilla.read());
  s += ";DIST=" + String(d);
  s += ";HUM=" + String(h);
  s += ";NIVEL=" + String(clasificarHumedad(h));
  s += ";AUTO=" + String(modoAuto ? 1 : 0);
  enviar(s);
}

void atenderOrden(char orden) {
  switch (orden) {
    case 'L': ponerLuz(true);  luzPorAuto = false; break;   // una orden manual toma el control de la luz
    case 'l': ponerLuz(false); luzPorAuto = false; break;
    case 'V': ponerVent(true);  break;
    case 'v': ponerVent(false); break;
    case 'A': rejilla.write(REJ_ABIERTA); break;
    case 'a': rejilla.write(REJ_CERRADA); break;
    case 'M': modoAuto = true;  break;
    case 'm': modoAuto = false; luzPorAuto = false; break;
    case 'E': break;
    case '\n': case '\r': case ' ': return;
    default:
      enviar("ERR;ORDEN_DESCONOCIDA");
      return;
  }
  enviarEstado("OK");
}

void revisarSensores() {
  long d = medirDistancia();
  int h = analogRead(PIN_HUM);
  bool conectado = hayCliente();

  // ----- Proximidad (histeresis) -----
  bool presenciaAntes = presencia;
  if (!presencia && d > 0 && d < DIST_ALERTA) presencia = true;
  else if (presencia && (d < 0 || d > DIST_REARME)) presencia = false;
  if (presencia) ultimaPresencia = millis();

  // Alerta a Telegram: una vez por evento; si no hay cliente se envia al conectarse si persiste
  if (presencia && !alertaProx && conectado) {
    alertaProx = true;
    enviar("ALERTA;PROXIMIDAD;DIST=" + String(d));
  }
  if (!presencia) alertaProx = false;

  // Decision automatica de la luz (solo al LLEGAR alguien; asi una orden manual no se pelea con el sensor)
  if (modoAuto) {
    if (presencia && !presenciaAntes && !luzOn) {
      ponerLuz(true);
      luzPorAuto = true;
      enviar("AUTO;LUZ_ON;MOTIVO=PRESENCIA");
    }
    if (luzPorAuto && !presencia && millis() - ultimaPresencia >= LUZ_AUTO_MS) {
      ponerLuz(false);
      luzPorAuto = false;
      enviar("AUTO;LUZ_OFF;MOTIVO=SIN_PRESENCIA");
    }
  }

  // ----- Humedad (histeresis) -----
  bool mojadoAntes = mojado;
  if (!mojado && h < HUM_MOJADO_MAX) mojado = true;
  else if (mojado && h > HUM_REARME) mojado = false;

  if (mojado && !alertaHum && conectado) {
    alertaHum = true;
    enviar("ALERTA;HUMEDAD;HUM=" + String(h) + ";NIVEL=MOJADO");
  }
  if (!mojado) alertaHum = false;

  // Decision automatica ante filtracion: cerrar la rejilla (solo al detectar el agua)
  if (modoAuto && mojado && !mojadoAntes) {
    if (rejilla.read() != REJ_CERRADA) rejilla.write(REJ_CERRADA);
    enviar("AUTO;REJILLA_CERRADA;MOTIVO=FILTRACION");
  }
}

// El modo automatico NO depende del WiFi: si la red falla, el ESP32 sigue decidiendo solo
void gestionarRed() {
  if (WiFi.status() == WL_CONNECTED) {
    if (!servidorIniciado) {
      servidor.begin();
      servidorIniciado = true;
      Serial.print("IP del ESP32: ");
      Serial.println(WiFi.localIP());   // esta IP va en el .env de Python
    }
  } else if (millis() - ultimoIntentoWiFi > 15000) {
    ultimoIntentoWiFi = millis();
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

void setup() {
  pinMode(PIN_LUZ, OUTPUT);
  pinMode(PIN_VENT, OUTPUT);
  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);
  digitalWrite(PIN_LUZ, LOW);
  digitalWrite(PIN_VENT, LOW);
  analogSetPinAttenuation(PIN_HUM, ADC_11db);
  rejilla.setPeriodHertz(50);
  rejilla.attach(PIN_SERVO, 500, 2400);
  rejilla.write(REJ_CERRADA);
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  ultimoIntentoWiFi = millis();
  Serial.print("Conectando a WiFi");
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 10000) {  // maximo 10 s, luego sigue sin red
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) Serial.println("Sin WiFi: modo automatico activo, se seguira intentando conectar");
}

void loop() {
  gestionarRed();

  if (servidorIniciado) {
    WiFiClient nuevo = servidor.available();   // un cliente nuevo reemplaza al anterior
    if (nuevo) {
      if (cliente) cliente.stop();
      cliente = nuevo;
      enviar("READY");
    }
  }

  if (hayCliente() && cliente.available() > 0) atenderOrden(cliente.read());  // ordenes desde Telegram/Python
  if (Serial.available() > 0) atenderOrden(Serial.read());                    // ordenes desde el Monitor Serie

  if (millis() - ultimaLectura >= PERIODO_LECTURA) {
    ultimaLectura = millis();
    revisarSensores();
  }
}
