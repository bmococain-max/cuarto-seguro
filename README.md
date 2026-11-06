# Cuarto Seguro

Proyecto ABP de Informática: una habitación inteligente y segura, controlada desde el celular.

**Página de control:** https://bmococain-max.github.io/cuarto-seguro/

```
Celular (página en GitHub Pages)  <->  Broker MQTT (EMQX)  <->  ESP32  ->  Habitación
```

## Qué hace

| Función | Componente | Cómo se controla |
|---|---|---|
| Puerta con contraseña | Servomotor | Se abre solo con la clave numérica. Se cierra sola a los 5 s |
| Luces | LED | Prender / apagar |
| Alarma de socorro | Pulsador + buzzer | Se activa con el botón físico o la página; se apaga desde la página |
| Corte de WiFi por horario | Relé (simula el router) | Manual o automático a la hora que configures |

**Ciberseguridad:** la contraseña la verifica el ESP32 (no la página). Con 3 intentos fallidos se bloquea 30 segundos y suena la alarma.

## Temas MQTT (broker.emqx.io)

| Tema | Quién publica | Qué tiene |
|---|---|---|
| cuartoseguro/4deeb0/comandos | Página | luz:1, abrir:XXXXXX, cerrar, alarma:0, router:1, horario:23:00,07:00,1, hora:600 |
| cuartoseguro/4deeb0/estado | ESP32 | Estado de todo en JSON (retenido) |
| cuartoseguro/4deeb0/respuesta | ESP32 | Resultado de la contraseña |
| cuartoseguro/4deeb0/online | ESP32 | 1 conectado / 0 desconectado |

## Conexiones

| Componente | Pin del ESP32 |
|---|---|
| Servo (señal) | GPIO 13 |
| LED luces (+ resistencia 220 Ω) | GPIO 26 |
| Buzzer | GPIO 25 |
| Pulsador de socorro (otra pata a GND) | GPIO 27 |
| Relé (IN) | GPIO 14 |

Servo y relé a 5V (VIN) y GND. Todos los GND juntos.

## Cómo cargarlo

1. Arduino IDE con el paquete **esp32** de Espressif.
2. Librerías: **PubSubClient** (Nick O'Leary) y **ESP32Servo** (Kevin Harrington).
3. En `cuarto_seguro/cuarto_seguro.ino` poné el nombre y la clave del WiFi (tiene que tener internet).
4. Subilo y mirá el Monitor Serie (115200): tiene que decir "¡conectado!".
