"""
Laboratorio IoT #5 - Bot de Telegram para el nodo de la huerta (ESP32 por WiFi)
Integrantes: Karen - Camila

Instalación:
    pip install "python-telegram-bot[job-queue]"

Uso:
    1. Cargue el .ino con el SSID y la clave de su red WiFi (2.4 GHz).
    2. Abra el Monitor Serie del ESP32 una vez para ver su IP
       ("[wifi] IP: 192.168.x.x"). Ya no necesita el cable USB para el bot.
    3. Ponga el token en la variable de entorno TELEGRAM_TOKEN y, si quiere,
       la IP en ESP32_HOST (por defecto se usa el nombre mDNS huerta.local).
         Windows (cmd):        set TELEGRAM_TOKEN=123456:ABC...
         Windows (PowerShell): $env:TELEGRAM_TOKEN="123456:ABC..."
         Mac/Linux:            export TELEGRAM_TOKEN=123456:ABC...
    4. El computador debe estar en la misma red que el ESP32.
    5. python bot_huerta.py
    6. En Telegram envíe /start desde el chat que recibirá las alertas.

Cómo se evita confundir alertas con respuestas:
    Un hilo lector lee TODAS las líneas del socket TCP y las separa por prefijo:
      OK / ERR      -> cola de respuestas (las recibe el comando que espera)
      ALERTA / AVISO -> cola de alertas (las reenvía la tarea periódica)
    Si se pierde la conexión, el hilo reintenta solo cada pocos segundos.
"""

import asyncio
import os
import queue
import socket
import threading
import time

from telegram import Update
from telegram.ext import (Application, CommandHandler, ContextTypes,
                          MessageHandler, filters)

# --------------------------------------------------------------------------
# Configuración
# --------------------------------------------------------------------------
TOKEN = os.environ.get("TELEGRAM_TOKEN", "8229122473:AAGEJWwvqA2X5B22k5IfKCVIYsitHOgiO2A")
ESP32_HOST = os.environ.get("ESP32_HOST", "10.233.52.82")  # o la IP: 192.168.x.x
ESP32_PORT = 8888          # debe coincidir con PUERTO_TCP del .ino
TIEMPO_RESPUESTA = 2.0     # segundos que se espera la respuesta del ESP32
REINTENTO_S = 3            # segundos entre intentos de reconexión

# Seguridad: solo este chat puede usar el bot.
# Si se deja en None, el primer chat que envíe /start queda como dueño.
CHAT_PERMITIDO = 7072338163

# --------------------------------------------------------------------------
# Conexión WiFi (TCP)
# --------------------------------------------------------------------------
sock = None                    # socket actual (None si no hay conexión)
respuestas = queue.Queue()     # líneas OK / ERR
alertas = queue.Queue()        # líneas ALERTA / AVISO
candado = threading.Lock()     # una sola orden a la vez
chat_alertas = CHAT_PERMITIDO


def lector_red():
    """Hilo: mantiene la conexión, lee todas las líneas y las clasifica."""
    global sock
    while True:
        try:
            s = socket.create_connection((ESP32_HOST, ESP32_PORT), timeout=5)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_KEEPALIVE, 1)
            s.settimeout(None)
            print(f"[red] conectado a {ESP32_HOST}:{ESP32_PORT}")
        except OSError as error:
            print(f"[red] sin conexión ({error}); reintento en {REINTENTO_S} s")
            time.sleep(REINTENTO_S)
            continue

        sock = s
        try:
            with s.makefile("r", encoding="utf-8", errors="ignore", newline="\n") as f:
                for linea in f:
                    linea = linea.strip()
                    if not linea:
                        continue
                    print("[ESP32]", linea)
                    if linea.startswith(("OK", "ERR")):
                        respuestas.put(linea)
                    elif linea.startswith(("ALERTA", "AVISO")):
                        alertas.put(linea)
                    # cualquier otra línea se ignora
        except OSError as error:
            print("[red] error de lectura:", error)
        finally:
            sock = None
            s.close()
            print("[red] conexión cerrada")
        time.sleep(REINTENTO_S)


threading.Thread(target=lector_red, daemon=True).start()


def enviar_orden(orden):
    """Envía un carácter y espera la respuesta OK/ERR del ESP32."""
    with candado:
        while not respuestas.empty():       # descarta respuestas atrasadas
            respuestas.get_nowait()
        s = sock
        if s is None:
            return None
        try:
            s.sendall(orden.encode())
        except OSError:
            return None
        try:
            return respuestas.get(timeout=TIEMPO_RESPUESTA)
        except queue.Empty:
            return None


# --------------------------------------------------------------------------
# Formato de mensajes
# --------------------------------------------------------------------------
def a_diccionario(linea):
    datos = {}
    for parte in linea.split(";")[1:]:
        if "=" in parte:
            clave, valor = parte.split("=", 1)
            datos[clave] = valor
    return datos


def texto_estado(linea):
    d = a_diccionario(linea)
    nivel = d.get("NIVEL", "?")
    nivel = "sin lectura" if nivel == "NA" else f"{nivel} %"
    temp = d.get("TEMP", "?")
    temp = "sin lectura" if temp == "NA" else f"{temp} °C"
    return (
        "🌱 Estado de la huerta\n"
        f"• Temperatura: {temp}\n"
        f"• Nivel del tanque: {nivel} ({d.get('TANQUE', '?')})\n"
        f"• Evento tanque bajo: {'ACTIVO' if d.get('EVENTO') == '1' else 'no'}\n"
        f"• Salida 1 (válvula): {'ENCENDIDA' if d.get('S1') == 'ON' else 'APAGADA'}\n"
        f"• Salida 2 (alarma): {'ACTIVA' if d.get('S2') == 'ON' else 'apagada'}"
        f"{' (silenciada)' if d.get('SIL') == 'SI' else ''}\n"
        f"\nESP32: {linea}"
    )


def texto_respuesta(linea):
    if linea is None:
        if sock is None:
            return (f"📡 Sin conexión WiFi con el ESP32 ({ESP32_HOST}:{ESP32_PORT}). "
                    "Reintentando automáticamente.")
        return "⏱️ El ESP32 no respondió a tiempo."
    if linea.startswith("ERR;TANQUE_NO_BAJO"):
        return f"⛔ No se puede abrir la válvula: el tanque no está bajo.\nESP32: {linea}"
    if linea.startswith("ERR;TANQUE_NO_LLENO"):
        return f"ℹ️ El tanque no está marcado como lleno; no hay nada que vaciar.\nESP32: {linea}"
    if linea.startswith("ERR"):
        return f"❌ Orden no válida. No se cambió ninguna salida.\nESP32: {linea}"
    if linea.startswith("OK;RESUMEN"):
        d = a_diccionario(linea)
        return (
            "📊 Resumen desde el arranque\n"
            f"• Temperatura: mín. {d.get('TEMP_MIN')} °C / máx. {d.get('TEMP_MAX')} °C\n"
            f"• Nivel del tanque: mín. {d.get('NIVEL_MIN')} % / máx. {d.get('NIVEL_MAX')} %\n"
            f"\nESP32: {linea}"
        )
    return texto_estado(linea)


def texto_alerta(linea):
    d = a_diccionario(linea)
    if linea.startswith("ALERTA;UMBRAL"):
        texto = f"⚠️ Temperatura alta en la huerta: {d.get('TEMP')} °C"
    elif linea.startswith("ALERTA;TANQUE_BAJO"):
        texto = f"⚠️ Tanque bajo: nivel {d.get('NIVEL')}. Válvula bloqueada."
    elif linea.startswith("ALERTA;TARJETA_NO_AUTORIZADA"):
        texto = f"🚫 Tarjeta no autorizada en el lector: UID {d.get('UID')}"
    elif linea.startswith("AVISO;MANDO_LOCAL_RECHAZADO"):
        texto = "ℹ️ Se intentó abrir la válvula con tarjeta, pero el tanque no está bajo."
    elif linea.startswith("AVISO;MANDO_LOCAL"):
        estado = "ENCENDIDA" if d.get("S1") == "ON" else "APAGADA"
        texto = f"ℹ️ Válvula {estado} con tarjeta en el sitio"
    elif linea.startswith("AVISO;S1_APAGADA_TANQUE_NO_BAJO"):
        texto = "ℹ️ La válvula se cerró sola porque el tanque ya no está bajo."
    elif linea.startswith("AVISO;TANQUE_LLENO"):
        texto = (f"✅ Tanque lleno: nivel {d.get('NIVEL')} %. Estado guardado: no habrá "
                 "alertas de nivel bajo hasta que use /vaciar.")
    elif linea.startswith("AVISO;INICIO"):
        texto = "🔄 El nodo se reinició."
    else:
        texto = "Mensaje del nodo"
    return f"{texto}\nESP32: {linea}"


# --------------------------------------------------------------------------
# Comandos
# --------------------------------------------------------------------------
def autorizado(update: Update):
    return CHAT_PERMITIDO is None or update.effective_chat.id == CHAT_PERMITIDO


async def start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    global chat_alertas, CHAT_PERMITIDO
    chat_id = update.effective_chat.id
    if CHAT_PERMITIDO is None:
        CHAT_PERMITIDO = chat_id
        print(f"[bot] chat autorizado: {chat_id} (puede fijarlo en CHAT_PERMITIDO)")
    if chat_id != CHAT_PERMITIDO:
        await update.message.reply_text("⛔ Este bot ya tiene un dueño.")
        return
    chat_alertas = chat_id
    await update.message.reply_text(
        "✅ Alertas activadas en este chat.\n\n"
        "/estado – estado del nodo\n"
        "/encender – abrir la válvula (salida 1)\n"
        "/apagar – cerrar la válvula (salida 1)\n"
        "/silenciar – apagar la alarma local (salida 2)\n"
        "/resumen – temperatura y nivel mínimos y máximos\n"
        "/vaciar – borrar el estado «tanque lleno» y reactivar alertas de nivel bajo\n"
        "/orden X – enviar un carácter de prueba (ej. /orden Z)"
    )


async def ejecutar(update: Update, orden: str):
    if not autorizado(update):
        await update.message.reply_text("⛔ No autorizado.")
        return
    linea = await asyncio.to_thread(enviar_orden, orden)
    await update.message.reply_text(texto_respuesta(linea))


async def estado(update: Update, context: ContextTypes.DEFAULT_TYPE):
    await ejecutar(update, "E")


async def encender(update: Update, context: ContextTypes.DEFAULT_TYPE):
    await ejecutar(update, "1")


async def apagar(update: Update, context: ContextTypes.DEFAULT_TYPE):
    await ejecutar(update, "0")


async def silenciar(update: Update, context: ContextTypes.DEFAULT_TYPE):
    await ejecutar(update, "S")


async def resumen(update: Update, context: ContextTypes.DEFAULT_TYPE):
    await ejecutar(update, "R")


async def vaciar(update: Update, context: ContextTypes.DEFAULT_TYPE):
    await ejecutar(update, "V")


async def orden_libre(update: Update, context: ContextTypes.DEFAULT_TYPE):
    """Permite probar una orden inválida: /orden Z"""
    if not context.args:
        await update.message.reply_text("Uso: /orden X  (un carácter)")
        return
    await ejecutar(update, context.args[0][0])


async def desconocido(update: Update, context: ContextTypes.DEFAULT_TYPE):
    await update.message.reply_text("Comando no reconocido. Use /start para ver la lista.")


# --------------------------------------------------------------------------
# Tarea periódica: reenviar alertas
# --------------------------------------------------------------------------
async def revisar_alertas(context: ContextTypes.DEFAULT_TYPE):
    while not alertas.empty():
        linea = alertas.get_nowait()
        if chat_alertas is None:
            print("[bot] alerta sin chat registrado (envíe /start):", linea)
            continue
        await context.bot.send_message(chat_id=chat_alertas, text=texto_alerta(linea))


# --------------------------------------------------------------------------
# Programa principal
# --------------------------------------------------------------------------
def main():
    app = Application.builder().token(TOKEN).build()
    app.add_handler(CommandHandler("start", start))
    app.add_handler(CommandHandler("estado", estado))
    app.add_handler(CommandHandler("encender", encender))
    app.add_handler(CommandHandler("apagar", apagar))
    app.add_handler(CommandHandler("silenciar", silenciar))
    app.add_handler(CommandHandler("resumen", resumen))
    app.add_handler(CommandHandler("vaciar", vaciar))
    app.add_handler(CommandHandler("orden", orden_libre))
    app.add_handler(MessageHandler(filters.COMMAND, desconocido))
    app.job_queue.run_repeating(revisar_alertas, interval=1)
    print("[bot] en marcha. Envíe /start en Telegram. Ctrl+C para salir.")
    app.run_polling()


if __name__ == "__main__":
    main()
