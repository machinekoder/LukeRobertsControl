#pragma once
//
// Based on
// https://github.com/h2zero/NimBLE-Arduino/tree/master/examples/NimBLE_Client
//
//
//
#include <queue>
#include <NimBLEDevice.h>

// The remote service we wish to connect to.
static BLEUUID serviceUUID("44092840-0567-11E6-B862-0002A5D5C51B");
// The characteristic of the remote service we are interested in.
static BLEUUID charUUID("44092842-0567-11E6-B862-0002A5D5C51B");
// static BLEUUID descUUID("00002902-0000-1000-8000-00805f9b34fb");
static BLEUUID charUUID_Scene("44092844-0567-11E6-B862-0002A5D5C51B");

// static BLEAddress device_addr_ = BLEAddress(LR_BLEADDRESS, 1);

class BleGattClient {

public:
  friend uint8_t get_current_scene(BleGattClient &client);

  using on_complete_callback = std::function<void()>;
  using on_downlight_callback =
      std::function<void(uint8_t brightness, uint16_t kelvin)>;

  static void init(BLEAddress device_addr);
  bool connect_to_server(on_complete_callback on_complete = nullptr);

  static notify_callback set_on_notify(notify_callback on_notify) {
    auto tmp = on_notify_;
    on_notify_ = on_notify;
    return tmp;
  }

  static void set_on_downlight(on_downlight_callback on_downlight) {
    on_downlight_notification_ = on_downlight;
  }

  bool connected() { return client->isConnected(); }

  void write(const uint8_t *data, size_t length) {
    log_i("WrittBLE");
    characteristic->writeValue(const_cast<uint8_t *>(data), length, true);
  }

  bool send(const uint8_t *data, size_t length) {
    log_i("BLE GATT SEND");
    int attempts = 5;
    while (connected_ != true && attempts-- > 0) {
      connected_ = connect_to_server();
    }
    if (connected_) {
      log_i("We are now connected to the BLE Server.");

      characteristic->writeValue(const_cast<uint8_t *>(data), length, true);
      return true;
    } else {
      log_e("Failed to connect to the ble server");
      NimBLEDevice::deleteClient(client);
      log_e("BLE Connect %s", "Failed to connect, deleted client");
      client = nullptr;
      return false;
    }
    yield();
  }

  struct BleCommand {
    uint8_t data[16];
    size_t size;
    bool is_dirty;
    std::function<void(int)> on_send;
  };

  static const int CACHE_SIZE_ = 4;
  // std::vector<BleData> cached_commands;
  std::array<BleCommand, CACHE_SIZE_> cached_commands;
  std::queue<BleCommand> pending_commands;
  uint8_t number_of_cached_commands_ = CACHE_SIZE_;

  void set_cache_size(uint8_t number_of_cached_commands) {
    // cached_commands.resize(number_of_cached_commands);
    number_of_cached_commands_ = number_of_cached_commands;
  }

  // ble_data *cached_commands;
  void set_cache_array(BleCommand cache[], size_t n_items) {
    for (auto i = 0; i < n_items; i++) {
      cached_commands[i] = cache[i];
    }
  }

  void queue_cmd(BleCommand &item, uint8_t index, bool is_dirty = true) {
    item.is_dirty = is_dirty;
    cached_commands[index] = item;
  }

  void queue_cmd(BleCommand &item) {
    item.is_dirty = true;
    pending_commands.push(item);
  }

  bool send_queued();
  void loop(on_complete_callback on_send = nullptr);

  static BLEAddress device_addr_;

private:
  NimBLERemoteService *service;
  NimBLERemoteCharacteristic *characteristic;
  NimBLERemoteDescriptor *remote_descriptor;
  NimBLEClient *client;
  static notify_callback on_notify_;
  static bool initialized_;
  volatile bool connected_;

  static on_complete_callback on_connect_;
  static on_complete_callback on_disconnect_;

  /**  None of these are required as they will be handled by the library with
   *defaults. **
   **                       Remove as you see fit for your needs */
  class ClientCallbacks : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient *pClient) {
      if (on_connect_) {
        on_connect_();
      }

      log_i("BLE Connected");
      /** After connection we should change the parameters if we don't need fast
       * response times.
       *  These settings are 150ms interval, 0 latency, 450ms timout.
       *  Timeout should be a multiple of the interval, minimum is 100ms.
       *  I find a multiple of 3-5 * the interval works best for quick
       * response/reconnect.
       *  Min interval: 120 * 1.25ms = 150, Max interval: 120 * 1.25ms = 150, 0
       * latency, 60 * 10ms = 600ms timeout
       */
      pClient->updateConnParams(120, 120, 0, 60);
      // gatt_client_-
      // connected_ = true;
    };

    void onDisconnect(NimBLEClient *pClient) {

      log_i("%s Disconnected", pClient->getPeerAddress().toString().c_str());
      // connected_ = false;
      //   NimBLEDevice::getScan()->start(scanTime, scanEndedCB);
      if (on_disconnect_) {
        on_disconnect_();
      }
    };

    /** Called when the peripheral requests a change to the connection
     * parameters.
     *  Return true to accept and apply them or false to reject and keep
     *  the currently used parameters. Default will return true.
     */
    bool onConnParamsUpdateRequest(NimBLEClient *pClient,
                                   const ble_gap_upd_params *params) {
      if (params->itvl_min < 12) { /** 1.25ms units */
        return false;
      } else if (params->itvl_max > 40) { /** 1.25ms units */
        return false;
      } else if (params->latency >
                 2) { /** Number of intervals allowed to skip */
        return false;
      } else if (params->supervision_timeout > 100) { /** 10ms units */
        return false;
      }
      return true;
    };

  public:
    void set_on_connect(BleGattClient &client, on_complete_callback notify) {
      client.on_connect_ = notify;
    }

    void set_on_disconnect(BleGattClient &client, on_complete_callback notify) {
      client.on_disconnect_ = notify;
    }

    /********************* Security handled here **********************
    ****** Note: these are the same return values as defaults ********/
    uint32_t onPassKeyRequest() {
      log_i("Client Passkey Request");
      /** return the passkey to send to the server */
      return 123456;
    };

    bool onConfirmPIN(uint32_t pass_key) {
      log_i("The passkey YES/NO number: pass key %lu", pass_key);
      /** Return false if passkeys don't match. */
      return true;
    };

    /** Pairing process complete, we can check the results in ble_gap_conn_desc
     */
    void onAuthenticationComplete(ble_gap_conn_desc *desc) {
      if (!desc->sec_state.encrypted) {
        log_i("Encrypt connection failed - disconnecting");
        /** Find the client with the connection handle provided in desc */
        NimBLEDevice::getClientByID(desc->conn_handle)->disconnect();
        return;
      }
    };
  };

  static on_downlight_callback on_downlight_notification_;

  /** Notification / Indication receiving handler callback */
  static void notifyCB(NimBLERemoteCharacteristic *pRemoteCharacteristic,
                       uint8_t *pData, size_t length, bool isNotify) {
    log_i(
        "%s from %s : Service = %s, Characteristic = %s, len=%d, Value=<%.*s> "
        "(%d)",
        (isNotify == true) ? "Notification" : "Indication",
        pRemoteCharacteristic->getRemoteService()
            ->getClient()
            ->getPeerAddress()
            .toString()
            .c_str(),
        pRemoteCharacteristic->getRemoteService()->getUUID().toString().c_str(),
        pRemoteCharacteristic->getUUID().toString().c_str(), length, length,
        (char *)pData, (int)pData[0]);

    log_d("Client data received len: %d\n", length);
    for (auto i = 0; i < length; i++) {
      log_d("Response byte[%d]: %d (0x%X)", i, pData[i], pData[i]);
    }

    if (on_downlight_notification_ != nullptr &&
        pRemoteCharacteristic->getUUID() == charUUID && length == 9 &&
        pData[0] == 0 && pData[1] == 0x88 && pData[2] == 0xF4 &&
        pData[3] == 0x18 && pData[4] == 0x71) {
      on_downlight_notification_(pData[7], pData[5] | pData[6] << 8);
    }
    if (on_notify_ != nullptr) {
      on_notify_(pRemoteCharacteristic, pData, length, isNotify);
    }
  }

  ClientCallbacks client_cb_;
};

#ifdef USE_SCENE_MAPPER
// Probably not neded anymore since we now have an API to request the current
// values for brightness and color temperature

// Helper call. Reads/Writes  bightnesses for scenes from SPIFFS storage

template <typename T> class SPIFFSHelper {
public:
  File file_;
  File open(const char *filename, const char *mode) {
    if (!SPIFFS.begin(true)) {
      log_e("An Error has occurred while mounting SPIFFS");
      return File();
    }
    file_ = SPIFFS.open(filename, mode);
    return file_;
  }

  void close() {
    file_.close();
    SPIFFS.end();
  }
  SPIFFSHelper() {}

  ~SPIFFSHelper() { close(); }
  size_t read(File &file, T &element) {
    return file.read((uint8_t *)&element, sizeof(T));
  }
  size_t read(T &element) { return read(file_, element); }

  size_t write(File &file, const T &element) {
    return file.write((const uint8_t *)&element, sizeof(T));
  }
  size_t write(const T &element) { return write(file_, element); }

  bool write(const char *filename, T &element) {
    if (!SPIFFS.begin(true)) {
      log_e("An Error has occurred while mounting SPIFFS");
      return false;
    }
    File mapfile = SPIFFS.open(filename, "w+");
    if (mapfile) {
      write(mapfile, &element);
      mapfile.close();
      SPIFFS.end();
      return true;
    }
    return false;
  }

  bool read(const char *filename, T &element) {
    if (!SPIFFS.begin(true)) {
      log_e("An Error has occurred while mounting SPIFFS");
      return false;
    }
    File mapfile = SPIFFS.open(filename, "r");
    if (mapfile) {
      read(mapfile, &element);
      mapfile.close();
      SPIFFS.end();
      return true;
    }
    return false;
  }
};

class SceneMapper {
public:
  SceneMapper() {}
  static int save() {

    SPIFFSHelper<uint16_t> spiff;
    if (spiff.open("/scenemap.bin", "w+")) {
      uint16_t number_of_elements = brightness_map_.size();
      number_of_elements = colortemperature_map_.size();
      spiff.write(number_of_elements);
      for (auto i : brightness_map_) {
        spiff.write(i);
      }
      for (auto i : colortemperature_map_) {
        spiff.write(i);
      }

      spiff.close();
      return number_of_elements;
    } else {
      log_e("Can't create brightness mapfile");
    }
    return 0;
  }
  static int load() {

    SPIFFSHelper<uint16_t> spiff;
    File mapfile = spiff.open("/scenemap.bin", "r");
    if (!mapfile) {
      log_w("Failed to open file for reading");
      return save();
    }

    uint16_t number_of_brightness_elements = 0;
    uint16_t number_of_colortemperature_elements = 0;
    if (mapfile) {
      brightness_map_.clear();
      colortemperature_map_.clear();
      spiff.read(number_of_brightness_elements);
      spiff.read(number_of_colortemperature_elements);
      uint16_t item = 0;
      for (int i = 0; i < number_of_brightness_elements; i++) {
        spiff.read(item);
        brightness_map_.push_back(item);
      }
      item = 0;
      for (int i = 0; i < number_of_colortemperature_elements; i++) {
        spiff.read(item);
        colortemperature_map_.push_back(item);
      }

      spiff.close();
    }
    return number_of_brightness_elements;
  }

  static int size() { return brightness_map_.size(); }

  static int map_brightness(int scene) {
    if (scene >= 0 && scene < brightness_map_.size()) {
      return brightness_map_[scene];
    }
    // just add a default value ;
    return 50;
  }

  static int map_colortemperature(int scene) {
    if (scene >= 0 && scene < colortemperature_map_.size()) {
      return colortemperature_map_[scene];
    }
    // just add a default value ;
    return 50;
  }

  static bool set_brightness(int scene, int brightness_level) {
    if (scene >= 0 && scene < brightness_map_.size()) {
      brightness_map_[scene] = brightness_level;
      save();
      return true;
    } else if (scene == brightness_map_.size()) {
      brightness_map_.push_back(brightness_level);
      save();
      return true;
    }
    return false;
  }

  static bool set_colortemperature(int scene, int kelvin) {
    if (scene >= 0 && scene < brightness_map_.size()) {
      colortemperature_map_[scene] = kelvin;
      save();
      return true;
    } else if (scene == colortemperature_map_.size()) {
      colortemperature_map_.push_back(kelvin);
      save();
      return true;
    }
    return false;
  }

private:
  static std::vector<int> brightness_map_;
  static std::vector<int> colortemperature_map_;
};
#endif

void get_all_scenes(BleGattClient &client);
uint8_t get_current_scene(BleGattClient &client);
void request_downlight_settings(BleGattClient &client);
NimBLEAddress scan_for_device();