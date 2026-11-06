/*
  ============================================================
   CUARTO SEGURO - ABP de Informática  (versión con broker MQTT)
  ============================================================
   Celular (página web en GitHub)  <->  Broker MQTT (EMQX)  <->  ESP32

   - Puerta (servomotor): se abre SOLO con la contraseña y se cierra sola a los 5 s
   - Luces (LED): prender / apagar
   - Alarma de socorro: botón físico + buzzer, se maneja desde la página
   - Router (relé): se corta solo en el horario que elijas

   CÓMO SE USA
   1) Cambiá WIFI_NOMBRE y WIFI_CLAVE (el WiFi tiene que tener internet).
   2) Instalá las librerías (Herramientas > Administrar bibliotecas):
        - "PubSubClient" de Nick O'Leary
        - "ESP32Servo" de Kevin Harrington
   3) Subí el código y abrí el Monitor Serie (115200) para ver si conectó.
   4) Abrí la página: https://bmococain-max.github.io/cuarto-seguro/

   IMPORTANTE: el TEMA tiene que ser el mismo acá y en la página.
  ============================================================
*/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ESP32Servo.h>
#include <time.h>

// ---------------- CONFIGURACIÓN ----------------
const char* WIFI_NOMBRE = "NOMBRE_DEL_WIFI";   // <-- WiFi con internet
const char* WIFI_CLAVE  = "CLAVE_DEL_WIFI";    // <-- su contraseña

const char* BROKER = "broker.emqx.io";      // broker público y gratis
const int   BROKER_PUERTO = 1883;
const char* TEMA = "cuartoseguro/4deeb0";      // "canal" privado del proyecto (igual en la página)

const char* CONTRASENA_PUERTA = "030708";      // solo números

// Pines
const int PIN_SERVO  = 13;
const int PIN_LUZ    = 26;
const int PIN_BUZZER = 25;
const int PIN_BOTON  = 27;   // botón de socorro (la otra pata va a GND)
const int PIN_RELE   = 14;

const bool RELE_ACTIVO_EN_LOW = true;   // si el relé anda al revés, poné false

const int ANGULO_CERRADA = 0;
const int ANGULO_ABIERTA = 90;
const unsigned long CIERRE_AUTOMATICO_MS = 5000;

const int INTENTOS_MAX = 3;
const unsigned long BLOQUEO_MS = 30000;
// -----------------------------------------------

WiFiClient wifi;
PubSubClient mqtt(wifi);
Servo puerta;

String temaComandos, temaEstado, temaRespuesta, temaOnline;

bool puertaAbierta  = false;
bool luzPrendida    = false;
bool alarmaActiva   = false;
bool routerPrendido = true;
unsigned long momentoApertura = 0;

bool horarioActivo = false;
int  horaApagado   = 23 * 60;
int  horaPrendido  = 7 * 60;
int  ultimoEstadoHorario = -1;

bool horaDelCelu = false;
long minutosBase = 0;
unsigned long millisBase = 0;

int intentosFallidos = 0;
unsigned long bloqueadoHasta = 0;

bool botonAnterior = HIGH;
unsigned long ultimoCambioBoton = 0;
unsigned long ultimoBeep = 0;
bool buzzerEncendido = false;

bool hayQuePublicar = true;
unsigned long ultimaPublicacion = 0;
unsigned long ultimoIntentoMqtt = 0;

// ======================= HABITACIÓN =======================

void moverPuerta(bool abrir) {
  puertaAbierta = abrir;
  puerta.write(abrir ? ANGULO_ABIERTA : ANGULO_CERRADA);
  if (abrir) momentoApertura = millis();
  hayQuePublicar = true;
}

void ponerLuz(bool prender) {
  luzPrendida = prender;
  digitalWrite(PIN_LUZ, prender ? HIGH : LOW);
  hayQuePublicar = true;
}

void ponerRouter(bool prender) {
  routerPrendido = prender;
  bool nivel = RELE_ACTIVO_EN_LOW ? !prender : prender;
  digitalWrite(PIN_RELE, nivel ? HIGH : LOW);
  hayQuePublicar = true;
}

void ponerAlarma(bool activar) {
  alarmaActiva = activar;
  if (!activar) {
    buzzerEncendido = false;
    digitalWrite(PIN_BUZZER, LOW);
  }
  hayQuePublicar = true;
}

int minutosActuales() {
  struct tm t;
  if (getLocalTime(&t, 10)) return t.tm_hour * 60 + t.tm_min;
  if (!horaDelCelu) return -1;
  unsigned long pasados = (millis() - millisBase) / 60000UL;
  return (minutosBase + pasados) % 1440;
}

bool horarioDiceApagado(int ahora) {
  if (horaApagado == horaPrendido) return false;
  if (horaApagado < horaPrendido) return ahora >= horaApagado && ahora < horaPrendido;
  return ahora >= horaApagado || ahora < horaPrendido;
}

String formatoHora(int minutos) {
  char buf[6];
  sprintf(buf, "%02d:%02d", minutos / 60, minutos % 60);
  return String(buf);
}

int leerHora(const String& texto) {
  int dp = texto.indexOf(':');
  if (dp < 0) return -1;
  int h = texto.substring(0, dp).toInt();
  int m = texto.substring(dp + 1).toInt();
  if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
  return h * 60 + m;
}

// ======================= MQTT =======================

void publicarEstado() {
  int ahora = minutosActuales();
  String j = "{";
  j += "\"puerta\":" + String(puertaAbierta ? "true" : "false");
  j += ",\"luz\":" + String(luzPrendida ? "true" : "false");
  j += ",\"alarma\":" + String(alarmaActiva ? "true" : "false");
  j += ",\"router\":" + String(routerPrendido ? "true" : "false");
  j += ",\"horarioActivo\":" + String(horarioActivo ? "true" : "false");
  j += ",\"apagado\":\"" + formatoHora(horaApagado) + "\"";
  j += ",\"prendido\":\"" + formatoHora(horaPrendido) + "\"";
  j += ",\"hora\":\"" + (ahora < 0 ? String("--:--") : formatoHora(ahora)) + "\"";
  j += "}";
  mqtt.publish(temaEstado.c_str(), j.c_str(), true);   // retenido: la página lo ve apenas entra
  hayQuePublicar = false;
  ultimaPublicacion = millis();
}

void responder(const String& texto) {
  mqtt.publish(temaRespuesta.c_str(), texto.c_str());
}

void intentarAbrir(const String& clave) {
  unsigned long ahora = millis();
  if (ahora < bloqueadoHasta) {
    responder("{\"ok\":false,\"bloqueado\":" + String((bloqueadoHasta - ahora) / 1000 + 1) + "}");
    return;
  }
  if (clave == CONTRASENA_PUERTA) {
    intentosFallidos = 0;
    moverPuerta(true);
    Serial.println("Puerta ABIERTA (contraseña correcta)");
    responder("{\"ok\":true}");
  } else {
    intentosFallidos++;
    Serial.println("Contraseña incorrecta");
    if (intentosFallidos >= INTENTOS_MAX) {
      intentosFallidos = 0;
      bloqueadoHasta = ahora + BLOQUEO_MS;
      ponerAlarma(true);
      Serial.println("¡Bloqueado por intentos fallidos!");
      responder("{\"ok\":false,\"bloqueado\":" + String(BLOQUEO_MS / 1000) + "}");
    } else {
      responder("{\"ok\":false,\"quedan\":" + String(INTENTOS_MAX - intentosFallidos) + "}");
    }
  }
}

// Llegan comandos de texto, por ejemplo:  luz:1   abrir:030708   horario:23:00,07:00,1
void alRecibir(char* tema, byte* datos, unsigned int largo) {
  String msg;
  for (unsigned int i = 0; i < largo; i++) msg += (char)datos[i];
  Serial.print("Comando: ");
  Serial.println(msg.startsWith("abrir:") ? "abrir:******" : msg);

  int dp = msg.indexOf(':');
  String cmd = dp < 0 ? msg : msg.substring(0, dp);
  String val = dp < 0 ? "" : msg.substring(dp + 1);

  if (cmd == "abrir")       intentarAbrir(val);
  else if (cmd == "cerrar") moverPuerta(false);
  else if (cmd == "luz")    ponerLuz(val == "1");
  else if (cmd == "alarma") ponerAlarma(val == "1");
  else if (cmd == "router") ponerRouter(val == "1");
  else if (cmd == "hora") {
    int min = val.toInt();
    if (min >= 0 && min < 1440) { minutosBase = min; millisBase = millis(); horaDelCelu = true; }
  }
  else if (cmd == "horario") {          // formato: 23:00,07:00,1
    int c1 = val.indexOf(','), c2 = val.lastIndexOf(',');
    int apagar = leerHora(val.substring(0, c1));
    int prender = leerHora(val.substring(c1 + 1, c2));
    if (apagar >= 0) horaApagado = apagar;
    if (prender >= 0) horaPrendido = prender;
    horarioActivo = val.substring(c2 + 1) == "1";
    ultimoEstadoHorario = -1;
  }
  hayQuePublicar = true;
}

void conectarMqtt() {
  if (mqtt.connected() || millis() - ultimoIntentoMqtt < 3000) return;
  ultimoIntentoMqtt = millis();
  Serial.print("Conectando al broker... ");
  String id = "cuarto-esp32-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  // "Último deseo": si el ESP32 se desconecta, el broker avisa "0" en el tema online
  if (mqtt.connect(id.c_str(), temaOnline.c_str(), 0, true, "0")) {
    Serial.println("¡conectado!");
    mqtt.publish(temaOnline.c_str(), "1", true);
    mqtt.subscribe(temaComandos.c_str());
    hayQuePublicar = true;
  } else {
    Serial.print("falló, código ");
    Serial.println(mqtt.state());
  }
}

void conectarWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_NOMBRE, WIFI_CLAVE);
  Serial.print("Conectando a ");
  Serial.print(WIFI_NOMBRE);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.print("WiFi conectado. IP: ");
  Serial.println(WiFi.localIP());
  configTime(-3 * 3600, 0, "pool.ntp.org", "time.google.com");   // hora de Argentina
}

// ======================= SETUP Y LOOP =======================

void setup() {
  Serial.begin(115200);
  delay(500);

  temaComandos  = String(TEMA) + "/comandos";
  temaEstado    = String(TEMA) + "/estado";
  temaRespuesta = String(TEMA) + "/respuesta";
  temaOnline    = String(TEMA) + "/online";

  pinMode(PIN_LUZ, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_RELE, OUTPUT);
  pinMode(PIN_BOTON, INPUT_PULLUP);

  puerta.setPeriodHertz(50);
  puerta.attach(PIN_SERVO, 500, 2400);

  moverPuerta(false);
  ponerLuz(false);
  ponerAlarma(false);
  ponerRouter(true);

  conectarWiFi();
  mqtt.setServer(BROKER, BROKER_PUERTO);
  mqtt.setCallback(alRecibir);
  mqtt.setBufferSize(512);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Se cortó el WiFi, reconectando...");
    WiFi.reconnect();
    delay(2000);
  }
  conectarMqtt();
  mqtt.loop();

  unsigned long ahora = millis();

  // Botón de socorro
  bool boton = digitalRead(PIN_BOTON);
  if (boton != botonAnterior && ahora - ultimoCambioBoton > 50) {
    ultimoCambioBoton = ahora;
    if (boton == LOW) {
      ponerAlarma(true);
      Serial.println("¡ALARMA DE SOCORRO ACTIVADA!");
    }
    botonAnterior = boton;
  }

  // Sonido de la alarma
  if (alarmaActiva && ahora - ultimoBeep > 300) {
    ultimoBeep = ahora;
    buzzerEncendido = !buzzerEncendido;
    digitalWrite(PIN_BUZZER, buzzerEncendido ? HIGH : LOW);
  }

  // Cierre automático de la puerta
  if (puertaAbierta && ahora - momentoApertura > CIERRE_AUTOMATICO_MS) {
    moverPuerta(false);
    Serial.println("Puerta cerrada automáticamente");
  }

  // Horario del router (una vez por segundo)
  static unsigned long ultimaRevision = 0;
  if (ahora - ultimaRevision > 1000) {
    ultimaRevision = ahora;
    int minutos = minutosActuales();
    if (horarioActivo && minutos >= 0) {
      int debeApagar = horarioDiceApagado(minutos) ? 1 : 0;
      if (debeApagar != ultimoEstadoHorario) {
        ultimoEstadoHorario = debeApagar;
        ponerRouter(!debeApagar);
        Serial.println(debeApagar ? "Horario: router APAGADO" : "Horario: router PRENDIDO");
      }
    }
  }

  // Mandar el estado cuando algo cambia (y cada 30 s para actualizar la hora)
  if (mqtt.connected() && (hayQuePublicar || ahora - ultimaPublicacion > 30000)) {
    publicarEstado();
  }
}
