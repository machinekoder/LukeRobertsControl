#define ETH_CLK_MODE ETH_CLOCK_GPIO17_OUT
#define ETH_PHY_POWER 12

#define UART_BUF_SIZE (1024)
#define CMD_RELAY_OPEN "\xa0\x01\x00\xa1"
#define CMD_RELAY_CLOSE "\xa0\x01\x01\xa2"

#include <string>
#include <Arduino.h>
//#include <ETH.h>
//#include <WiFi.h>
#include <AsyncTCP.h>
#include <SPIFFS.h>
#include <ESPAsyncWebServer.h>
#include <esp_wifi.h>
#include <WiFiGeneric.h>

#include "app_utils.h"
#include "BleGattClient.h"
#include "lukeroberts.h"
#include "mqtt_handler.h"
#include "webpages.h"

#include "driver/uart.h"
#include "driver/gpio.h"

#if defined(ROTARY_PIN_A)
#if !defined(ROTARY_PIN_B)
#error "ROTARY configuration error - PIN A and B must be defined"
#endif
#include "rotaryencoder.h"

#if !defined(ROTARY_STEP_VALUE)
#define ROTARY_STEP_VALUE 5
#endif
#endif

#include <AceButton.h>

using namespace app_utils;
#if defined(ROTARY_PIN_A)
using namespace rotary_encoder;
#endif
using namespace ace_button;
WiFiClient network_client;
AppUtils app;

extern boolean ble_connected;
extern uint8_t max_scenes;

// RTC_DATA_ATTR bool powerstate = false;
// RTC_DATA_ATTR int16_t dimlevel = 50;
// RTC_DATA_ATTR int16_t colortemperature = 3000;
// RTC_DATA_ATTR int16_t scene = 01;

// publishing mqtt message is a blocking operation
// use a task instead that sends queues messages

MqttPublish mqtt;

// provide a lamdba to call the mqtt client (decouples mqtt library used)
RTC_DATA_ATTR LukeRobertsLamp lr([](const char *topic, const char *data,
                                    bool retained, uint8_t qos) {
  mqtt.queue(topic, data, retained, qos);
});

AsyncWebServer server(80);

void notFound(AsyncWebServerRequest *request) {
  request->send(404, "text/plain", "Not found");
}

///// Command parsing //////////////
bool get_powerstate() { return lr.get_powerstate(); }

bool set_powerstate(bool value) {

#ifndef RELAY_PIN
  lr.set_powerstate(value, true);
#else
  digitalWrite(RELAY_PIN, value ? HIGH : LOW);
  //  log_i("PORT %d = %d",RELAY_PIN,value);
  lr.sync_powerstate(value);
#endif
  if (mqtt.connected() && value == false) {
    char json[32];
    mqtt.queue("stat/" HOSTNAME "/RESULT", lr.create_state_message());
    snprintf(json, sizeof(json), "%d", lr.state().brightness);
    snprintf(json, sizeof(json), "%d", lr.state().mired);
    mqtt.queue("stat/" HOSTNAME "/CT", json);
    mqtt.queue("stat/" HOSTNAME "/POWER", value ? "ON" : "OFF");
  }

  return get_powerstate();
}
bool set_powerstate(const String &value) {
  if (value.equals("0") || value.equals("off") || value.equals("false") ||
      value.equals("0")) {
    set_powerstate(false);
  } else if (value.equals("1") || value.equals("on") || value.equals("true") ||
             value.equals("1")) {
    set_powerstate(true);
  } else if (value.equals("toggle")) {
    set_powerstate(!get_powerstate());
  }
  return get_powerstate();
}

int get_dimmer_value() { return lr.state().brightness; }

int set_dimmer_value(int new_level) {
  if (new_level > 100) {
    new_level = 100;
  }
  if (new_level < 0) {
    new_level = 0;
  }
  log_i("Set Dimmer Level to %d", new_level);
  if (new_level == 0) { // dimmer level 0 is power off
    lr.set_powerstate(false, true);
  } else {
    lr.set_dimmer(new_level, true);
  }

  return new_level;
}

int set_dimmer_value(const String &value) {
  bool parsing_success = false;
  char *p;
  long numvalue = strtol(value.c_str(), &p, 10);
  auto brightness = lr.state().brightness;
  if (*p && *p != '.') {
    // p points to the char after the last digit. so if the value is a number it
    // points to the terminating \0

    // is there another param e.g. "dimmer up 5"
    auto param = value.indexOf(' ');
    String param_value;
    String cmd;
    int step_value = 10;
    if (param > 0) {
      param_value = value.substring(param + 1);
      cmd = value.substring(0, param);
      step_value = atol(param_value.c_str());
    } else {
      cmd = value;
    }
    if (cmd.equals("+") || cmd.equals("up")) {
      parsing_success = true;
      brightness += step_value;
      log_d("Increase Dimmer Level to %d", brightness);
    } else {
      if (cmd.equals("-") || cmd.equals("down")) {
        parsing_success = true;
        brightness -= step_value;
      }
    }
  } else {
    parsing_success = true;
    brightness = (int)numvalue;
  }
  if (parsing_success) {
    set_dimmer_value(brightness);
  }
  return brightness;
}

int get_scene() { return lr.state().scene; }

int set_scene(int numvalue) {
  auto scene = numvalue & 0xFF;
  if (scene > lr.scenes.size() && scene != 0xFF)
    scene = 1;
  if (scene < 1 && scene != 0xFF)
    scene = lr.scenes.size();

  log_i("Set Scene Level to %d", scene);
  lr.set_scene(scene, true);
  return scene;
}

int set_scene(const String &value) {
  bool parsing_success = false;
  char *p;
  long numvalue = strtol(value.c_str(), &p, 10);
  auto scene = lr.state().scene;
  if (*p) {
    log_d("Scene %s", value.c_str());
    // p points to the char after the last digit. so if the value is a number it
    // points to the terminating \0

    // is there another param e.g. "scene up 2"
    auto param = value.indexOf(' ');
    String param_value;
    String cmd;
    int step_value = 1;
    if (param > 0) {
      param_value = value.substring(param + 1);
      cmd = value.substring(0, param);
      step_value = atol(param_value.c_str());
    } else {
      cmd = value;
    }
    if (cmd.equals("+") || cmd.equals("up")) {
      parsing_success = true;
      scene += step_value;
      log_i("Increase scene %d", scene);
    } else {
      if (cmd.equals("-") || cmd.equals("down")) {
        parsing_success = true;
        scene -= step_value;
      }
    }
  } else {
    parsing_success = true;
    scene = (uint8_t)numvalue;
  }
  if (parsing_success) {
    set_scene(scene);
  }
  return scene;
}

int set_colortemperature_mired(int numvalue) {
  if (numvalue > 370) {
    numvalue = 370;
  }
  if (numvalue < 250) {
    numvalue = 250;
  }
  log_i("Set Color temperature to %d mired", numvalue);

  lr.set_colortemperature_mired(numvalue, true);
  return lr.state().mired;
}

int get_colortemperature_kelvin() { return lr.state().kelvin; }

int set_colortemperature_kelvin(int numvalue) {
  if (numvalue > 4000) {
    numvalue = 4000;
  }
  if (numvalue < 2700) {
    numvalue = 2700;
  }
  log_i("Set Color temperature to %d kelvin", numvalue);

  lr.set_colortemperature_kelvin(numvalue, true);
  return lr.state().kelvin;
}
int set_colortemperature(const String &value, bool use_kelvin = false) {
  bool parsing_success = false;
  char *p;
  long numvalue = strtol(value.c_str(), &p, 10);
  auto current_value = use_kelvin ? lr.state().kelvin : lr.state().mired;
  if (*p && *p != '.') {
    // p points to the char after the last digit. so if the value is a number it
    // points to the terminating \0

    // is there another param e.g. "dimmer up 5"
    auto param = value.indexOf(' ');
    String param_value;
    String cmd;
    int step_value = use_kelvin ? 10 : 1;
    if (param > 0) {
      param_value = value.substring(param + 1);
      cmd = value.substring(0, param);
      step_value = atol(param_value.c_str());
    } else {
      cmd = value;
    }
    if (cmd.equals("+") || cmd.equals("up")) {
      parsing_success = true;
      current_value += step_value;
      log_i("Increase Color Temperature to %d", current_value);
    } else {
      if (cmd.equals("-") || cmd.equals("down")) {
        parsing_success = true;
        current_value -= step_value;
      }
    }
  } else {
    parsing_success = true;
    current_value = (int)numvalue;
  }
  if (parsing_success) {
    if (use_kelvin) {
      set_colortemperature_kelvin(current_value);
    } else {
      set_colortemperature_mired(current_value);
    }
  }
  return current_value;
}
#ifdef USE_SCENE_MAPPER
int set_scene_brightness(const String &value) {
  char *p;
  strtol(value.c_str(), &p, 10);
  if (*p && *p != '.') {
    log_d("Map Scene %s", value.c_str());
    // p points to the char after the last digit. so if value is a number it
    // points to the terminating \0

    // is there another param e.g. "scene up 2"
    auto param = value.indexOf(' ');

    String param_value;
    String cmd;
    if (param > 0) {
      uint8_t scene;
      int dim_level = 50;
      param_value = value.substring(param + 1);
      cmd = value.substring(0, param);
      scene = atol(cmd.c_str());
      dim_level = atol(param_value.c_str());
      SceneMapper::set_brightness(scene, dim_level);
    }
  }
  return SceneMapper::size();
}

int set_scene_colortemperature(const String &value) {
  char *p;
  strtol(value.c_str(), &p, 10);
  if (*p && *p != '.') {
    log_d("Map Scene %s", value.c_str());
    // p points to the char after the last digit. so if value is a number it
    // points to the terminating \0

    // is there another param e.g. "scene up 2"
    auto param = value.indexOf(' ');

    String param_value;
    String cmd;
    if (param > 0) {
      uint8_t scene;
      int kelvin = 3000;
      param_value = value.substring(param + 1);
      cmd = value.substring(0, param);
      scene = atol(cmd.c_str());
      kelvin = atol(param_value.c_str());
      SceneMapper::set_colortemperature(scene, kelvin);
    }
  }
  return SceneMapper::size();
}
#endif

static bool relay_state = false;

bool get_relais() {
  return relay_state;
}

void set_relais(bool on) {
  if (on) {
    log_d("Set Relay on");
    uart_write_bytes(UART_NUM_2, CMD_RELAY_CLOSE, 4);
    relay_state = true;
  }
  else {
    log_d("Set Relay off");
    uart_write_bytes(UART_NUM_2, CMD_RELAY_OPEN, 4);
    relay_state = false;
  }
}

bool set_relais(const String &value) {
  if (value.equals("0") || value.equals("off") || value.equals("false") ||
      value.equals("0")) {
      set_relais(false);
  } else if (value.equals("1") || value.equals("on") || value.equals("true") ||
          value.equals("1")) {
      set_relais(true);
  } else if (value.equals("toggle")) {
    set_relais(!get_relais());
  }
  return get_relais();
}

void queue_ble_command(const String &value) {
  int len = value.length() / 2;
  char *p;
  unsigned long byte;
  uint8_t ble_data[32];
  char two_chars[3];
  const char *numptr = value.c_str();
  for (int i = 0; i < len && i < sizeof(ble_data); i++) {
    two_chars[0] = *numptr++;
    two_chars[1] = *numptr++;
    two_chars[2] = '\0';
    byte = strtoul(two_chars, &p, 16);
    ble_data[i] = byte & 0xFF;
    log_v(" BLE custom %d : %d", i, ble_data[i]);
  }

  lr.send_custom(ble_data, len);
}

bool set_uplight(const char *json) {
  long duration = 0;
  long saturation = 0;
  long hue = 0;
  long brightness = 0;

  bool success = false;
  if (!get_jsonvalue(json, "duration", duration)) {
    success = get_jsonvalue(json, "d", duration);
  }

  success = get_jsonvalue(json, "saturation", saturation);

  if (!success) {
    success = get_jsonvalue(json, "s", saturation);
  }
  if (success) {
    success = get_jsonvalue(json, "hue", hue);
    if (!success) {
      success = get_jsonvalue(json, "h", hue);
    }
  }
  if (success) {
    success = get_jsonvalue(json, "brightness", brightness);
    if (!success) {
      success = get_jsonvalue(json, "b", brightness);
    }
  }
  if (success) {
    lr.set_intermmediate_uplight(duration, saturation, hue, brightness);
  }
  return success;
}

bool set_downlight(const char *json) {
  long duration = 0;
  long kelvin = 0;
  long brightness = 0;

  bool success = false;

  char onoff_state[16];
  if (get_jsonvalue(json, "state", onoff_state, sizeof(onoff_state))) {
    String value = onoff_state;
    value.trim();
    value.toLowerCase();
    if (value == "off" || value == "0" || value == "false") {
      set_powerstate(false);
      // exit routine. if state is poweroff no need to parse remaining
      // properties
      return true;
    }
#ifndef RELAY_PIN
    lr.set_powerstate(true, false);

#else
    if (value == "on" || value == "1" || value == "true") {
      set_powerstate(true);
    }
#endif
  }

  bool have_brightness = true;
  success = get_jsonvalue(json, "brightness", brightness);
  if (!success) {
    success = get_jsonvalue(json, "b", brightness);
    if (!success) {
      success = get_jsonvalue(json, "dimmer", brightness);
      brightness = brightness * 255 / 100;
    } else {
      have_brightness = false;
    }
  }

  bool have_ct = true;
  if (success) {
    if (!get_jsonvalue(json, "duration", duration)) {
      duration = 0;
    }
    success = get_jsonvalue(json, "kelvin", kelvin);
    if (!success) {
      success = get_jsonvalue(json, "k", kelvin);
      if (!success) {
        success = get_jsonvalue(json, "ct", kelvin);
        if (success) {
          kelvin = lr.switch_kelvin_mired(kelvin);
        } else {
          have_ct = false;
        }
      }
    }
  }
  if (success) {
    lr.set_intermmediate_downlight(duration, kelvin, brightness);
  } else {
    if (have_brightness) {
      lr.set_dimmer(brightness * 100 / 255, true);
    }
    if (have_ct) {
      lr.set_colortemperature_kelvin(kelvin, true);
    }
  }

  return success;
}

unsigned long last_mqttping = millis();

bool parse_command(String cmd, String value) {

  log_d("%ld Start Parsing %s %s ", millis(), cmd.c_str(), value.c_str());
  bool has_value = value.length() > 0;

  // is this a json command
  if (cmd.equals("multi")) {
     int position_space = value.indexOf(' ');
     int position_multi = value.indexOf("multi");
     int position_start = 0;
     if (position_multi > -1) {
        parse_command(value.substring(0, position_space),
                      value.substring(position_space + 1, position_multi - 1));
        position_start = position_multi;
        position_space = value.indexOf(' ', position_multi);
      }
     if (position_space != -1) {
        parse_command(value.substring(position_start, position_space),
                    value.substring(position_space + 1));
      }
      return true;
  } else if (cmd.equals("uplight")) {
    if (has_value) {
      set_uplight(value.c_str());
    }
    return true;
  } else if (cmd.equals("downlight")) {
    if (has_value) {
      set_downlight(value.c_str());
    }
    return true;
  } else if (cmd.equals("power")) {
    if (has_value) {
      set_powerstate(value);
    }
    return true;
  } else if (cmd.equals("dimmer")) {
    if (has_value) {
      set_dimmer_value(value);
    } else { /* return state */
    }
    return true;
  } else if (cmd.equals("scene")) {
    if (has_value) {
      set_scene(value);
    } else { /* return state */
    }
    return true;
  } else if (cmd.equals("ct")) {
    if (has_value) {
      set_colortemperature(value, false);
    } else { /* return state */
    }
    return true;
  } else if (cmd.equals("kelvin")) {
    if (has_value) {
      set_colortemperature(value, true);
    } else { /* return state */
    }
    return true;
#ifdef USE_SCENE_MAPPER
  } else if (cmd.equals("mapscene")) {
    if (has_value) {
      set_scene_brightness(value);
    } else { /* return state */
    }
    return true;
#endif
  } else if (cmd.equals("relais")) {
    if (has_value) {
      set_relais(value);
    } else { /* return state */
    }
    return true;
  } else if (cmd.equals("ota")) {
    AppUtils::setupOta();
    mqtt.queue("tele/" HOSTNAME "/ota",
               "{\"ota\":true,\"state\":\"waiting\",\"port\":3232}");
    return true;
  } else if (cmd.equals("result") || cmd.equals("mqttping")) {
    last_mqttping = millis();
    log_d("got mqtt ping");
    mqtt.queue("tele/" HOSTNAME "/state",
               lr.create_state_message(
                   app.ota_started()
                       ? "{\"ota\":true,\"state\":\"waiting\",\"port\":3232}"
                       : nullptr));
    return true;
  } else if (cmd.equals("reboot") || cmd.equals("restart")) {
    log_i("------- REBOOT -------");
    yield();
    delay(500);
    app.fast_restart();
  } else if (cmd.equals("blecustom") && has_value) {
    queue_ble_command(value);
  }
  return false;
}
////// PARSING End ///////////

// Replaces placeholder with button section in your web page
String processor(const String &var) {
  // Serial.println(var);
  if (var == "DIMVALUE") {
    return String(lr.state().brightness);
  }
  if (var == "CTVALUE") {
    return String(lr.state().mired);
  }

  if (var == "CHECKED") {
    return lr.state().power ? "checked" : "";
  }
  if (var == "ANAUS") {
    return lr.state().power ? "An" : "Aus";
  }

  if (var == "POWER") {
    return lr.state().power ? "On" : "Off";
  }
  if (var == "SCENES") {

    String scene_html = "<br><select id=\"sceneselect\" name=\"scenes\" "
                        "onchange=\"updateScene(this)\"  size=\"" +
                        String(lr.scenes.size() - 1) + String("\" >");
    for (const auto &s : lr.scenes) {
      if (s.first != 0) {
        scene_html += "<option" +
                      String(lr.state().scene == s.first ? " selected" : "") +
                      " value=\"" + String(s.first) + "\">" + s.second.c_str() +
                      "</option>";
      }
    }
    scene_html += "</select>";
    log_v("HTML: %s", scene_html.c_str());
    return scene_html;
  }

  return String();
}

static const char *PARAM_CMD = "cmnd";
#if defined(ROTARY_PIN_A)
RotaryEncoderButton rotary;
#endif

// odd place for an include but the button settings depend on the functions
// defined above
// until proper prototypes are created the include has to stay here
// the button code was moved out because it is pretty repeptive, simple but long
#include "buttons.h"

#include <esp_panic.h>

void setup() {

/* Used to help narrowing down a heap corruption */
/*
xTaskCreatePinnedToCore( [](void*){
  esp_set_watchpoint(0, (void *)0xfffba5c4, 4, ESP_WATCHPOINT_STORE);
  vTaskDelete(0);
},"wp",4096,nullptr,1,nullptr,1);
*/

// init uart
uart_config_t uart_config = {
        .baud_rate = 9600,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };

ESP_ERROR_CHECK(uart_param_config(UART_NUM_2, &uart_config));
uart_set_pin(UART_NUM_2, GPIO_NUM_17, GPIO_NUM_16, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
//ESP_ERROR_CHECK(uart_set_pin(UART_NUM_2, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
ESP_ERROR_CHECK(uart_driver_install(UART_NUM_2, UART_BUF_SIZE * 2, 0, 0, NULL, 0));

//  esp_wifi_stop();
#ifdef USE_ETHERNET
  app.set_hostname(HOSTNAME);
#else
  app.set_hostname(HOSTNAME).set_ssid(WIFISID).set_password(WIFIPASSWORD);
#endif

  app.start_network();
  app.start_network_keepalive();

  while (!app.network_connected()) {
    delay(100);
  }
  mqtt.init(network_client, parse_command);

#ifdef LR_BLEADDRESS
  lr.client().init(NimBLEAddress(LR_BLEADDRESS, 1), serviceUUID);
#else
#pragma message(                                                               \
    "NO BLE Device Address provided. Scanning for a Luke Roberts Lamp during startup")
  auto device_addr = scan_for_device(serviceUUID);
  log_i("DEVICE : %s", device_addr.toString().c_str());
  lr.client().init(device_addr, serviceUUID);
#endif
  lr.init();
  // Route for root / web page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html, processor);
  });

  // Send a GET request to <IP>/get?message=<message>
  server.on("/cm", HTTP_GET, [](AsyncWebServerRequest *request) {
    String message = "";
    if (request->hasParam(PARAM_CMD)) {
      message = request->getParam(PARAM_CMD)->value();
      message.trim();
      message.toLowerCase();
      int position_space = message.indexOf(' ');
      if (position_space != -1) {
        parse_command(message.substring(0, position_space),
                      message.substring(position_space + 1));
      }
      request->send(
          200, "application/json",
          lr.create_state_message(
              app.ota_started() ? "{\"ota\":true ,\"port\":3232}" : nullptr));
    } else {
      message = "Not a valid command message sent";
      request->send(400, "text/plain", "GET: " + message);
    }
  });

  server.onNotFound(notFound);
  server.begin();

  last_mqttping = millis();
  mqtt.start();
  if (!mqtt.mqtt_connect()) {
    log_i("mqtt connect failed");
  }
  app.on_network_connect = std::bind(&MqttPublish::mqtt_connect, &mqtt);
  mqtt.queue("tele/" HOSTNAME "/LWT", "Online", true);

#if TESTONLYMQTT == 1
  lr.client().disable_ble(true);
#else
  bool result;
  int attempts = 0;
  while (!(result = lr.client().connect_to_server(
               lr.charUUID(), []() { lr.get_all_scenes(lr.client()); }))) {
    delay(1000);
    if (attempts++ == 60) {
      log_e("unable to connect to BLE Device - restarting");
      app.fast_restart();
      ;
    }
  }

  if (result) {
    auto initalscene = lr.get_current_scene(lr.client());
    if (initalscene == 0) {
      lr.sync_powerstate(false);
      // if (lr.state().scene == 0)
      //   lr.set_scene(0xFF,false);
    } else {
      lr.sync_powerstate(true);
      ;
      lr.sync_scene(initalscene);
    }
    lr.request_downlight_settings(lr.client());
    // returns immediatly - values will be set async in lr.client when we have a
    // BLE response
  }

#if defined(ROTARY_PIN_A)
  ESP_ERROR_CHECK(rotary.init(ROTARY_PIN_A, ROTARY_PIN_B, false));
  rotary.set_speedup_times(50, 25);
  rotary.on_rotary_event = [&](rotary_encoder_event_t event) {
    if (!get_powerstate())
      return;

    log_v("Rotary event %d  %d (%d) %ld ", event.state.direction,
          event.state.position, event.state.speed);
    int dimmerlevel = get_dimmer_value();
    int step = ROTARY_STEP_VALUE;
    if (event.state.direction == ROTARY_ENCODER_DIRECTION_CLOCKWISE) {
      step *= event.state.speed;
    } else if (event.state.direction ==
               ROTARY_ENCODER_DIRECTION_COUNTER_CLOCKWISE) {
      step *= -1 * event.state.speed;
    }
    if (dimmerlevel < 6)
      dimmerlevel = 6 - step;
    set_dimmer_value(dimmerlevel + step);

  };

#endif

#ifdef RELAY_PIN
  pinMode(RELAY_PIN, OUTPUT);
#endif

  button_handler::setup_buttons();

  log_d("Inital State: Power = %d scene %d", get_powerstate(), get_scene());

#ifndef LR_BLEADDRESS
  mqtt.queue("tele/" HOSTNAME "/BLEADDRESS", device_addr.toString().c_str());
#endif
#endif
  AppUtils::setupOta(
      []() {
        lr.client().disable_ble(true);
        mqtt.queue("tele/" HOSTNAME "/debug/ota", "starting");
      },
      [](unsigned int progress, unsigned int total) {
        static unsigned int lastvalue = 0xFFFF;
        auto complete = (progress) / (total / 100);
        log_i("Progress: %u%%\r", complete);
        if (complete - lastvalue > 9) {
          char msg[32];
          snprintf(msg, sizeof(msg), "progress=%u%%", complete);
          mqtt.queue("tele/" HOSTNAME "/debug/ota", msg);
          lastvalue = complete;
        }
      },
      [](const char *msg) {
        mqtt.queue("tele/" HOSTNAME "/debug/ota", msg);
        lr.client().disable_ble(false);
      });

  if (lr.state().power && lr.state().brightness < 5) {
    lr.set_dimmer(5);
  }
}

// Hack to inject mqtt debug messages
void debug(const char *msg, long val = 10) {
  /*
    char buffer[128];
    sniprintf(buffer,sizeof(buffer),"%s %ld",msg,val);
    mqtt.queue("tele/" HOSTNAME "/debug",buffer);
  */
}
void loop() {
  static unsigned long last_statemsg = 0;
  app_utils::AppUtils::loop();
  mqtt.loop();
  if (millis() - last_statemsg > 60000) {
    mqtt.queue("stat/" HOSTNAME "/RESULT", lr.create_state_message(), true);
    //   mqtt.queue("cmnd/" HOSTNAME "/mqttping", "ping", false);
    last_statemsg = millis();

    // usually the free heap is around 100k . If it is below 50k I must have a
    // memory leak somewhere. Reboot as a workaround
    if (ESP.getFreeHeap() < 50000) {
      log_e("POSSIBLE MEMORY LEAK detected. Free heap is %d k.  Rebooting",
            ESP.getFreeHeap() / 1024);
      app.fast_restart();
    }
  }
}