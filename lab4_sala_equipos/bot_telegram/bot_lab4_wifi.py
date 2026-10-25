"""Laboratorio IoT #4 - Bot de Telegram con el ESP32 por WiFi (TCP).

Instalacion:  pip install "python-telegram-bot[job-queue]" python-dotenv
Archivo .env:
    TOKEN=123456:ABC...
    ESP32_IP=192.168.1.50          (la IP que imprime el ESP32 en el Monitor Serie)
    ESP32_PUERTO=5000
    CHATS_AUTORIZADOS=             (opcional)

El computador y el ESP32 deben estar en la misma red WiFi (2.4 GHz).
"""
import os
import queue
import socket
import threading
import time

from dotenv import load_dotenv
from telegram import Update
from telegram.ext import Application, CommandHandler, ContextTypes

load_dotenv()
TOKEN = os.getenv("TOKEN")
ESP32_IP = os.getenv("ESP32_IP")
ESP32_PUERTO = int(os.getenv("ESP32_PUERTO", "5000"))
AUTORIZADOS = {int(x) for x in os.getenv("CHATS_AUTORIZADOS", "").split(",") if x.strip()}

respuestas = queue.Queue()   # lineas OK / ERR / READY
alertas = queue.Queue()      # lineas ALERTA y avisos
candado_envio = threading.Lock()
sock = None
chat_alertas = None


def conectar():
    """Intenta conectarse al ESP32 hasta lograrlo."""
    global sock
    while True:
        try:
            s = socket.create_connection((ESP32_IP, ESP32_PUERTO), timeout=5)
            s.settimeout(1)
            sock = s
            print(f"Conectado al ESP32 en {ESP32_IP}:{ESP32_PUERTO}")
            return
        except OSError:
            print("No se pudo conectar al ESP32, reintentando en 3 s...")
            time.sleep(3)


def lector_tcp():
    """Unico hilo que lee del ESP32: separa respuestas y alertas por prefijo; reconecta si se cae."""
    global sock
    conectar()
    buffer = b""
    while True:
        try:
            datos = sock.recv(1024)
            if not datos:
                raise ConnectionError("conexion cerrada")
        except socket.timeout:
            continue
        except OSError:
            alertas.put("Aviso: se perdió la conexión con el ESP32, reconectando...")
            sock = None
            buffer = b""
            conectar()
            continue
        buffer += datos
        while b"\n" in buffer:
            linea, buffer = buffer.split(b"\n", 1)
            linea = linea.decode(errors="ignore").strip()
            if not linea:
                continue
            if linea.startswith(("ALERTA", "AUTO;")):
                alertas.put(linea)
            else:
                if linea.startswith("READY"):
                    alertas.put("Aviso: conexión con el ESP32 establecida (salidas en estado actual)")
                respuestas.put(linea)


threading.Thread(target=lector_tcp, daemon=True).start()


def enviar_orden(orden: str) -> str:
    with candado_envio:
        while not respuestas.empty():
            respuestas.get_nowait()
        try:
            sock.sendall(orden.encode())
        except (OSError, AttributeError):
            return "Sin conexión con el ESP32"
        try:
            while True:
                linea = respuestas.get(timeout=3)
                if linea.startswith(("OK", "ERR")):
                    return linea
        except queue.Empty:
            return "Sin respuesta del ESP32"


def formatear(linea: str) -> str:
    if not linea.startswith("OK"):
        return linea
    d = dict(p.split("=", 1) for p in linea.split(";")[1:] if "=" in p)
    dist = "sin eco" if d.get("DIST") == "-1" else f"{d.get('DIST')} cm"
    return (
        f"Luz: {'ENCENDIDA' if d.get('LUZ') == '1' else 'apagada'}\n"
        f"Ventilación: {'ENCENDIDA' if d.get('VENT') == '1' else 'apagada'}\n"
        f"Rejilla: {d.get('REJ')}° ({'abierta' if d.get('REJ') == '90' else 'cerrada' if d.get('REJ') == '0' else 'intermedia'})\n"
        f"Distancia: {dist}\n"
        f"Humedad: {d.get('HUM')} → {d.get('NIVEL')}\n"
        f"Modo automático: {'activado' if d.get('AUTO') == '1' else 'desactivado'}"
    )


def autorizado(update: Update) -> bool:
    return not AUTORIZADOS or update.effective_chat.id in AUTORIZADOS


def comando(orden: str):
    async def manejador(update: Update, context: ContextTypes.DEFAULT_TYPE):
        if not autorizado(update):
            await update.message.reply_text("No autorizado")
            return
        # enviar_orden bloquea hasta 3 s: se ejecuta en un hilo para no frenar al bot
        import asyncio
        respuesta = await asyncio.to_thread(enviar_orden, orden)
        await update.message.reply_text(formatear(respuesta))
    return manejador


async def start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    global chat_alertas
    if not autorizado(update):
        await update.message.reply_text("No autorizado")
        return
    chat_alertas = update.effective_chat.id
    await update.message.reply_text("Alertas activadas en este chat")


async def revisar_alertas(context: ContextTypes.DEFAULT_TYPE):
    while not alertas.empty():
        linea = alertas.get_nowait()
        if chat_alertas is None:
            continue
        if linea.startswith("ALERTA;PROXIMIDAD"):
            texto = f"⚠️ Alerta de proximidad: {linea.split('DIST=')[1]} cm"
        elif linea.startswith("ALERTA;HUMEDAD"):
            texto = "💧 Alerta de humedad: agua detectada (posible filtración)"
        elif linea.startswith("AUTO;LUZ_ON"):
            texto = "🤖 Automático: se encendió la iluminación porque se detectó presencia"
        elif linea.startswith("AUTO;LUZ_OFF"):
            texto = "🤖 Automático: se apagó la iluminación (sin presencia)"
        elif linea.startswith("AUTO;REJILLA_CERRADA"):
            texto = "🤖 Automático: se cerró la rejilla por la filtración de agua"
        else:
            texto = linea
        await context.bot.send_message(chat_id=chat_alertas, text=texto)


app = Application.builder().token(TOKEN).build()
app.add_handler(CommandHandler("start", start))
for nombre, orden in [
    ("luz_on", "L"), ("luz_off", "l"),
    ("vent_on", "V"), ("vent_off", "v"),
    ("rejilla_abrir", "A"), ("rejilla_cerrar", "a"),
    ("estado", "E"),
    ("auto_on", "M"), ("auto_off", "m"),
]:
    app.add_handler(CommandHandler(nombre, comando(orden)))
app.job_queue.run_repeating(revisar_alertas, interval=1)
app.run_polling()
