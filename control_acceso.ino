/*
 * ============================================================
 *  SISTEMA DE CONTROL DE ACCESO CON CERRADURA ELECTROMAGNETICA
 *  Materia: Integracion Tecnologica
 *  Placa: ESP32 DevKit (30 pines)
 *
 *  Funcionamiento general:
 *  - Lee tarjetas NFC/RFID de 13,56 MHz con un modulo PN532 (I2C).
 *  - Si la tarjeta esta autorizada, activa el rele y la cerradura
 *    se abre durante 5 segundos (LED verde + mensaje en pantalla).
 *  - Si no esta autorizada, muestra "ACCESO DENEGADO" y el LED
 *    rojo parpadea.
 *  - Un pulsador interior abre la puerta desde adentro.
 *  - Tras 10 segundos sin actividad se apaga la luz de la pantalla.
 * ============================================================
 */

// ------------------------------------------------------------
// LIBRERIAS
// ------------------------------------------------------------
#include <Wire.h>               // Comunicacion I2C (pines 21 SDA y 22 SCL)
#include <Adafruit_PN532.h>     // Manejo del lector NFC PN532
#include <LiquidCrystal_I2C.h>  // Manejo de la pantalla LCD 20x4 por I2C

// ------------------------------------------------------------
// PINES DEL ESP32
// Se declaran como constantes para no repetir numeros en el codigo
// y poder cambiarlos en un solo lugar.
// ------------------------------------------------------------
const int RELE      = 26;  // Salida al IN del rele (HIGH = abre la cerradura)
const int LED_ROJO  = 25;  // Pata roja del LED bicolor (con resistencia de 220 ohm)
const int LED_VERDE = 33;  // Pata verde del LED bicolor (con resistencia de 220 ohm)
const int BOTON     = 32;  // Pulsador interior (la otra pata va a GND)

// ------------------------------------------------------------
// CONFIGURACION DE TIEMPOS
// ------------------------------------------------------------
const int SEGUNDOS_ABIERTO = 5;                  // Tiempo que la cerradura queda abierta
const unsigned long TIEMPO_INACTIVIDAD = 10000;  // 10 s sin actividad -> se apaga la luz del LCD

// ------------------------------------------------------------
// TARJETAS AUTORIZADAS
// Cada usuario tiene el UID de su tarjeta (en hexadecimal) y un nombre.
// Para agregar una tarjeta: acercarla, leer el UID que aparece en
// "ACCESO DENEGADO" y sumar una linea a la lista.
// ------------------------------------------------------------
struct Usuario {
  const char* uid;     // Numero unico de la tarjeta
  const char* nombre;  // Nombre que se muestra en pantalla
};

Usuario autorizados[] = {
  {"2FD4034D", "Lucas"},
  // {"XXXXXXXX", "Otro nombre"},
};

// Cantidad de usuarios: tamano total de la lista / tamano de un usuario
const int CANT = sizeof(autorizados) / sizeof(autorizados[0]);

// ------------------------------------------------------------
// OBJETOS DE LOS MODULOS
// ------------------------------------------------------------
Adafruit_PN532 nfc(-1, -1);  // PN532 por I2C: -1 = sin pines IRQ ni RESET conectados
LiquidCrystal_I2C* lcd;      // Puntero: el LCD se crea en setup() cuando se conoce su direccion

// ------------------------------------------------------------
// VARIABLES GLOBALES
// ------------------------------------------------------------
unsigned long ultimaActividad = 0;  // Momento (en ms) de la ultima accion del usuario
bool luzPrendida = true;            // Estado de la luz de fondo del LCD

// ============================================================
// FUNCIONES PERSONALIZADAS
// ============================================================

// pantalla(): escribe hasta 4 lineas en el LCD.
// Parametros: l1..l4 = texto de cada linea. l2, l3 y l4 son opcionales
// (si no se pasan quedan vacias), asi se reutiliza para cualquier mensaje.
void pantalla(String l1, String l2 = "", String l3 = "", String l4 = "") {
  lcd->clear();                              // Borra lo que habia
  lcd->setCursor(0, 0); lcd->print(l1);      // Linea 1
  lcd->setCursor(0, 1); lcd->print(l2);      // Linea 2
  lcd->setCursor(0, 2); lcd->print(l3);      // Linea 3
  lcd->setCursor(0, 3); lcd->print(l4);      // Linea 4
}

// despertar(): prende la luz del LCD si estaba apagada y reinicia
// el contador de inactividad. Se llama ante cualquier accion del usuario.
void despertar() {
  if (!luzPrendida) {        // Solo si la luz estaba apagada
    lcd->backlight();        // Prende la luz de fondo
    luzPrendida = true;
  }
  ultimaActividad = millis();  // Guarda el momento de la actividad
}

// reposo(): deja el sistema en su estado normal de espera.
// Cerradura cerrada, LED rojo y mensaje "Acerque tarjeta".
void reposo() {
  digitalWrite(RELE, LOW);        // Rele apagado -> contacto NC cerrado -> cerradura alimentada (cerrada)
  digitalWrite(LED_VERDE, LOW);   // Apaga el verde
  digitalWrite(LED_ROJO, HIGH);   // Prende el rojo (puerta cerrada)
  pantalla("  CONTROL DE ACCESO", "", "  Acerque tarjeta");
  ultimaActividad = millis();     // Empieza a contar el tiempo de inactividad
}

// abrir(): abre la cerradura durante SEGUNDOS_ABIERTO con cuenta regresiva.
// Parametros: titulo = primera linea (ej. "ACCESO PERMITIDO"),
//             mensaje = segunda linea (ej. "Bienvenido Lucas").
// Se usa tanto para tarjetas autorizadas como para el pulsador interior.
void abrir(String titulo, String mensaje) {
  digitalWrite(LED_ROJO, LOW);    // Apaga el rojo
  digitalWrite(LED_VERDE, HIGH);  // Prende el verde
  digitalWrite(RELE, HIGH);       // Rele activado -> corta la cerradura -> puerta abierta

  // Cuenta regresiva: actualiza la pantalla una vez por segundo
  for (int s = SEGUNDOS_ABIERTO; s > 0; s--) {
    pantalla(titulo, mensaje, "", "Cerrando en " + String(s) + " s");
    delay(1000);
  }

  reposo();  // Al terminar, vuelve a cerrar y queda en espera
}

// denegar(): muestra que la tarjeta no esta autorizada.
// Parametro: id = UID de la tarjeta leida (se muestra en pantalla
// para poder darla de alta si hace falta).
void denegar(String id) {
  digitalWrite(LED_VERDE, LOW);
  pantalla("  ACCESO DENEGADO", "", "Tarjeta: " + id);

  // Parpadeo del LED rojo: 6 cambios de 250 ms (3 parpadeos)
  for (int i = 0; i < 6; i++) {
    digitalWrite(LED_ROJO, i % 2 == 0 ? LOW : HIGH);  // Pares: apaga / impares: prende
    delay(250);
  }

  delay(1500);  // Deja el mensaje un momento mas en pantalla
  reposo();
}

// buscarNombre(): busca un UID en la lista de autorizados.
// Parametro: id = UID leido.
// Devuelve: el nombre del usuario si esta autorizado, o "" si no esta.
String buscarNombre(String id) {
  for (int i = 0; i < CANT; i++) {          // Recorre toda la lista
    if (id == autorizados[i].uid) {         // Si el UID coincide...
      return autorizados[i].nombre;         // ...devuelve el nombre
    }
  }
  return "";  // No se encontro: tarjeta no autorizada
}

// ============================================================
// SETUP: se ejecuta una sola vez al encender el ESP32
// ============================================================
void setup() {
  // --- Configuracion de pines ---
  pinMode(RELE, OUTPUT);
  digitalWrite(RELE, LOW);         // Arranca con la cerradura cerrada (seguridad)
  pinMode(LED_ROJO, OUTPUT);
  pinMode(LED_VERDE, OUTPUT);
  pinMode(BOTON, INPUT_PULLUP);    // Pull-up interno: suelto = HIGH, apretado = LOW

  // --- Comunicaciones ---
  Serial.begin(115200);            // Monitor serie para ver los UID leidos
  Wire.begin(21, 22);              // Bus I2C: SDA = 21, SCL = 22 (LCD y PN532 comparten el bus)

  // --- Pantalla LCD ---
  // Los modulos I2C vienen con direccion 0x27 o 0x3F: se prueba cual responde.
  byte dir = 0x27;
  Wire.beginTransmission(0x3F);
  if (Wire.endTransmission() == 0) dir = 0x3F;  // 0 = el dispositivo respondio
  lcd = new LiquidCrystal_I2C(dir, 20, 4);      // Pantalla de 20 columnas x 4 filas
  lcd->init();
  lcd->backlight();

  // --- Lector NFC PN532 ---
  nfc.begin();
  if (!nfc.getFirmwareVersion()) {             // Si no responde, no se puede seguir
    pantalla("ERROR:", "Lector NFC no", "detectado");
    while (true) delay(10);                    // Detiene el programa
  }
  nfc.SAMConfig();  // Configura el PN532 para leer tarjetas

  reposo();  // Estado inicial: cerrado y esperando tarjeta
}

// ============================================================
// LOOP: se repite continuamente
// ============================================================
void loop() {
  // --- 1) Ahorro de pantalla ---
  // Si la luz esta prendida y pasaron mas de 10 s sin actividad, se apaga.
  if (luzPrendida && millis() - ultimaActividad > TIEMPO_INACTIVIDAD) {
    lcd->noBacklight();
    luzPrendida = false;
  }

  // --- 2) Pulsador interior ---
  // Con INPUT_PULLUP el boton apretado se lee como LOW.
  if (digitalRead(BOTON) == LOW) {
    despertar();
    abrir("CERRADURA ABIERTA", "Por boton interior");
    return;  // Termina esta vuelta del loop y empieza otra
  }

  // --- 3) Lectura de tarjeta ---
  uint8_t uid[7];   // Espacio para el UID (hasta 7 bytes)
  uint8_t largo;    // Cantidad de bytes que tiene el UID leido

  // Espera una tarjeta como maximo 200 ms, asi el boton sigue respondiendo rapido
  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &largo, 200)) {
    despertar();

    // Convierte el UID de bytes a texto hexadecimal (ej. "2FD4034D")
    String id = "";
    for (int i = 0; i < largo; i++) {
      if (uid[i] < 0x10) id += "0";   // Agrega el 0 a la izquierda (ej. 0x0A -> "0A")
      id += String(uid[i], HEX);
    }
    id.toUpperCase();
    Serial.println("UID: " + id);     // Lo muestra en el monitor serie

    // --- 4) Decision de acceso ---
    String nombre = buscarNombre(id);
    if (nombre != "") {
      abrir(" ACCESO PERMITIDO", "Bienvenido " + nombre);  // Autorizada
    } else {
      denegar(id);                                         // No autorizada
    }

    delay(1000);  // Evita leer la misma tarjeta dos veces seguidas
  }
}
