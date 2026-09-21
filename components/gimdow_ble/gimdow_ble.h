#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/log.h"

#include "esphome/components/lock/lock.h"
#include "esphome/components/ble_client/ble_client.h"
#include "esphome/components/binary_sensor/binary_sensor.h"

#include <esp_gatt_common_api.h>
#include <esp_gattc_api.h>
#include <esp_system.h>

#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/md5.h>
#include <mbedtls/version.h>


namespace esphome {
namespace gimdow_ble {

namespace espbt = esphome::esp32_ble_tracker;


static const char *const TAG = "gimdow_ble";


// -----------------------------------------------------------------------------
// Tuya BLE codes
// -----------------------------------------------------------------------------

static constexpr uint16_t FUN_SENDER_DEVICE_INFO = 0x0000;
static constexpr uint16_t FUN_SENDER_PAIR        = 0x0001;
static constexpr uint16_t FUN_SENDER_DPS         = 0x0002;
static constexpr uint16_t FUN_SENDER_DPS_V4      = 0x0027;

static constexpr uint16_t FUN_RECEIVE_DP         = 0x8001;
static constexpr uint16_t FUN_RECEIVE_TIME_DP    = 0x8003;
static constexpr uint16_t FUN_RECEIVE_DP_V4      = 0x8006;
static constexpr uint16_t FUN_RECEIVE_TIME_DP_V4 = 0x8007;
static constexpr uint16_t FUN_RECEIVE_TIME1_REQ  = 0x8011;
static constexpr uint16_t FUN_RECEIVE_TIME2_REQ  = 0x8012;


// -----------------------------------------------------------------------------
// Supported lock models
// -----------------------------------------------------------------------------

enum class GimdowModel : uint8_t {
  A1_PRO_MAX = 0,
  A1_ULTRA = 1,
};


// -----------------------------------------------------------------------------
// Gimdow A1 PRO MAX / rlyxv7pe / Tuya BLE v3
// -----------------------------------------------------------------------------

static constexpr uint16_t V3_SERVICE_UUID = 0x1910;
static constexpr uint16_t V3_NOTIFY_UUID  = 0x2B10;
static constexpr uint16_t V3_WRITE_UUID   = 0x2B11;

static constexpr uint8_t V3_PROTOCOL_VERSION = 3;
static constexpr size_t V3_GATT_PACKET_SIZE = 20;


// -----------------------------------------------------------------------------
// Gimdow A1 Ultra / hc7n0urm / TuyaOS FD50
// -----------------------------------------------------------------------------

static constexpr char ULTRA_SERVICE_UUID[] =
    "0000fd50-0000-1000-8000-00805f9b34fb";
static constexpr char ULTRA_NOTIFY_UUID[] =
    "00000002-0000-1001-8001-00805f9b07d0";
static constexpr char ULTRA_WRITE_UUID[] =
    "00000001-0000-1001-8001-00805f9b07d0";

static constexpr uint16_t ULTRA_LOCAL_MTU = 247;
static constexpr size_t ULTRA_DEVICE_INFO_PACKET_SIZE = 244;


// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

inline void put_u16_be(std::vector<uint8_t> &out, uint16_t value) {
  out.push_back((value >> 8) & 0xFF);
  out.push_back(value & 0xFF);
}

inline void put_u32_be(std::vector<uint8_t> &out, uint32_t value) {
  out.push_back((value >> 24) & 0xFF);
  out.push_back((value >> 16) & 0xFF);
  out.push_back((value >> 8) & 0xFF);
  out.push_back(value & 0xFF);
}

inline uint16_t get_u16_be(const uint8_t *p) {
  return (static_cast<uint16_t>(p[0]) << 8) |
         static_cast<uint16_t>(p[1]);
}

inline uint32_t get_u32_be(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) |
         static_cast<uint32_t>(p[3]);
}


class GimdowBLELock :
    public lock::Lock,
    public ble_client::BLEClientNode,
    public Component {
 public:

  // ---------------------------------------------------------------------------
  // Configuration
  // ---------------------------------------------------------------------------

  void set_parent(ble_client::BLEClient *parent) {
    this->ble_parent_ = parent;
  }

  void set_model(uint8_t value) {
    this->model_ = static_cast<GimdowModel>(value);
  }

  void set_local_key(const std::string &value) {
    this->local_key_string_ = value;
  }

  void set_uuid(const std::string &value) {
    this->uuid_ = value;
  }

  void set_device_id(const std::string &value) {
    this->device_id_ = value;
  }

  void set_ble_unlock_check(const std::string &value) {
    this->ble_unlock_check_ = value;
  }

  void set_state_sensor(binary_sensor::BinarySensor *sensor) {
    this->state_sensor_ = sensor;

    sensor->add_on_state_callback([this](bool state) {
      this->external_state_initialized_ = true;

      ESP_LOGD(
          TAG,
          "External lock state sensor: %s",
          state ? "UNLOCKED" : "LOCKED"
      );

      this->publish_state(
          state
              ? lock::LOCK_STATE_UNLOCKED
              : lock::LOCK_STATE_LOCKED
      );
    });
  }


  // ---------------------------------------------------------------------------
  // ESPHome
  // ---------------------------------------------------------------------------

  void setup() override {
    ESP_LOGI(
        TAG,
        "Initialising Gimdow BLE lock (%s)",
        this->model_name_()
    );

    this->traits.set_assumed_state(true);

    if (this->local_key_string_.size() < 6) {
      ESP_LOGE(TAG, "local_key is too short");
      this->mark_failed();
      return;
    }

    if (
        this->model_ == GimdowModel::A1_ULTRA &&
        this->ble_unlock_check_.empty()
    ) {
      ESP_LOGE(TAG, "a1_ultra requires ble_unlock_check");
      this->mark_failed();
      return;
    }

    // Tuya lock profiles used here derive the BLE login/session keys from
    // the first six bytes of local_key.
    memcpy(
        this->local_key_.data(),
        this->local_key_string_.data(),
        6
    );

    this->md5_(
        this->local_key_.data(),
        this->local_key_.size(),
        this->login_key_.data()
    );

    if (this->model_ == GimdowModel::A1_ULTRA) {
      // FD50 DEVICE_INFO must fit in one ATT write. ESPHome/ESP-IDF will
      // negotiate this preferred local MTU when the connection is created.
      esp_err_t err = esp_ble_gatt_set_local_mtu(ULTRA_LOCAL_MTU);
      if (err != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Unable to set preferred BLE MTU %u: %d",
            ULTRA_LOCAL_MTU,
            err
        );
      }
    }

    ESP_LOGI(TAG, "Tuya login key prepared");
  }


  void dump_config() override {
    ESP_LOGCONFIG(TAG, "Gimdow BLE:");
    ESP_LOGCONFIG(TAG, "  Model: %s", this->model_name_());
    ESP_LOGCONFIG(TAG, "  UUID: %s", this->uuid_.c_str());
    ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
    ESP_LOGCONFIG(
        TAG,
        "  Protocol: %s",
        this->model_ == GimdowModel::A1_ULTRA
            ? "TuyaOS FD50 / V4"
            : "Tuya BLE v3"
    );
  }


  void loop() override {
    // On startup, immediately publish the current external sensor state
    // if it already has a valid value.
    if (
        this->state_sensor_ != nullptr &&
        !this->external_state_initialized_ &&
        this->state_sensor_->has_state()
    ) {
      this->external_state_initialized_ = true;

      bool state = this->state_sensor_->state;

      ESP_LOGD(
          TAG,
          "Initial external lock state: %s",
          state ? "UNLOCKED" : "LOCKED"
      );

      this->publish_state(
          state
              ? lock::LOCK_STATE_UNLOCKED
              : lock::LOCK_STATE_LOCKED
      );
    }

    // Send one BLE fragment at a time.
    if (this->tx_index_ >= this->tx_packets_.size())
      return;

    uint32_t now = millis();

    if (now - this->last_tx_ms_ < 30)
      return;

    this->last_tx_ms_ = now;

    auto &packet = this->tx_packets_[this->tx_index_];

    if (this->write_handle_ == 0) {
      ESP_LOGE(TAG, "Write characteristic is not available");
      this->tx_packets_.clear();
      this->tx_index_ = 0;
      return;
    }

    ESP_LOGD(
        TAG,
        "TX fragment %u/%u, %u bytes",
        static_cast<unsigned>(this->tx_index_ + 1),
        static_cast<unsigned>(this->tx_packets_.size()),
        static_cast<unsigned>(packet.size())
    );

    esp_err_t err = esp_ble_gattc_write_char(
        this->parent()->get_gattc_if(),
        this->parent()->get_conn_id(),
        this->write_handle_,
        packet.size(),
        packet.data(),
        ESP_GATT_WRITE_TYPE_NO_RSP,
        ESP_GATT_AUTH_REQ_NONE
    );

    if (err != ESP_OK) {
      ESP_LOGE(TAG, "BLE write failed: %d", err);
      return;
    }

    this->tx_index_++;

    if (this->tx_index_ >= this->tx_packets_.size()) {
      this->tx_packets_.clear();
      this->tx_index_ = 0;
    }
  }


 protected:

  // ---------------------------------------------------------------------------
  // Home Assistant lock commands
  // ---------------------------------------------------------------------------

  void control(const lock::LockCall &call) override {
    auto state = call.get_state();

    if (!state.has_value())
      return;

    if (*state == lock::LOCK_STATE_UNLOCKED) {
      ESP_LOGI(TAG, "Requested UNLOCK via BLE");
      this->pending_dp_ = 6;
      this->pending_lock_state_ = lock::LOCK_STATE_UNLOCKED;
    } else if (*state == lock::LOCK_STATE_LOCKED) {
      ESP_LOGI(TAG, "Requested LOCK via BLE");
      this->pending_dp_ = 46;
      this->pending_lock_state_ = lock::LOCK_STATE_LOCKED;
    } else {
      return;
    }

    this->command_pending_ = true;

    if (this->paired_ && this->session_key_valid_) {
      this->send_pending_command_();
      return;
    }

    if (this->ble_parent_ != nullptr) {
      ESP_LOGD(TAG, "Connecting to Gimdow...");
      this->ble_parent_->connect();
    }
  }


 public:

  // ---------------------------------------------------------------------------
  // BLE events
  // ---------------------------------------------------------------------------

  void gattc_event_handler(
      esp_gattc_cb_event_t event,
      esp_gatt_if_t gattc_if,
      esp_ble_gattc_cb_param_t *param
  ) override {

    switch (event) {

      case ESP_GATTC_CFG_MTU_EVT: {
        if (param->cfg_mtu.status == ESP_GATT_OK) {
          this->negotiated_mtu_ = param->cfg_mtu.mtu;
          ESP_LOGI(
              TAG,
              "BLE MTU negotiated: %u",
              this->negotiated_mtu_
          );
          this->try_start_handshake_();
        }
        break;
      }


      // -----------------------------------------------------------------------
      // GATT database discovered
      // -----------------------------------------------------------------------

      case ESP_GATTC_SEARCH_CMPL_EVT: {
        ESP_LOGI(TAG, "GATT discovery complete");

        ble_client::BLECharacteristic *notify_chr = nullptr;
        ble_client::BLECharacteristic *write_chr = nullptr;

        if (this->model_ == GimdowModel::A1_ULTRA) {
          auto service_uuid = espbt::ESPBTUUID::from_raw(
              ULTRA_SERVICE_UUID,
              strlen(ULTRA_SERVICE_UUID)
          );
          auto notify_uuid = espbt::ESPBTUUID::from_raw(
              ULTRA_NOTIFY_UUID,
              strlen(ULTRA_NOTIFY_UUID)
          );
          auto write_uuid = espbt::ESPBTUUID::from_raw(
              ULTRA_WRITE_UUID,
              strlen(ULTRA_WRITE_UUID)
          );

          notify_chr = this->parent()->get_characteristic(
              service_uuid,
              notify_uuid
          );

          write_chr = this->parent()->get_characteristic(
              service_uuid,
              write_uuid
          );
        } else {
          notify_chr = this->parent()->get_characteristic(
              V3_SERVICE_UUID,
              V3_NOTIFY_UUID
          );

          write_chr = this->parent()->get_characteristic(
              V3_SERVICE_UUID,
              V3_WRITE_UUID
          );
        }

        if (notify_chr == nullptr) {
          ESP_LOGE(
              TAG,
              "%s notify characteristic not found",
              this->model_name_()
          );
          return;
        }

        if (write_chr == nullptr) {
          ESP_LOGE(
              TAG,
              "%s write characteristic not found",
              this->model_name_()
          );
          return;
        }

        this->notify_handle_ = notify_chr->handle;
        this->write_handle_ = write_chr->handle;

        ESP_LOGI(
            TAG,
            "Tuya characteristics found: notify=0x%04X write=0x%04X",
            this->notify_handle_,
            this->write_handle_
        );

        auto err = this->parent()->register_for_notify(
            this->notify_handle_
        );

        if (err != ESP_OK) {
          ESP_LOGE(TAG, "register_for_notify failed: %d", err);
        }

        break;
      }


      // -----------------------------------------------------------------------
      // Notification registration finished
      // -----------------------------------------------------------------------

      case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
        if (param->reg_for_notify.handle != this->notify_handle_)
          break;

        if (param->reg_for_notify.status != ESP_GATT_OK) {
          ESP_LOGE(
              TAG,
              "Notification registration failed: %d",
              param->reg_for_notify.status
          );
          return;
        }

        ESP_LOGI(TAG, "Notifications enabled");

        this->node_state =
            esp32_ble_tracker::ClientState::ESTABLISHED;

        this->notifications_ready_ = true;

        this->reset_protocol_();
        this->try_start_handshake_();

        break;
      }


      // -----------------------------------------------------------------------
      // Tuya notification
      // -----------------------------------------------------------------------

      case ESP_GATTC_NOTIFY_EVT: {
        if (param->notify.handle != this->notify_handle_)
          break;

        if (param->notify.value_len == 0)
          break;

        this->handle_notification_(
            param->notify.value,
            param->notify.value_len
        );

        break;
      }


      // -----------------------------------------------------------------------
      // Disconnected
      // -----------------------------------------------------------------------

      case ESP_GATTC_DISCONNECT_EVT:
      case ESP_GATTC_CLOSE_EVT: {
        ESP_LOGD(TAG, "Gimdow disconnected");

        this->paired_ = false;
        this->session_key_valid_ = false;
        this->notifications_ready_ = false;
        this->device_info_sent_ = false;
        this->mtu_requested_ = false;
        this->negotiated_mtu_ = 23;

        this->notify_handle_ = 0;
        this->write_handle_ = 0;

        this->rx_buffer_.clear();
        this->rx_expected_packet_ = 0;
        this->rx_expected_length_ = 0;

        this->tx_packets_.clear();
        this->tx_index_ = 0;

        break;
      }

      default:
        break;
    }
  }


 private:

  // ---------------------------------------------------------------------------
  // Model helpers
  // ---------------------------------------------------------------------------

  const char *model_name_() const {
    switch (this->model_) {
      case GimdowModel::A1_ULTRA:
        return "A1 Ultra";
      case GimdowModel::A1_PRO_MAX:
      default:
        return "A1 Pro Max";
    }
  }


  void try_start_handshake_() {
    if (!this->notifications_ready_ || this->device_info_sent_)
      return;

    if (
        this->model_ == GimdowModel::A1_ULTRA &&
        this->negotiated_mtu_ < 40
    ) {
      ESP_LOGI(
          TAG,
          "Waiting for larger MTU before FD50 DEVICE_INFO (current %u)",
          this->negotiated_mtu_
      );

      if (!this->mtu_requested_) {
        this->mtu_requested_ = true;

        esp_err_t err = esp_ble_gattc_send_mtu_req(
            this->parent()->get_gattc_if(),
            this->parent()->get_conn_id()
        );

        if (err != ESP_OK) {
          ESP_LOGW(TAG, "MTU request failed: %d", err);
        }
      }

      return;
    }

    std::vector<uint8_t> payload;

    if (this->model_ == GimdowModel::A1_ULTRA) {
      payload = {0x00, 0xF3};
    }

    this->device_info_sent_ = true;

    ESP_LOGI(TAG, "Sending DEVICE_INFO");

    this->send_packet_(
        FUN_SENDER_DEVICE_INFO,
        payload,
        0
    );
  }


  // ---------------------------------------------------------------------------
  // MD5
  // ---------------------------------------------------------------------------

  static bool md5_(
      const uint8_t *data,
      size_t len,
      uint8_t out[16]
  ) {
#if MBEDTLS_VERSION_NUMBER >= 0x03000000
    return mbedtls_md5(data, len, out) == 0;
#else
    return mbedtls_md5_ret(data, len, out) == 0;
#endif
  }


  // ---------------------------------------------------------------------------
  // CRC16
  // ---------------------------------------------------------------------------

  static uint16_t crc16_(
      const uint8_t *data,
      size_t len
  ) {
    uint16_t crc = 0xFFFF;

    for (size_t i = 0; i < len; i++) {
      crc ^= data[i];

      for (uint8_t bit = 0; bit < 8; bit++) {
        bool lsb = crc & 1;
        crc >>= 1;

        if (lsb)
          crc ^= 0xA001;
      }
    }

    return crc;
  }


  // ---------------------------------------------------------------------------
  // Tuya variable integer
  // ---------------------------------------------------------------------------

  static std::vector<uint8_t> pack_varint_(uint32_t value) {
    std::vector<uint8_t> out;

    while (true) {
      uint8_t current = value & 0x7F;
      value >>= 7;

      if (value != 0)
        current |= 0x80;

      out.push_back(current);

      if (value == 0)
        break;
    }

    return out;
  }


  static bool unpack_varint_(
      const uint8_t *data,
      size_t len,
      size_t &pos,
      uint32_t &value
  ) {
    value = 0;
    uint8_t shift = 0;

    for (uint8_t i = 0; i < 5; i++) {
      if (pos >= len)
        return false;

      uint8_t current = data[pos++];

      value |=
          static_cast<uint32_t>(current & 0x7F)
          << shift;

      if ((current & 0x80) == 0)
        return true;

      shift += 7;
    }

    return false;
  }


  // ---------------------------------------------------------------------------
  // AES CBC
  // ---------------------------------------------------------------------------

  static bool aes_encrypt_(
      const uint8_t key[16],
      const uint8_t iv_in[16],
      const std::vector<uint8_t> &plain,
      std::vector<uint8_t> &encrypted
  ) {
    if ((plain.size() % 16) != 0)
      return false;

    encrypted.resize(plain.size());

    uint8_t iv[16];
    memcpy(iv, iv_in, 16);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);

    if (mbedtls_aes_setkey_enc(&aes, key, 128) != 0) {
      mbedtls_aes_free(&aes);
      return false;
    }

    int ret = mbedtls_aes_crypt_cbc(
        &aes,
        MBEDTLS_AES_ENCRYPT,
        plain.size(),
        iv,
        plain.data(),
        encrypted.data()
    );

    mbedtls_aes_free(&aes);

    return ret == 0;
  }


  static bool aes_decrypt_(
      const uint8_t key[16],
      const uint8_t iv_in[16],
      const uint8_t *encrypted,
      size_t encrypted_len,
      std::vector<uint8_t> &plain
  ) {
    if ((encrypted_len % 16) != 0)
      return false;

    plain.resize(encrypted_len);

    uint8_t iv[16];
    memcpy(iv, iv_in, 16);

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);

    if (mbedtls_aes_setkey_dec(&aes, key, 128) != 0) {
      mbedtls_aes_free(&aes);
      return false;
    }

    int ret = mbedtls_aes_crypt_cbc(
        &aes,
        MBEDTLS_AES_DECRYPT,
        plain.size(),
        iv,
        encrypted,
        plain.data()
    );

    mbedtls_aes_free(&aes);

    return ret == 0;
  }


  // ---------------------------------------------------------------------------
  // Build and queue Tuya packet
  // ---------------------------------------------------------------------------

  void send_packet_(
      uint16_t code,
      const std::vector<uint8_t> &data,
      uint32_t response_to
  ) {
    const uint8_t *key = nullptr;
    uint8_t security_flag = 0;

    if (code == FUN_SENDER_DEVICE_INFO) {
      key = this->login_key_.data();
      security_flag = 0x04;
    } else {
      if (!this->session_key_valid_) {
        ESP_LOGE(TAG, "Session key not available");
        return;
      }

      key = this->session_key_.data();
      security_flag = 0x05;
    }

    uint32_t seq = this->sequence_++;

    std::vector<uint8_t> raw;

    put_u32_be(raw, seq);
    put_u32_be(raw, response_to);
    put_u16_be(raw, code);
    put_u16_be(raw, data.size());

    raw.insert(
        raw.end(),
        data.begin(),
        data.end()
    );

    uint16_t crc = crc16_(
        raw.data(),
        raw.size()
    );

    put_u16_be(raw, crc);

    while ((raw.size() % 16) != 0)
      raw.push_back(0x00);


    uint8_t iv[16];
    esp_fill_random(iv, sizeof(iv));

    std::vector<uint8_t> encrypted_payload;

    if (!aes_encrypt_(
        key,
        iv,
        raw,
        encrypted_payload
    )) {
      ESP_LOGE(TAG, "AES encryption failed");
      return;
    }


    std::vector<uint8_t> encrypted;

    encrypted.push_back(security_flag);

    encrypted.insert(
        encrypted.end(),
        iv,
        iv + sizeof(iv)
    );

    encrypted.insert(
        encrypted.end(),
        encrypted_payload.begin(),
        encrypted_payload.end()
    );


    std::vector<std::vector<uint8_t>> packets;

    size_t pos = 0;
    uint32_t packet_num = 0;

    while (pos < encrypted.size()) {
      std::vector<uint8_t> packet;

      auto pn = pack_varint_(packet_num);
      packet.insert(packet.end(), pn.begin(), pn.end());

      if (packet_num == 0) {
        auto total = pack_varint_(encrypted.size());

        packet.insert(
            packet.end(),
            total.begin(),
            total.end()
        );

        uint8_t packet_protocol_version = this->protocol_version_;

        if (
            this->model_ == GimdowModel::A1_ULTRA &&
            code == FUN_SENDER_DEVICE_INFO
        ) {
          packet_protocol_version = 2;
        }

        packet.push_back(
            packet_protocol_version << 4
        );
      }

      size_t packet_size = V3_GATT_PACKET_SIZE;

      if (
          this->model_ == GimdowModel::A1_ULTRA &&
          code == FUN_SENDER_DEVICE_INFO
      ) {
        packet_size = ULTRA_DEVICE_INFO_PACKET_SIZE;
      }

      size_t available =
          packet_size > packet.size()
              ? packet_size - packet.size()
              : 0;

      if (available == 0) {
        ESP_LOGE(TAG, "Invalid Tuya packet size");
        return;
      }

      size_t count =
          std::min(
              available,
              encrypted.size() - pos
          );

      packet.insert(
          packet.end(),
          encrypted.begin() + pos,
          encrypted.begin() + pos + count
      );

      pos += count;

      packets.push_back(packet);

      packet_num++;
    }

    ESP_LOGD(
        TAG,
        "Queue Tuya command 0x%04X seq=%u fragments=%u",
        code,
        static_cast<unsigned>(seq),
        static_cast<unsigned>(packets.size())
    );

    this->tx_packets_ = packets;
    this->tx_index_ = 0;
    this->last_tx_ms_ = 0;
  }


  // ---------------------------------------------------------------------------
  // Incoming Tuya BLE fragments
  // ---------------------------------------------------------------------------

  void handle_notification_(
      const uint8_t *data,
      size_t len
  ) {
    size_t pos = 0;

    uint32_t packet_num;

    if (!unpack_varint_(
        data,
        len,
        pos,
        packet_num
    ))
      return;


    if (packet_num < this->rx_expected_packet_) {
      this->reset_rx_();
    }


    if (packet_num != this->rx_expected_packet_) {
      ESP_LOGW(
          TAG,
          "Unexpected Tuya fragment %u, expected %u",
          static_cast<unsigned>(packet_num),
          static_cast<unsigned>(this->rx_expected_packet_)
      );

      this->reset_rx_();
      return;
    }


    if (packet_num == 0) {
      uint32_t expected_length;

      if (!unpack_varint_(
          data,
          len,
          pos,
          expected_length
      ))
        return;

      this->rx_expected_length_ =
          expected_length;

      this->rx_buffer_.clear();

      if (pos >= len)
        return;

      pos++;
    }


    this->rx_buffer_.insert(
        this->rx_buffer_.end(),
        data + pos,
        data + len
    );

    this->rx_expected_packet_++;


    if (
        this->rx_buffer_.size() ==
        this->rx_expected_length_
    ) {
      this->parse_message_();
      this->reset_rx_();
    } else if (
        this->rx_buffer_.size() >
        this->rx_expected_length_
    ) {
      ESP_LOGW(TAG, "Tuya RX packet too long");
      this->reset_rx_();
    }
  }


  void reset_rx_() {
    this->rx_buffer_.clear();
    this->rx_expected_packet_ = 0;
    this->rx_expected_length_ = 0;
  }


  // ---------------------------------------------------------------------------
  // Decrypt complete Tuya message
  // ---------------------------------------------------------------------------

  void parse_message_() {
    if (this->rx_buffer_.size() < 17)
      return;

    uint8_t security_flag =
        this->rx_buffer_[0];

    const uint8_t *key = nullptr;

    if (security_flag == 0x04) {
      key = this->login_key_.data();
    } else if (security_flag == 0x05) {
      if (!this->session_key_valid_) {
        ESP_LOGW(TAG, "RX uses session key before session exists");
        return;
      }

      key = this->session_key_.data();
    } else {
      ESP_LOGW(
          TAG,
          "Unknown security flag: 0x%02X",
          security_flag
      );
      return;
    }


    const uint8_t *iv =
        this->rx_buffer_.data() + 1;

    const uint8_t *encrypted =
        this->rx_buffer_.data() + 17;

    size_t encrypted_len =
        this->rx_buffer_.size() - 17;


    std::vector<uint8_t> raw;

    if (!aes_decrypt_(
        key,
        iv,
        encrypted,
        encrypted_len,
        raw
    )) {
      ESP_LOGE(TAG, "AES decrypt failed");
      return;
    }


    if (raw.size() < 14)
      return;


    uint32_t seq_num =
        get_u32_be(raw.data());

    uint32_t response_to =
        get_u32_be(raw.data() + 4);

    uint16_t code =
        get_u16_be(raw.data() + 8);

    uint16_t data_length =
        get_u16_be(raw.data() + 10);


    if (12 + data_length > raw.size()) {
      ESP_LOGE(TAG, "Invalid Tuya payload length");
      return;
    }


    const uint8_t *payload =
        raw.data() + 12;


    ESP_LOGD(
        TAG,
        "RX Tuya command 0x%04X seq=%u response_to=%u len=%u",
        code,
        static_cast<unsigned>(seq_num),
        static_cast<unsigned>(response_to),
        data_length
    );


    this->handle_tuya_message_(
        seq_num,
        response_to,
        code,
        payload,
        data_length
    );
  }


  // ---------------------------------------------------------------------------
  // Tuya state machine
  // ---------------------------------------------------------------------------

  void handle_tuya_message_(
      uint32_t seq_num,
      uint32_t response_to,
      uint16_t code,
      const uint8_t *data,
      size_t len
  ) {

    if (code == FUN_SENDER_DEVICE_INFO) {
      if (len < 46) {
        ESP_LOGE(TAG, "DEVICE_INFO response is too short");
        return;
      }

      this->protocol_version_ = data[2];

      ESP_LOGI(
          TAG,
          "DEVICE_INFO received, protocol=%u.%u",
          data[2],
          data[3]
      );


      uint8_t source[12];

      memcpy(
          source,
          this->local_key_.data(),
          6
      );

      memcpy(
          source + 6,
          data + 6,
          6
      );

      if (!md5_(
          source,
          sizeof(source),
          this->session_key_.data()
      )) {
        ESP_LOGE(TAG, "Unable to calculate session key");
        return;
      }

      this->session_key_valid_ = true;

      ESP_LOGI(TAG, "Session key established");

      this->send_pair_();
      return;
    }


    if (code == FUN_SENDER_PAIR) {
      if (len != 1) {
        ESP_LOGE(TAG, "Invalid PAIR response");
        return;
      }

      uint8_t result = data[0];

      if (result == 0 || result == 2) {
        this->paired_ = true;

        ESP_LOGI(
            TAG,
            result == 2
                ? "Gimdow already paired"
                : "Gimdow pairing OK"
        );

        if (this->command_pending_) {
          this->send_pending_command_();
        }
      } else {
        ESP_LOGE(
            TAG,
            "Gimdow PAIR error: %u",
            result
        );
      }

      return;
    }


    if (code == FUN_RECEIVE_DP) {
      this->parse_datapoints_v3_(
          data,
          len
      );
      return;
    }


    if (
        code == FUN_RECEIVE_DP_V4 ||
        code == FUN_RECEIVE_TIME_DP_V4
    ) {
      this->parse_datapoints_v4_(
          data,
          len
      );

      // Tuya V4 async datapoint events expect an empty response using
      // the same message code and response_to=received sequence number.
      this->send_packet_(
          code,
          {},
          seq_num
      );
      return;
    }


    if (code == FUN_RECEIVE_TIME_DP) {
      return;
    }


    if (
        code == FUN_RECEIVE_TIME1_REQ ||
        code == FUN_RECEIVE_TIME2_REQ
    ) {
      ESP_LOGD(
          TAG,
          "Time request received (ignored)"
      );
      return;
    }
  }


  // ---------------------------------------------------------------------------
  // Pairing payload
  // ---------------------------------------------------------------------------

  void send_pair_() {
    std::vector<uint8_t> payload;

    payload.insert(
        payload.end(),
        this->uuid_.begin(),
        this->uuid_.end()
    );

    payload.insert(
        payload.end(),
        this->local_key_.begin(),
        this->local_key_.end()
    );

    payload.insert(
        payload.end(),
        this->device_id_.begin(),
        this->device_id_.end()
    );

    while (payload.size() < 44)
      payload.push_back(0x00);

    ESP_LOGI(TAG, "Sending PAIR");

    this->send_packet_(
        FUN_SENDER_PAIR,
        payload,
        0
    );
  }


  // ---------------------------------------------------------------------------
  // Ultra unlock helper
  // ---------------------------------------------------------------------------

  bool build_ultra_unlock_payload_(std::vector<uint8_t> &payload) {
    if (this->ble_unlock_check_.empty()) {
      ESP_LOGE(TAG, "ble_unlock_check is missing");
      return false;
    }

    size_t decoded_capacity =
        (this->ble_unlock_check_.size() * 3) / 4 + 4;

    std::vector<uint8_t> decoded(decoded_capacity);
    size_t decoded_len = 0;

    int ret = mbedtls_base64_decode(
        decoded.data(),
        decoded.size(),
        &decoded_len,
        reinterpret_cast<const unsigned char *>(
            this->ble_unlock_check_.data()
        ),
        this->ble_unlock_check_.size()
    );

    if (ret != 0 || decoded_len < 19) {
      ESP_LOGE(
          TAG,
          "Invalid ble_unlock_check (base64 decode=%d len=%u)",
          ret,
          static_cast<unsigned>(decoded_len)
      );
      return false;
    }

    payload = {
        0x00, 0x00, 0x00, 0x00,
        0x01,
        0x47,
        0x00, 0x00, 0x13
    };

    // prefix = check[2:4]
    payload.push_back(decoded[2]);
    payload.push_back(decoded[3]);

    payload.push_back(0x00);
    payload.push_back(0x01);

    // check_code = check[4:12]
    payload.insert(
        payload.end(),
        decoded.begin() + 4,
        decoded.begin() + 12
    );

    payload.push_back(0x01);

    // check_key = check[13:17]
    payload.insert(
        payload.end(),
        decoded.begin() + 13,
        decoded.begin() + 17
    );

    payload.push_back(0x00);
    payload.push_back(0x01);

    return true;
  }


  // ---------------------------------------------------------------------------
  // Lock / unlock command
  // ---------------------------------------------------------------------------

  void send_pending_command_() {
    if (!this->command_pending_)
      return;

    if (this->model_ == GimdowModel::A1_ULTRA) {
      std::vector<uint8_t> payload;

      if (this->pending_lock_state_ == lock::LOCK_STATE_UNLOCKED) {
        if (!this->build_ultra_unlock_payload_(payload)) {
          this->command_pending_ = false;
          return;
        }

        ESP_LOGI(TAG, "Sending A1 Ultra V4 remote unlock");
      } else {
        // manual_lock / DP46 = true
        payload = {
            0x00, 0x00, 0x00, 0x00,
            0x01,
            0x2E,
            0x00, 0x00, 0x01,
            0x01
        };

        ESP_LOGI(TAG, "Sending A1 Ultra V4 manual lock");
      }

      this->send_packet_(
          FUN_SENDER_DPS_V4,
          payload,
          0
      );
    } else {
      uint8_t dp = this->pending_dp_;

      std::vector<uint8_t> payload = {
          dp,
          0x01,
          0x01,
          0x01
      };

      ESP_LOGI(
          TAG,
          "Sending A1 Pro Max DP%u = true",
          dp
      );

      this->send_packet_(
          FUN_SENDER_DPS,
          payload,
          0
      );
    }

    this->command_pending_ = false;

    if (this->state_sensor_ == nullptr) {
      this->publish_state(
          this->pending_lock_state_
      );
    }
  }


  // ---------------------------------------------------------------------------
  // Incoming Tuya BLE v3 datapoints
  // ---------------------------------------------------------------------------

  void parse_datapoints_v3_(
      const uint8_t *data,
      size_t len
  ) {
    size_t pos = 0;

    while (len - pos >= 3) {
      uint8_t dp_id = data[pos++];
      uint8_t type = data[pos++];
      uint8_t value_len = data[pos++];

      if (pos + value_len > len)
        return;

      if (
          type == 0x01 &&
          value_len == 1
      ) {
        bool value =
            data[pos] != 0;

        ESP_LOGD(
            TAG,
            "RX DP%u BOOL=%s",
            dp_id,
            value ? "true" : "false"
        );

        if (dp_id == 47 && this->state_sensor_ == nullptr) {
          this->publish_state(
              value
                  ? lock::LOCK_STATE_UNLOCKED
                  : lock::LOCK_STATE_LOCKED
          );
        }
      }

      pos += value_len;
    }
  }


  // ---------------------------------------------------------------------------
  // Incoming TuyaOS FD50 / V4 datapoints
  // ---------------------------------------------------------------------------

  void parse_datapoints_v4_(
      const uint8_t *data,
      size_t len
  ) {
    // Observed passive physical-state event:
    // <id:1> <flags:3=0x00002F> <type:1=BOOL> <len:2=1> <value:1>
    // true  = open/unlocked
    // false = closed/locked
    for (size_t pos = 0; pos + 8 <= len; pos++) {
      uint32_t flags =
          (static_cast<uint32_t>(data[pos + 1]) << 16) |
          (static_cast<uint32_t>(data[pos + 2]) << 8) |
          static_cast<uint32_t>(data[pos + 3]);

      uint8_t type = data[pos + 4];
      uint16_t value_len = get_u16_be(data + pos + 5);

      if (
          flags == 0x00002F &&
          type == 0x01 &&
          value_len == 1 &&
          pos + 8 <= len
      ) {
        bool value = data[pos + 7] != 0;

        ESP_LOGD(
            TAG,
            "RX A1 Ultra physical state: %s",
            value ? "UNLOCKED" : "LOCKED"
        );

        if (this->state_sensor_ == nullptr) {
          this->publish_state(
              value
                  ? lock::LOCK_STATE_UNLOCKED
                  : lock::LOCK_STATE_LOCKED
          );
        }

        return;
      }
    }

    ESP_LOGD(TAG, "RX A1 Ultra V4 payload without known state event");
  }


  // ---------------------------------------------------------------------------
  // Reset protocol state on every fresh BLE connection
  // ---------------------------------------------------------------------------

  void reset_protocol_() {
    this->sequence_ = 1;

    this->paired_ = false;
    this->session_key_valid_ = false;

    this->protocol_version_ =
        this->model_ == GimdowModel::A1_ULTRA
            ? 2
            : V3_PROTOCOL_VERSION;

    this->reset_rx_();

    this->tx_packets_.clear();
    this->tx_index_ = 0;
  }


  // ---------------------------------------------------------------------------
  // Configuration
  // ---------------------------------------------------------------------------

  ble_client::BLEClient *ble_parent_{nullptr};

  GimdowModel model_{GimdowModel::A1_PRO_MAX};

  std::string local_key_string_;
  std::string uuid_;
  std::string device_id_;
  std::string ble_unlock_check_;

  binary_sensor::BinarySensor *state_sensor_{nullptr};
  bool external_state_initialized_{false};


  // ---------------------------------------------------------------------------
  // Keys / protocol state
  // ---------------------------------------------------------------------------

  std::array<uint8_t, 6> local_key_{};
  std::array<uint8_t, 16> login_key_{};
  std::array<uint8_t, 16> session_key_{};

  bool session_key_valid_{false};
  bool paired_{false};
  uint8_t protocol_version_{V3_PROTOCOL_VERSION};


  // ---------------------------------------------------------------------------
  // GATT
  // ---------------------------------------------------------------------------

  uint16_t notify_handle_{0};
  uint16_t write_handle_{0};

  uint16_t negotiated_mtu_{23};
  bool notifications_ready_{false};
  bool device_info_sent_{false};
  bool mtu_requested_{false};


  // ---------------------------------------------------------------------------
  // TX
  // ---------------------------------------------------------------------------

  uint32_t sequence_{1};

  std::vector<std::vector<uint8_t>> tx_packets_;
  size_t tx_index_{0};
  uint32_t last_tx_ms_{0};


  // ---------------------------------------------------------------------------
  // RX
  // ---------------------------------------------------------------------------

  std::vector<uint8_t> rx_buffer_;

  uint32_t rx_expected_packet_{0};
  size_t rx_expected_length_{0};


  // ---------------------------------------------------------------------------
  // Pending lock command
  // ---------------------------------------------------------------------------

  bool command_pending_{false};
  uint8_t pending_dp_{0};

  lock::LockState pending_lock_state_{
      lock::LOCK_STATE_NONE
  };
};


}  // namespace gimdow_ble
}  // namespace esphome
