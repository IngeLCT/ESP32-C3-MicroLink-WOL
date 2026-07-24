# ESP32-C3 Alexa Wake-on-LAN

Firmware ESP-IDF para presentar un ESP32-C3 ante Amazon Alexa como un
interruptor Matter y encender una computadora mediante Wake-on-LAN.

## Flujo

```text
Usuario / aplicación Alexa
            |
            | Orden local Matter: encender
            v
Echo compatible con Matter
            |
            | Matter sobre Wi-Fi
            v
ESP32-C3
            |
            | Magic Packet UDP
            v
Computadora en la red local
```

No utiliza Tailscale, MicroLink, VPN, puertos abiertos en el router ni un
servidor adicional. El Echo y el ESP32-C3 deben estar en una red local que
permita tráfico entre clientes.

## Comportamiento

- Alexa registra el dispositivo como un enchufe/interruptor Matter.
- Al recibir `ON`, el ESP32-C3 envía tres Magic Packets al broadcast de su
  subred por UDP/9.
- Después vuelve automáticamente a `OFF`. Esto permite repetir el comando
  «Alexa, enciende la computadora» aunque la orden anterior ya se haya usado.
- Una orden `OFF` no apaga la computadora.
- La MAC de destino se configura en `sdkconfig.credentials`.

## Requisitos

- ESP32-C3 con al menos 4 MiB de flash.
- ESP-IDF 5.5.x.
- Un Echo o eero compatible con Matter configurado en Alexa.
- Bluetooth habilitado en el teléfono durante el emparejamiento.
- Wake-on-LAN habilitado en BIOS/UEFI y en el sistema operativo de la PC.
- ESP32-C3 y computadora en la misma subred IPv4.

## Estado de prototipo y credenciales Matter

Este firmware usa las credenciales de atestación de desarrollo incluidas por
ESP-Matter. Son apropiadas para desarrollo y pruebas de laboratorio, pero no
para distribuir un producto comercial.

La incorporación real con Alexa debe validarse con el modelo de Echo y la
versión de la aplicación Alexa que se utilizarán. Para producción se requieren
credenciales Matter propias (DAC/PAI/CD), certificación Matter y los requisitos
de certificación aplicables de Amazon.

## Configuración

```bash
cp sdkconfig.credentials.example sdkconfig.credentials
```

Edita el archivo:

```ini
CONFIG_WOL_TARGET_MAC="AA:BB:CC:DD:EE:FF"
```

También pueden ajustarse con `idf.py menuconfig`, en
`Alexa Wake-on-LAN`:

- nombre descriptivo;
- puerto UDP;
- cantidad de paquetes;
- pausa entre paquetes;
- tiempo máximo para esperar IPv4;
- tiempo para volver automáticamente a `OFF`.

Las credenciales Wi-Fi no se escriben en el firmware. Durante el alta, Alexa
las entrega al ESP32-C3 mediante el proceso estándar de comisión Matter por
BLE. Después quedan guardadas en NVS y el equipo se reconecta automáticamente.

## Compilación y flasheo

```bash
source /home/eduardo/tools/esp-idf/export.sh
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

La primera compilación descarga el componente oficial `esp_matter` desde el
registro de componentes de Espressif.

## Alta en Alexa

1. Flashea el firmware y abre el monitor serial.
2. Busca en el log el código QR o el código manual de Matter.
3. En la aplicación Alexa selecciona `Dispositivos` → `+` →
   `Agregar dispositivo` → `Matter`.
4. Escanea el QR o introduce el código manual.
5. Asigna un nombre claro, por ejemplo `Computadora`.
6. Prueba: «Alexa, enciende la computadora».

Para repetir el alta después de haber emparejado el dispositivo, ejecuta
`matter esp factoryreset` en la consola serial y reinicia.

El firmware registra explícitamente ese comando mediante
`factoryreset_register_commands()`. El restablecimiento borra la configuración
Matter y reinicia el dispositivo.

## Compilación en Windows y selección del target

Abre una terminal **ESP-IDF PowerShell**, no una consola PowerShell normal, y
ejecuta desde la carpeta principal del proyecto:

```powershell
idf.py set-target esp32c3
idf.py reconfigure
idf.py build
```

El proyecto declara `esp32c3` en tres lugares para que no dependa del selector
gráfico del IDE:

- `CMakeLists.txt`, como target predeterminado de CMake;
- `sdkconfig.defaults`, mediante `CONFIG_IDF_TARGET="esp32c3"`;
- `main/idf_component.yml`, como único target compatible.

Si una configuración anterior dejó el proyecto fijado a otro chip, limpia solo
los archivos generados y vuelve a configurar:

```powershell
Remove-Item -Recurse -Force .\build -ErrorAction SilentlyContinue
Remove-Item .\sdkconfig, .\sdkconfig.old -ErrorAction SilentlyContinue
idf.py set-target esp32c3
idf.py reconfigure
idf.py build
```

No borres `sdkconfig.credentials`, ya que ahí está la MAC objetivo.

## Notas de red

- El Magic Packet se envía al broadcast dirigido calculado con la IP y máscara
  del ESP32-C3.
- Algunas redes de invitados o configuraciones con aislamiento de clientes
  bloquean Matter, mDNS, multicast o el broadcast UDP.
- Para Wake-on-LAN desde apagado completo, la tarjeta de red de la PC debe
  permanecer energizada y configurada para aceptar el Magic Packet.

## Arquitectura técnica

- Integración Alexa: Matter sobre Wi-Fi mediante el componente oficial
  `espressif/esp_matter`.
- Tipo Matter: `On/Off Plug-in Unit`.
- Comisión inicial: BLE.
- Acción local: Magic Packet de 102 bytes, repetido por UDP.
- Estado: interruptor momentáneo; `ON` dispara WOL y retorna a `OFF`.
- Seguridad de concurrencia: el retorno del atributo `OnOff` se programa en el
  hilo Matter mediante `PlatformMgr().ScheduleWork()`.
