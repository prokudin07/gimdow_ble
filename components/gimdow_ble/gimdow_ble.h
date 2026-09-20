#pragma once

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

#include <esp_gattc_api.h>
#include <esp_system.h>

#include <mbedtls/aes.h>
#include <mbedtls/md5.h>
#include <mbedtls/version.h>


namespace esphome {
namespace gimdow_ble {


static const char *const TAG = "gimdow_ble";


// -----------------------------------------------------------------------------
// Tuya BLE v3 codes
// -----------------------------------------------------------------------------

static constexpr uint16_t FUN_SENDER_DEVICE_INFO = 0x0000;
static constexpr uint16_t FUN_SENDER_PAIR        = 0x0001;
static constexpr uint16_t FUN_SENDER_DPS         = 0x0002;

static constexpr uint16_t FUN_RECEIVE_DP         = 0x8001;
static constexpr uint16_t FUN_RECEIVE_TIME_DP    = 0x8003;
static constexpr uint16_t FUN_RECEIVE_TIME1_REQ  = 0x8011;
static constexpr uint16_t FUN_RECEIVE_TIME2_REQ  = 0x8012;


// -----------------------------------------------------------------------------
// Gimdow A1 PRO MAX / rlyxv7pe
// -----------------------------------------------------------------------------

static constexpr uint16_t TUYA_SERVICE_UUID = 0x1910;
static constexpr uint16_t TUYA_NOTIFY_UUID  = 0x2B10;
static constexpr uint16_t TUYA_WRITE_UUID   = 0x2B11;

static constexpr uint8_t PROTOCOL_VERSION = 3;
static constexpr size_t GATT_PACKET_SIZE = 20;


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

  void set_local_key(const std::string &value) {
    this->local_key_string_ = value;
  }

  void set_uuid(const std::string &value) {
    this->uuid_ = value;
  }

  void set_device_id(const std::string &value) {
    this->device_id_ = value;
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
    ESP_LOGI(TAG, "Initialising Gimdow BLE test lock");

    this->traits.set_assumed_state(true);

    if (this->local_key_string_.size() < 6) {
      ESP_LOGE(TAG, "local_key is too short");
      this->mark_failed();
      return;
    }

    // Tuya rlyxv7pe uses only first six bytes of local_key.
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

    ESP_LOGI(TAG, "Tuya login key prepared");
  }


  void dump_config() override {
    ESP_LOGCONFIG(TAG, "Gimdow BLE:");
    ESP_LOGCONFIG(TAG, "  UUID: %s", this->uuid_.c_str());
    ESP_LOGCONFIG(TAG, "  Device ID: %s", this->device_id_.c_str());
    ESP_LOGCONFIG(TAG, "  Protocol: Tuya BLE v3");
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
    // This avoids flooding Bluedroid with several GATT writes at once.

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

    // If already authenticated, command can be sent immediately.
    if (this->paired_ && this->session_key_valid_) {
      this->send_pending_command_();
      return;
    }

    // Otherwise request BLE connection.
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

      // -----------------------------------------------------------------------
      // GATT database discovered
      // -----------------------------------------------------------------------

      case ESP_GATTC_SEARCH_CMPL_EVT: {
        ESP_LOGI(TAG, "GATT discovery complete");

        auto *notify_chr = this->parent()->get_characteristic(
            TUYA_SERVICE_UUID,
            TUYA_NOTIFY_UUID
        );

        auto *write_chr = this->parent()->get_characteristic(
            TUYA_SERVICE_UUID,
            TUYA_WRITE_UUID
        );

        if (notify_chr == nullptr) {
          ESP_LOGE(TAG, "Tuya notify characteristic 0x2B10 not found");
          return;
        }

        if (write_chr == nullptr) {
          ESP_LOGE(TAG, "Tuya write characteristic 0x2B11 not found");
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

        // Do NOT set ESTABLISHED here.
        // ESPHome 2026 requires waiting until REG_FOR_NOTIFY_EVT.
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

        this->reset_protocol_();

        ESP_LOGI(TAG, "Sending DEVICE_INFO");

        this->send_packet_(
            FUN_SENDER_DEVICE_INFO,
            {},
            0
        );

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
  // CRC16 — same as Tuya-BLE Python implementation
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
        encrypted_len,
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


    // -------------------------------------------------------------------------
    // Split into 20-byte Tuya BLE fragments
    // -------------------------------------------------------------------------

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

        packet.push_back(
            PROTOCOL_VERSION << 4
        );
      }

      size_t available =
          GATT_PACKET_SIZE - packet.size();

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

      // Skip protocol-version nibble byte.
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

    // -------------------------------------------------------------------------
    // DEVICE_INFO response
    // -------------------------------------------------------------------------

    if (code == FUN_SENDER_DEVICE_INFO) {
      if (len < 46) {
        ESP_LOGE(TAG, "DEVICE_INFO response is too short");
        return;
      }

      uint8_t protocol_version = data[2];

      ESP_LOGI(
          TAG,
          "DEVICE_INFO received, protocol=%u.%u",
          data[2],
          data[3]
      );

      if (protocol_version != 3) {
        ESP_LOGW(
            TAG,            "Unexpected Tuya protocol %u",
            protocol_version
        );
      }


      // session_key = MD5(local_key[:6] + srand)
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


    // -------------------------------------------------------------------------
    // PAIR response
    // -------------------------------------------------------------------------

    if (code == FUN_SENDER_PAIR) {
      if (len != 1) {
        ESP_LOGE(TAG, "Invalid PAIR response");
        return;
      }

      uint8_t result = data[0];

      // 0 = paired
      // 2 = already paired
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


    // -------------------------------------------------------------------------
    // Datapoints
    // -------------------------------------------------------------------------

    if (code == FUN_RECEIVE_DP) {
      this->parse_datapoints_(
          data,
          len
      );
      return;
    }


    if (code == FUN_RECEIVE_TIME_DP) {
      // Timestamp prefix exists before datapoints.
      // Not needed for initial test.
      return;
    }


    if (
        code == FUN_RECEIVE_TIME1_REQ ||
        code == FUN_RECEIVE_TIME2_REQ
    ) {
      ESP_LOGD(
          TAG,
          "Time request received (ignored in test component)"
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
  // DP6 / DP46
  // ---------------------------------------------------------------------------

  void send_pending_command_() {
    if (!this->command_pending_)
      return;

    uint8_t dp = this->pending_dp_;

    // Tuya v3 bool DP:
    // DP ID | type BOOL(1) | len(1) | TRUE(1)

    std::vector<uint8_t> payload = {
        dp,
        0x01,
        0x01,
        0x01
    };

    ESP_LOGI(
        TAG,
        "Sending Gimdow DP%u = true",
        dp
    );

    this->send_packet_(
        FUN_SENDER_DPS,
        payload,
        0
    );

    this->command_pending_ = false;

    // Without an external state sensor, keep the original optimistic
    // behaviour. If state_sensor is configured, it is the only source
    // of the lock state.
    if (this->state_sensor_ == nullptr) {
      this->publish_state(
          this->pending_lock_state_
      );
    }
  }


  // ---------------------------------------------------------------------------
  // Basic incoming datapoint parser
  // ---------------------------------------------------------------------------

  void parse_datapoints_(
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

        // DP47 is used as the lock state only when no external
        // bolt/lock sensor was configured.
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
  // Reset protocol state on every fresh BLE connection
  // ---------------------------------------------------------------------------

  void reset_protocol_() {
    this->sequence_ = 1;

    this->paired_ = false;
    this->session_key_valid_ = false;

    this->reset_rx_();

    this->tx_packets_.clear();
    this->tx_index_ = 0;
  }


  // ---------------------------------------------------------------------------
  // Configuration
  // ---------------------------------------------------------------------------

  ble_client::BLEClient *ble_parent_{nullptr};

  std::string local_key_string_;
  std::string uuid_;
  std::string device_id_;

  // Optional external binary sensor of the physical bolt state.
  // ON  = UNLOCKED
  // OFF = LOCKED
  binary_sensor::BinarySensor *state_sensor_{nullptr};
  bool external_state_initialized_{false};


  // ---------------------------------------------------------------------------
  // Keys
  // ---------------------------------------------------------------------------

  std::array<uint8_t, 6> local_key_{};
  std::array<uint8_t, 16> login_key_{};
  std::array<uint8_t, 16> session_key_{};

  bool session_key_valid_{false};
  bool paired_{false};


  // ---------------------------------------------------------------------------
  // GATT
  // ---------------------------------------------------------------------------

  uint16_t notify_handle_{0};
  uint16_t write_handle_{0};


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