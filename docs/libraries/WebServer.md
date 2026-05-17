# ESP32 WebServer

**Version:** 3.3.7 (bundled with M5Stack ESP32 core)  
**Source:** Part of the M5Stack / arduino-esp32 core  
**License:** LGPL 2.1

Synchronous single-client HTTP server for ESP32. Sufficient for low-traffic configuration pages. Used in this project to serve the DMX palette/brightness configuration UI over a WiFi access point.

---

## Setup

```cpp
#include <WiFi.h>
#include <WebServer.h>

WebServer server(80);  // port 80

// Access Point mode — device broadcasts its own network
WiFi.softAP("network-name", "password");

// Register routes
server.on("/",    HTTP_GET,  handlerFn);
server.on("/set", HTTP_POST, handlerFn);

server.begin();
```

### Station mode (connect to existing network)
```cpp
WiFi.begin("ssid", "password");
while (WiFi.status() != WL_CONNECTED) delay(100);
```

---

## Loop

```cpp
void loop() {
    server.handleClient();  // must be called regularly — processes one request per call
}
```

---

## Handler functions

```cpp
void handleRoot() {
    server.send(200, "text/html", "<h1>Hello</h1>");
}

void handlePost() {
    if (server.hasArg("myfield")) {
        String val = server.arg("myfield");
    }
    server.sendHeader("Location", "/");
    server.send(303);  // redirect after POST
}
```

### `server.send()`
```cpp
void send(int code, const char* contentType, const String& content);
```

| Parameter | Example |
|-----------|---------|
| `code` | `200`, `303`, `404` |
| `contentType` | `"text/html"`, `"application/json"` |
| `content` | HTML or body string |

---

## Reading POST / GET arguments

```cpp
server.hasArg("name")           // true if field present
server.arg("name")              // returns field value as String
server.args()                   // total number of arguments
server.argName(i)               // name of argument i
```

---

## Serving large strings from PROGMEM

```cpp
const char PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>...
)rawliteral";

server.send(200, "text/html", FPSTR(PAGE));
```

Use `String::replace()` to substitute template variables before sending:
```cpp
String page = FPSTR(PAGE);
page.replace("%VALUE%", String(currentValue));
server.send(200, "text/html", page);
```

---

## WiFi AP defaults

| Setting | Value |
|---------|-------|
| Default IP | `192.168.4.1` |
| Subnet | `255.255.255.0` |
| Max clients | 4 |

```cpp
WiFi.softAPIP()  // returns the AP's IP address as IPAddress
```

---

## Limitations

- Single client at a time (requests are processed sequentially)
- No WebSocket support
- No TLS/HTTPS
- `handleClient()` is blocking for the duration of a single request
