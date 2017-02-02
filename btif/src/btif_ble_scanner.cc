/******************************************************************************
 *
 *  Copyright (C) 2016 The Android Open Source Project
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at:
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 ******************************************************************************/

#define LOG_TAG "bt_btif_scanner"

#include <base/bind.h>
#include <base/threading/thread.h>
#include <errno.h>
#include <hardware/bluetooth.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_set>
#include "device/include/controller.h"

#include "btcore/include/bdaddr.h"
#include "btif_common.h"
#include "btif_util.h"

#include <hardware/bt_gatt.h>

#include "bta_api.h"
#include "bta_closure_api.h"
#include "bta_gatt_api.h"
#include "btif_config.h"
#include "btif_dm.h"
#include "btif_gatt.h"
#include "btif_gatt_util.h"
#include "btif_storage.h"
#include "osi/include/log.h"
#include "vendor_api.h"

using base::Bind;
using base::Owned;
using std::vector;
using RegisterCallback = BleScannerInterface::RegisterCallback;

extern bt_status_t do_in_jni_thread(const base::Closure& task);
extern const btgatt_callbacks_t* bt_gatt_callbacks;

#define SCAN_CBACK_IN_JNI(P_CBACK, ...)                              \
  do {                                                               \
    if (bt_gatt_callbacks && bt_gatt_callbacks->scanner->P_CBACK) {  \
      BTIF_TRACE_API("HAL bt_gatt_callbacks->client->%s", #P_CBACK); \
      do_in_jni_thread(                                              \
          Bind(bt_gatt_callbacks->scanner->P_CBACK, __VA_ARGS__));   \
    } else {                                                         \
      ASSERTC(0, "Callback is NULL", 0);                             \
    }                                                                \
  } while (0)

namespace std {
template <>
struct hash<bt_bdaddr_t> {
  size_t operator()(const bt_bdaddr_t& f) const {
    return f.address[0] + f.address[1] + f.address[2] + f.address[3] +
           f.address[4] + f.address[5];
  }
};

template <>
struct equal_to<bt_bdaddr_t> {
  size_t operator()(const bt_bdaddr_t& x, const bt_bdaddr_t& y) const {
    return memcmp(x.address, y.address, BD_ADDR_LEN);
  }
};
}

namespace {

std::unordered_set<bt_bdaddr_t> p_dev_cb;

void btif_gattc_add_remote_bdaddr(BD_ADDR p_bda, uint8_t addr_type) {
  bt_bdaddr_t bd_addr;
  memcpy(bd_addr.address, p_bda, BD_ADDR_LEN);
  p_dev_cb.insert(bd_addr);
}

bool btif_gattc_find_bdaddr(BD_ADDR p_bda) {
  bt_bdaddr_t bd_addr;
  memcpy(bd_addr.address, p_bda, BD_ADDR_LEN);
  return (p_dev_cb.count(bd_addr) != 0);
}

void btif_gattc_init_dev_cb(void) { p_dev_cb.clear(); }

btgattc_error_t btif_gattc_translate_btm_status(tBTM_STATUS status) {
  switch (status) {
    case BTM_SUCCESS:
    case BTM_SUCCESS_NO_SECURITY:
      return BT_GATTC_COMMAND_SUCCESS;

    case BTM_CMD_STARTED:
      return BT_GATTC_COMMAND_STARTED;

    case BTM_BUSY:
      return BT_GATTC_COMMAND_BUSY;

    case BTM_CMD_STORED:
      return BT_GATTC_COMMAND_STORED;

    case BTM_NO_RESOURCES:
      return BT_GATTC_NO_RESOURCES;

    case BTM_MODE_UNSUPPORTED:
    case BTM_WRONG_MODE:
    case BTM_MODE4_LEVEL4_NOT_SUPPORTED:
      return BT_GATTC_MODE_UNSUPPORTED;

    case BTM_ILLEGAL_VALUE:
    case BTM_SCO_BAD_LENGTH:
      return BT_GATTC_ILLEGAL_VALUE;

    case BTM_UNKNOWN_ADDR:
      return BT_GATTC_UNKNOWN_ADDR;

    case BTM_DEVICE_TIMEOUT:
      return BT_GATTC_DEVICE_TIMEOUT;

    case BTM_FAILED_ON_SECURITY:
    case BTM_REPEATED_ATTEMPTS:
    case BTM_NOT_AUTHORIZED:
      return BT_GATTC_SECURITY_ERROR;

    case BTM_DEV_RESET:
    case BTM_ILLEGAL_ACTION:
      return BT_GATTC_INCORRECT_STATE;

    case BTM_BAD_VALUE_RET:
      return BT_GATTC_INVALID_CONTROLLER_OUTPUT;

    case BTM_DELAY_CHECK:
      return BT_GATTC_DELAYED_ENCRYPTION_CHECK;

    case BTM_ERR_PROCESSING:
    default:
      return BT_GATTC_ERR_PROCESSING;
  }
}

void btif_gatts_upstreams_evt(uint16_t event, char* p_param) {
  LOG_VERBOSE(LOG_TAG, "%s: Event %d", __func__, event);

  tBTA_GATTC* p_data = (tBTA_GATTC*)p_param;
  switch (event) {
    case BTA_GATTC_DEREG_EVT:
      break;

    case BTA_GATTC_SEARCH_CMPL_EVT: {
      HAL_CBACK(bt_gatt_callbacks, client->search_complete_cb,
                p_data->search_cmpl.conn_id, p_data->search_cmpl.status);
      break;
    }

    default:
      LOG_DEBUG(LOG_TAG, "%s: Unhandled event (%d)", __func__, event);
      break;
  }
}

void bta_gatts_cback(tBTA_GATTC_EVT event, tBTA_GATTC* p_data) {
  bt_status_t status =
      btif_transfer_context(btif_gatts_upstreams_evt, (uint16_t)event,
                            (char*)p_data, sizeof(tBTA_GATTC), NULL);
  ASSERTC(status == BT_STATUS_SUCCESS, "Context transfer failed!", status);
}

void bta_scan_param_setup_cb(tGATT_IF client_if, tBTM_STATUS status) {
  SCAN_CBACK_IN_JNI(scan_parameter_setup_completed_cb, client_if,
                    btif_gattc_translate_btm_status(status));
}

void bta_scan_filt_cfg_cb(uint8_t filt_type, uint8_t client_if,
                          tBTM_BLE_PF_AVBL_SPACE avbl_space,
                          tBTM_BLE_PF_ACTION action, tBTA_STATUS status) {
  SCAN_CBACK_IN_JNI(scan_filter_cfg_cb, action, client_if, status, filt_type,
                    avbl_space);
}

void bta_scan_filt_param_setup_cb(tBTM_BLE_REF_VALUE ref_value,
                                  tBTM_BLE_PF_AVBL_SPACE avbl_space,
                                  uint8_t action_type, tBTA_STATUS status) {
  SCAN_CBACK_IN_JNI(scan_filter_param_cb, action_type, ref_value, status,
                    avbl_space);
}

void bta_scan_filt_status_cb(tBTM_BLE_REF_VALUE ref_value, uint8_t action,
                             tBTA_STATUS status) {
  SCAN_CBACK_IN_JNI(scan_filter_status_cb, action, ref_value, status);
}

void bta_batch_scan_threshold_cb(tBTM_BLE_REF_VALUE ref_value) {
  SCAN_CBACK_IN_JNI(batchscan_threshold_cb, ref_value);
}

void bta_batch_scan_reports_cb(int client_id, tBTA_STATUS status,
                               uint8_t report_format, uint8_t num_records,
                               std::vector<uint8_t> data) {
  SCAN_CBACK_IN_JNI(batchscan_reports_cb, client_id, status, report_format,
                    num_records, std::move(data));
}

void bta_scan_results_cb_impl(bt_bdaddr_t bd_addr, tBT_DEVICE_TYPE device_type,
                              int8_t rssi, uint8_t addr_type,
                              vector<uint8_t> value) {
  uint8_t remote_name_len;
  const uint8_t* p_eir_remote_name = NULL;
  bt_device_type_t dev_type;
  bt_property_t properties;

  p_eir_remote_name = BTM_CheckEirData(
      value.data(), BTM_EIR_COMPLETE_LOCAL_NAME_TYPE, &remote_name_len);

  if (p_eir_remote_name == NULL) {
    p_eir_remote_name = BTM_CheckEirData(
        value.data(), BT_EIR_SHORTENED_LOCAL_NAME_TYPE, &remote_name_len);
  }

  if ((addr_type != BLE_ADDR_RANDOM) || (p_eir_remote_name)) {
    if (!btif_gattc_find_bdaddr(bd_addr.address)) {
      btif_gattc_add_remote_bdaddr(bd_addr.address, addr_type);

      if (p_eir_remote_name) {
        bt_bdname_t bdname;
        memcpy(bdname.name, p_eir_remote_name, remote_name_len);
        bdname.name[remote_name_len] = '\0';

        LOG_VERBOSE(LOG_TAG, "%s BLE device name=%s len=%d dev_type=%d",
                    __func__, bdname.name, remote_name_len, device_type);
        btif_dm_update_ble_remote_properties(bd_addr.address, bdname.name,
                                             device_type);
      }
    }
  }

  dev_type = (bt_device_type_t)device_type;
  BTIF_STORAGE_FILL_PROPERTY(&properties, BT_PROPERTY_TYPE_OF_DEVICE,
                             sizeof(dev_type), &dev_type);
  btif_storage_set_remote_device_property(&(bd_addr), &properties);

  btif_storage_set_remote_addr_type(&bd_addr, addr_type);

  HAL_CBACK(bt_gatt_callbacks, scanner->scan_result_cb, &bd_addr, rssi,
            std::move(value));
}

void bta_scan_results_cb(tBTA_DM_SEARCH_EVT event, tBTA_DM_SEARCH* p_data) {
  uint8_t len;

  if (event == BTA_DM_INQ_CMPL_EVT) {
    BTIF_TRACE_DEBUG("%s  BLE observe complete. Num Resp %d", __func__,
                     p_data->inq_cmpl.num_resps);
    return;
  }

  if (event != BTA_DM_INQ_RES_EVT) {
    BTIF_TRACE_WARNING("%s : Unknown event 0x%x", __func__, event);
    return;
  }

  vector<uint8_t> value(BTGATT_MAX_ATTR_LEN);
  if (p_data->inq_res.p_eir) {
    value.insert(value.begin(), p_data->inq_res.p_eir,
                 p_data->inq_res.p_eir + 62);

    if (BTM_CheckEirData(p_data->inq_res.p_eir,
                         BTM_EIR_COMPLETE_LOCAL_NAME_TYPE, &len)) {
      p_data->inq_res.remt_name_not_required = true;
    }
  }

  bt_bdaddr_t bdaddr;
  bdcpy(bdaddr.address, p_data->inq_res.bd_addr);
  do_in_jni_thread(Bind(bta_scan_results_cb_impl, bdaddr,
                        p_data->inq_res.device_type, p_data->inq_res.rssi,
                        p_data->inq_res.ble_addr_type, std::move(value)));
}

void bta_track_adv_event_cb(tBTM_BLE_TRACK_ADV_DATA* p_track_adv_data) {
  btgatt_track_adv_info_t* btif_scan_track_cb = new btgatt_track_adv_info_t;

  BTIF_TRACE_DEBUG("%s", __func__);
  btif_gatt_move_track_adv_data(btif_scan_track_cb,
                                (btgatt_track_adv_info_t*)p_track_adv_data);

  SCAN_CBACK_IN_JNI(track_adv_event_cb, Owned(btif_scan_track_cb));
}

class BleScannerInterfaceImpl : public BleScannerInterface {
  ~BleScannerInterfaceImpl(){};

  void RegisterScanner(RegisterCallback cb) override {
    do_in_bta_thread(
        FROM_HERE,
        Bind(
            [](RegisterCallback cb) {
              BTA_GATTC_AppRegister(
                  bta_gatts_cback,
                  base::Bind(
                      [](RegisterCallback cb, uint8_t client_id,
                         uint8_t status) {
                        do_in_jni_thread(base::Bind(cb, client_id, status));
                      },
                      std::move(cb)));
            },
            std::move(cb)));
  }

  void Unregister(int scanner_id) override {
    do_in_bta_thread(FROM_HERE, Bind(&BTA_GATTC_AppDeregister, scanner_id));
  }

  void Scan(bool start) override {
    if (!start) {
      do_in_bta_thread(FROM_HERE, Bind(&BTA_DmBleObserve, false, 0, nullptr));
      return;
    }

    btif_gattc_init_dev_cb();
    do_in_bta_thread(FROM_HERE,
                     Bind(&BTA_DmBleObserve, true, 0,
                          (tBTA_DM_SEARCH_CBACK*)bta_scan_results_cb));
  }

  void ScanFilterParamSetup(
      uint8_t client_if, uint8_t action, uint8_t filt_index,
      std::unique_ptr<btgatt_filt_param_setup_t> filt_param) override {
    BTIF_TRACE_DEBUG("%s", __func__);

    if (filt_param && filt_param->dely_mode == 1) {
      do_in_bta_thread(
          FROM_HERE, base::Bind(BTM_BleTrackAdvertiser, bta_track_adv_event_cb,
                                client_if));
    }

    do_in_bta_thread(
        FROM_HERE,
        base::Bind(&BTM_BleAdvFilterParamSetup, action, filt_index,
                   base::Passed(&filt_param),
                   base::Bind(&bta_scan_filt_param_setup_cb, client_if)));
  }

  void ScanFilterAddRemove(int client_if, int action, int filt_type,
                           int filt_index, int company_id, int company_id_mask,
                           const bt_uuid_t* p_uuid,
                           const bt_uuid_t* p_uuid_mask,
                           const bt_bdaddr_t* bd_addr, char addr_type,
                           vector<uint8_t> data,
                           vector<uint8_t> mask) override {
    BTIF_TRACE_DEBUG("%s, %d, %d", __func__, action, filt_type);

    /* If data is passed, both mask and data have to be the same length */
    if (data.size() != mask.size() && data.size() != 0 && mask.size() != 0)
      return;

    switch (filt_type) {
      case BTM_BLE_PF_ADDR_FILTER: {
        tBLE_BD_ADDR target_addr;
        bdcpy(target_addr.bda, bd_addr->address);
        target_addr.type = addr_type;

        do_in_bta_thread(
            FROM_HERE,
            base::Bind(&BTM_LE_PF_addr_filter, action, filt_index,
                       std::move(target_addr),
                       Bind(&bta_scan_filt_cfg_cb, filt_type, client_if)));
        return;
      }

      case BTM_BLE_PF_SRVC_DATA:
        do_in_bta_thread(FROM_HERE,
                         base::Bind(&BTM_LE_PF_srvc_data, action, filt_index));
        return;

      case BTM_BLE_PF_SRVC_UUID:
      case BTM_BLE_PF_SRVC_SOL_UUID: {
        tBT_UUID bt_uuid;
        btif_to_bta_uuid(&bt_uuid, p_uuid);

        if (p_uuid_mask == NULL) {
          do_in_bta_thread(
              FROM_HERE,
              base::Bind(&BTM_LE_PF_uuid_filter, action, filt_index, filt_type,
                         bt_uuid, BTM_BLE_PF_LOGIC_AND, nullptr,
                         Bind(&bta_scan_filt_cfg_cb, filt_type, client_if)));
          return;
        }

        tBTM_BLE_PF_COND_MASK* mask = new tBTM_BLE_PF_COND_MASK;
        btif_to_bta_uuid_mask(mask, p_uuid_mask, p_uuid);
        do_in_bta_thread(
            FROM_HERE,
            base::Bind(&BTM_LE_PF_uuid_filter, action, filt_index, filt_type,
                       bt_uuid, BTM_BLE_PF_LOGIC_AND, base::Owned(mask),
                       Bind(&bta_scan_filt_cfg_cb, filt_type, client_if)));
        return;
      }

      case BTM_BLE_PF_LOCAL_NAME: {
        do_in_bta_thread(
            FROM_HERE, base::Bind(&BTM_LE_PF_local_name, action, filt_index,
                                  std::move(data), Bind(&bta_scan_filt_cfg_cb,
                                                        filt_type, client_if)));
        return;
      }

      case BTM_BLE_PF_MANU_DATA: {
        do_in_bta_thread(
            FROM_HERE,
            base::Bind(&BTM_LE_PF_manu_data, action, filt_index, company_id,
                       company_id_mask, std::move(data), std::move(mask),
                       Bind(&bta_scan_filt_cfg_cb, filt_type, client_if)));
        return;
      }

      case BTM_BLE_PF_SRVC_DATA_PATTERN: {
        do_in_bta_thread(
            FROM_HERE,
            base::Bind(&BTM_LE_PF_srvc_data_pattern, action, filt_index,
                       std::move(data), std::move(mask),
                       Bind(&bta_scan_filt_cfg_cb, filt_type, client_if)));
        return;
      }

      default:
        LOG_ERROR(LOG_TAG, "%s: Unknown filter type (%d)!", __func__, action);
        return;
    }
  }

  void ScanFilterClear(int client_if, int filter_index) override {
    BTIF_TRACE_DEBUG("%s: filter_index: %d", __func__, filter_index);
    do_in_bta_thread(FROM_HERE,
                     base::Bind(&BTM_LE_PF_clear, filter_index,
                                Bind(&bta_scan_filt_cfg_cb, BTM_BLE_PF_TYPE_ALL,
                                     client_if)));
  }

  void ScanFilterEnable(int client_if, bool enable) override {
    BTIF_TRACE_DEBUG("%s: enable: %d", __func__, enable);

    uint8_t action = enable ? 1 : 0;
    do_in_bta_thread(
        FROM_HERE, base::Bind(&BTM_BleEnableDisableFilterFeature, action,
                              base::Bind(&bta_scan_filt_status_cb, client_if)));
  }

  void SetScanParameters(int client_if, int scan_interval,
                         int scan_window) override {
    do_in_bta_thread(
        FROM_HERE,
        base::Bind(&BTM_BleSetScanParams, client_if, scan_interval, scan_window,
                   BTM_BLE_SCAN_MODE_ACTI, bta_scan_param_setup_cb));
  }

  void BatchscanConfigStorage(int client_if, int batch_scan_full_max,
                              int batch_scan_trunc_max,
                              int batch_scan_notify_threshold) override {
    base::Callback<void(uint8_t /* status */)> cb = base::Bind(
        [](int client_if, uint8_t status) {
          SCAN_CBACK_IN_JNI(batchscan_cfg_storage_cb, client_if, status);
        },
        client_if);

    do_in_bta_thread(
        FROM_HERE,
        base::Bind(&BTM_BleSetStorageConfig, (uint8_t)batch_scan_full_max,
                   (uint8_t)batch_scan_trunc_max,
                   (uint8_t)batch_scan_notify_threshold, cb,
                   bta_batch_scan_threshold_cb, (tBTM_BLE_REF_VALUE)client_if));
  }

  void BatchscanEnable(int client_if, int scan_mode, int scan_interval,
                       int scan_window, int addr_type,
                       int discard_rule) override {
    auto cb = base::Bind(
        [](int client_if, uint8_t status) {
          SCAN_CBACK_IN_JNI(batchscan_enb_disable_cb, 1, client_if, status);
        },
        client_if);

    do_in_bta_thread(
        FROM_HERE, base::Bind(&BTM_BleEnableBatchScan, scan_mode, scan_interval,
                              scan_window, discard_rule, addr_type, cb));
  }

  void BatchscanDisable(int client_if) override {
    auto cb = base::Bind(
        [](int client_if, uint8_t status) {
          SCAN_CBACK_IN_JNI(batchscan_enb_disable_cb, 1, client_if, status);
        },
        client_if);

    do_in_bta_thread(FROM_HERE, base::Bind(&BTM_BleDisableBatchScan, cb));
  }

  void BatchscanReadReports(int client_if, int scan_mode) override {
    do_in_bta_thread(FROM_HERE,
                     base::Bind(&BTM_BleReadScanReports, (uint8_t)scan_mode,
                                Bind(bta_batch_scan_reports_cb, client_if)));
  }
};

BleScannerInterface* btLeScannerInstance = nullptr;

}  // namespace

BleScannerInterface* get_ble_scanner_instance() {
  if (btLeScannerInstance == nullptr)
    btLeScannerInstance = new BleScannerInterfaceImpl();

  return btLeScannerInstance;
}