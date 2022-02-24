/*
 * Copyright (c) 2020 The Fuchsia Authors
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#ifndef SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_SIM_SIM_ERRINJ_H_
#define SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_SIM_SIM_ERRINJ_H_

#include <net/ethernet.h>
#include <zircon/status.h>

#include <cstring>
#include <optional>
#include <vector>

#include "src/connectivity/wlan/drivers/testing/lib/sim-env/sim-env.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/bits.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/debug.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/fweh.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/fwil_types.h"

namespace wlan::brcmfmac {
// Error inject class that enables setting various types of SIM FW errors.
class SimErrorInjector {
 public:
  explicit SimErrorInjector();
  ~SimErrorInjector();

  // Iovar int command specific
  void AddErrInjCmd(uint32_t cmd, zx_status_t ret_status, bcme_status_t ret_fw_err,
                    std::optional<uint16_t> ifidx = {});
  void DelErrInjCmd(uint32_t cmd);
  bool CheckIfErrInjCmdEnabled(uint32_t cmd, zx_status_t* ret_status, bcme_status_t* ret_fw_err,
                               uint16_t ifidx);

  // Iovar string command specific
  void AddErrInjIovar(const char* iovar, zx_status_t ret_status, bcme_status_t ret_fw_err,
                      std::optional<uint16_t> ifidx = {},
                      const std::vector<uint8_t>* alt_data = nullptr);
  void DelErrInjIovar(const char* iovar);
  bool CheckIfErrInjIovarEnabled(const char* iovar, zx_status_t* ret_status,
                                 bcme_status_t* ret_fw_err,
                                 const std::vector<uint8_t>** alt_value_out, uint16_t ifidx);

  // Iovar int command specific
  void AddErrEventInjCmd(uint32_t cmd, brcmf_fweh_event_code event_code,
                         brcmf_fweh_event_status_t ret_status, status_code_t ret_reason,
                         uint16_t flags, std::optional<uint16_t> ifidx = {});
  void DelErrEventInjCmd(uint32_t cmd);
  bool CheckIfErrEventInjCmdEnabled(uint32_t cmd, brcmf_fweh_event_code& event_code,
                                    brcmf_fweh_event_status_t& ret_status,
                                    status_code_t& ret_reason, uint16_t& flags,
                                    std::optional<uint16_t> ifidx);

  // Configure the mac address as reported by the (simulated) bootloader
  void SetBootloaderMacAddr(const wlan::common::MacAddr& mac_addr) {
    bootloader_mac_addr_ = mac_addr;
  }
  std::optional<wlan::common::MacAddr> BootloaderMacAddr() { return bootloader_mac_addr_; }

 private:
  struct ErrInjCmd {
    uint32_t cmd;
    std::optional<uint16_t> ifidx;
    zx_status_t ret_status;
    bcme_status_t ret_fw_err;

    ErrInjCmd(uint32_t cmd, zx_status_t status, bcme_status_t fw_err, std::optional<uint16_t> ifidx)
        : cmd(cmd), ifidx(ifidx), ret_status(status), ret_fw_err(fw_err) {}
  };

  struct ErrInjIovar {
    // Name of the iovar to override
    std::vector<uint8_t> iovar;

    // If set, only apply this override on the specified interface
    std::optional<uint16_t> ifidx;

    // Status code to return when iovar is read
    zx_status_t ret_status;

    // Firmware error code to return through bcdc.
    bcme_status_t ret_fw_err;

    // If set, specifies bytes to be used to override the payload
    const std::vector<uint8_t>* alt_data;

    ErrInjIovar(const char* iovar_str, zx_status_t status, bcme_status_t fw_err,
                std::optional<uint16_t> ifidx = {}, const std::vector<uint8_t>* alt_data = nullptr)
        : iovar(strlen(iovar_str) + 1),
          ifidx(ifidx),
          ret_status(status),
          ret_fw_err(fw_err),
          alt_data(alt_data) {
      std::memcpy(iovar.data(), iovar_str, strlen(iovar_str) + 1);
    }
  };

  struct ErrEventInjCmd {
    uint32_t cmd;
    std::optional<uint16_t> ifidx;
    brcmf_fweh_event_code event_code;
    brcmf_fweh_event_status_t ret_status;
    status_code_t ret_reason;
    uint16_t flags;

    ErrEventInjCmd(uint32_t cmd, brcmf_fweh_event_code event_code,
                   brcmf_fweh_event_status_t ret_status, status_code_t ret_reason, uint16_t flags,
                   std::optional<uint16_t> ifidx)
        : cmd(cmd),
          ifidx(ifidx),
          event_code(event_code),
          ret_status(ret_status),
          ret_reason(ret_reason),
          flags(flags) {}
  };

  std::list<ErrInjCmd> cmds_;
  std::list<ErrInjIovar> iovars_;
  std::list<ErrEventInjCmd> event_cmds_;

  // If set, overrides the bootloader-reported mac address
  std::optional<wlan::common::MacAddr> bootloader_mac_addr_ = {};
};

}  // namespace wlan::brcmfmac

#endif  // SRC_CONNECTIVITY_WLAN_DRIVERS_THIRD_PARTY_BROADCOM_BRCMFMAC_SIM_SIM_ERRINJ_H_
