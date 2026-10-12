"""
Laboratorio IoT #2 - Versión WiFi: ESP32 -> (red) -> Python -> Telegram

- Consulta cada pocos segundos http://IP_DEL_ESP32/datos, que devuelve una
  línea como "T:25.3,H:60.0,E:NORMAL,S:0".
- Responde a /start, /temperatura, /humedad y /estado.
- Envía una alerta automática cuando el estado pasa a CRITICA
  (y un aviso cuando vuelve a la normalidad).

Instalación:
    py -m pip install python-telegram-bot requests python-dotenv

Archivo .env (misma carpeta que este script, sin comillas ni espacios):
    TELEGRAM_TOKEN=tu_token
    TELEGRAM_CHAT_ID=tu_chat_id
    ESP32_URL=http://192.168.1.19
    INTERVALO_SEG=3

Ya no se usa el puerto serial: el PC y el ESP32 deben estar en la misma red.
"""

import asyncio
import logging
import os
import threading
import time

import requests
from dotenv import load_dotenv
from telegram import Update
from telegram.ext import Application, CommandHandler, ContextTypes

load_dotenv()

TOKEN = os.environ["TELEGRAM_TOKEN"]
CHAT_ID = int(os.environ.get("TELEGRAM_CHAT_ID", "0"))
ESP32_URL = os.environ["ESP32_URL"].rstrip("/")
INTERVALO_SEG = float(os.environ.get("INTERVALO_SEG", "3"))
SEGUNDOS_SIN_DATOS = 15  # si pasa este tiempo sin lecturas, se avisa

logging.basicConfig(
    format="%(asctime)s - %(levelname)s - %(message)s", level=logging.INFO
)
logging.getLogger("httpx").setLevel(logging.WARNING)  # menos ruido en la consola
log = logging.getLogger("ventilacion")

# Última lectura recibida (la escribe el hilo de consulta, la leen los comandos)
datos = {"temp": None, "hum": None, "estado": None, "servo": None, "hora": 0.0}
candado = threading.Lock()

ICONOS = {"NORMAL": "🟢", "ELEVADA": "🟡", "CRITICA": "🔴"}
VENTANA = {0: "cerrada", 90: "medio abierta", 180: "abierta"}


# ------------------------------------------------------- Consulta al ESP32
def procesar_linea(linea: str) -> bool:
    """Convierte 'T:25.3,H:60.0,E:NORMAL,S:0' en datos. Devuelve True si fue válida."""
    try:
        campos = dict(par.split(":", 1) for par in linea.split(","))
        nuevo = {
            "temp": float(campos["T"]),
            "hum": float(campos["H"]),
            "estado": campos["E"],
            "servo": int(campos["S"]),
            "hora": time.time(),
        }
    except (ValueError, KeyError):
        log.warning("Respuesta con formato inesperado: %r", linea)
        return False
    with candado:
        datos.update(nuevo)
    log.info("Lectura: %s", linea)
    return True


def consultar_esp32() -> None:
    """Hilo que pide /datos al ESP32 de forma periódica."""
    hubo_error = False
    while True:
        try:
            r = requests.get(f"{ESP32_URL}/datos", timeout=4)
            if r.status_code == 200:
                procesar_linea(r.text.strip())
                if hubo_error:
                    log.info("Conexión con el ESP32 recuperada")
                hubo_error = False
            else:
                if not hubo_error:
                    log.warning("El ESP32 respondió con código %s", r.status_code)
                hubo_error = True
        except requests.RequestException as e:
            if not hubo_error:  # solo se avisa la primera vez, para no llenar la consola
                log.error("No pude conectar con %s: %s", ESP32_URL, e)
            hubo_error = True
        time.sleep(INTERVALO_SEG)


def lectura_actual() -> dict:
    with candado:
        d = dict(datos)
    d["vigente"] = d["hora"] > 0 and (time.time() - d["hora"]) <= SEGUNDOS_SIN_DATOS
    return d


# --------------------------------------------------------------- Telegram
def autorizada(update: Update) -> bool:
    return CHAT_ID != 0 and update.effective_chat.id == CHAT_ID


async def start(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    # Se deja abierto a propósito: así puedes conocer tu chat_id la primera vez.
    texto = f"Tu chat_id es: {update.effective_chat.id}\n"
    if autorizada(update):
        texto += (
            "\nSistema de ventilación del aula.\n"
            "Comandos:\n"
            "/temperatura - temperatura actual\n"
            "/humedad - humedad actual\n"
            "/estado - estado del sistema"
        )
    await update.message.reply_text(texto)


async def _responder_si_hay_datos(update: Update, formato) -> None:
    if not autorizada(update):
        return
    d = lectura_actual()
    if not d["vigente"]:
        await update.message.reply_text(
            "No estoy recibiendo datos del ESP32. Revisa que esté encendido, "
            "conectado al WiFi y que la IP del .env sea la correcta."
        )
        return
    await update.message.reply_text(formato(d))


async def temperatura(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    await _responder_si_hay_datos(update, lambda d: f"🌡️ Temperatura: {d['temp']:.1f} °C")


async def humedad(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    await _responder_si_hay_datos(update, lambda d: f"💧 Humedad: {d['hum']:.1f} %")


async def estado(update: Update, ctx: ContextTypes.DEFAULT_TYPE):
    await _responder_si_hay_datos(
        update,
        lambda d: (
            f"{ICONOS.get(d['estado'], '')} Estado: {d['estado']}\n"
            f"🌡️ Temperatura: {d['temp']:.1f} °C\n"
            f"💧 Humedad: {d['hum']:.1f} %\n"
            f"🪟 Ventana: {VENTANA.get(d['servo'], str(d['servo']) + '°')}"
        ),
    )


async def vigilar(app: Application) -> None:
    """Revisa cada segundo si el estado cambió y envía alertas automáticas."""
    previo = None
    while True:
        await asyncio.sleep(1)
        d = lectura_actual()
        if CHAT_ID == 0 or not d["vigente"]:
            continue

        actual = d["estado"]
        mensaje = None
        if actual != previo:
            if actual == "CRITICA":
                mensaje = (
                    "🚨 ALERTA: temperatura CRÍTICA\n"
                    f"🌡️ {d['temp']:.1f} °C | 💧 {d['hum']:.1f} %\n"
                    "🪟 Ventana completamente abierta."
                )
            elif previo == "CRITICA":
                mensaje = (
                    f"✅ La temperatura bajó del nivel crítico ({d['temp']:.1f} °C). "
                    f"Estado actual: {actual}."
                )
        previo = actual

        if mensaje:
            try:
                await app.bot.send_message(chat_id=CHAT_ID, text=mensaje)
            except Exception as e:  # no tumbar el bot si Telegram falla un momento
                log.error("No pude enviar la alerta: %s", e)


async def iniciar(app: Application) -> None:
    app.bot_data["vigilante"] = asyncio.create_task(vigilar(app))


def main() -> None:
    threading.Thread(target=consultar_esp32, daemon=True).start()

    app = Application.builder().token(TOKEN).post_init(iniciar).build()
    app.add_handler(CommandHandler("start", start))
    app.add_handler(CommandHandler("temperatura", temperatura))
    app.add_handler(CommandHandler("humedad", humedad))
    app.add_handler(CommandHandler("estado", estado))
    app.run_polling()


if __name__ == "__main__":
    main()
