import serial
from datetime import datetime
from telegram import Update
from telegram.ext import Application, CommandHandler, ContextTypes

#  CONFIGURACIÓN 
TOKEN = ""

# Puerto serial 
PUERTO_SERIAL = "/dev/cu.usbserial-0001"

# Velocidad serial 
BAUDRATE = 115200

#  ESTADO GLOBAL 
CHAT_ALERTAS = None            # chat_id del usuario que inició el bot
estado_puerta = "cerrada"       # "cerrada", "abierta" o "alarma"
ultimo_acceso_fecha = None      # datetime del último acceso
ultimo_acceso_distancia = 0.0   # distancia del último acceso (cm)
total_accesos = 0               # contador de accesos
distancia_actual = 0.0          # última distancia leída

#  CONEXIÓN SERIAL 
try:
    arduino = serial.Serial(PUERTO_SERIAL, BAUDRATE, timeout=1)
    print(f"Conectado a {PUERTO_SERIAL} a {BAUDRATE} baudios")
except serial.SerialException as e:
    print(f"Error al abrir {PUERTO_SERIAL}: {e}")
    print("Verifique que el Monitor Serie de Arduino IDE esté cerrado.")
    exit(1)

#  COMANDOS DEL BOT 

async def start(update: Update, context: ContextTypes.DEFAULT_TYPE):
    """Comando /start — registra el chat para alertas."""
    global CHAT_ALERTAS
    CHAT_ALERTAS = update.effective_chat.id
    await update.message.reply_text(
        "Bot de Control de Acceso activo.\n\n"
        "Comandos disponibles:\n"
        "/estado — Estado actual de la puerta\n"
        "/acceso — Último acceso registrado\n\n"
        "Recibirás notificaciones automáticas cuando se detecte una persona."
    )

async def estado(update: Update, context: ContextTypes.DEFAULT_TYPE):
    """Comando /estado — muestra el estado actual."""
    iconos = {
        "cerrada": "Puerta CERRADA",
        "abierta": "Puerta ABIERTA — acceso en curso",
        "alarma":  "ALARMA — puerta abierta demasiado tiempo"
    }
    texto = iconos.get(estado_puerta, "❓ Estado desconocido")
    texto += f"\nDistancia actual: {distancia_actual:.1f} cm"
    texto += f"\nTotal de accesos: {total_accesos}"
    await update.message.reply_text(texto)

async def acceso(update: Update, context: ContextTypes.DEFAULT_TYPE):
    """Comando /acceso — muestra el último acceso registrado."""
    if ultimo_acceso_fecha is None:
        await update.message.reply_text("No se ha registrado ningún acceso todavía.")
    else:
        fecha_str = ultimo_acceso_fecha.strftime("%Y-%m-%d %H:%M:%S")
        await update.message.reply_text(
            f"Último acceso registrado:\n"
            f"Fecha y hora: {fecha_str}\n"
            f"Distancia: {ultimo_acceso_distancia:.1f} cm\n"
            f"Acceso #{total_accesos}"
        )

#  LECTURA SERIAL 

async def leer_arduino(context: ContextTypes.DEFAULT_TYPE):
    """Job periódico: lee líneas del serial y actualiza el estado."""
    global estado_puerta, ultimo_acceso_fecha, ultimo_acceso_distancia
    global total_accesos, distancia_actual

    while arduino.in_waiting:
        try:
            linea = arduino.readline().decode(errors="ignore").strip()
        except serial.SerialException:
            break

        if not linea:
            continue

        #  Evento de ACCESO: "ACCESO:distancia:contador" 
        if linea.startswith("ACCESO:"):
            partes = linea.split(":")
            if len(partes) >= 3:
                try:
                    ultimo_acceso_distancia = float(partes[1])
                    total_accesos = int(partes[2])
                except ValueError:
                    pass
            ultimo_acceso_fecha = datetime.now()
            estado_puerta = "abierta"

            # Enviar notificación al usuario
            if CHAT_ALERTAS:
                fecha_str = ultimo_acceso_fecha.strftime("%H:%M:%S")
                await context.bot.send_message(
                    chat_id=CHAT_ALERTAS,
                    text=(
                        f"Persona detectada a {ultimo_acceso_distancia:.1f} cm\n"
                        f"Puerta abierta — {fecha_str}\n"
                        f"Acceso #{total_accesos}"
                    )
                )

        #  Evento de CIERRE 
        elif linea == "CIERRE":
            estado_puerta = "cerrada"
            if CHAT_ALERTAS:
                await context.bot.send_message(
                    chat_id=CHAT_ALERTAS,
                    text="Puerta cerrada."
                )

        #  Evento de ALARMA 
        elif linea == "ALARMA":
            estado_puerta = "alarma"
            if CHAT_ALERTAS:
                await context.bot.send_message(
                    chat_id=CHAT_ALERTAS,
                    text="¡ALARMA! La puerta lleva demasiado tiempo abierta."
                )

        #  Lectura de distancia: "DIST:valor" 
        elif linea.startswith("DIST:"):
            try:
                distancia_actual = float(linea.split(":")[1])
            except (ValueError, IndexError):
                pass

        #  Sistema listo 
        elif linea == "SISTEMA_LISTO":
            print("Arduino/ESP32 listo.")
            if CHAT_ALERTAS:
                await context.bot.send_message(
                    chat_id=CHAT_ALERTAS,
                    text="Sistema de control de acceso iniciado."
                )

#  PROGRAMA PRINCIPAL 

def main():
    app = Application.builder().token(TOKEN).build()

    # Registrar comandos
    app.add_handler(CommandHandler("start", start))
    app.add_handler(CommandHandler("estado", estado))
    app.add_handler(CommandHandler("acceso", acceso))

    # Job periódico: revisar el serial cada segundo
    app.job_queue.run_repeating(leer_arduino, interval=1, first=2)

    print("Bot de Telegram iniciado. Presione Ctrl+C para salir.")
    app.run_polling()

if __name__ == "__main__":
    main()
