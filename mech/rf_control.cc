// Copyright 2020 Josh Pieper, jjp@pobox.com.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/// @file
///
/// # Received Commands #
///
/// The following slot protocol is used to receive commands.
///
/// ## Slot 0 - Primary Command ##
///   byte 0: Desired mode, from QuadrupedCommand::Mode, remaining
///           bytes are mode-specific
///
///    Zero Velocity - None
///    Joint - Not implemented
///    Leg - Not implemented
///    StandUp - None
///    Rest - none
///    Jump
///     byte 1: repeat
///     byte 2-3: uint16_t acceleration_mm_s2
///    Walk - none
///
/// ## Slot 1 - pose_mm_RB ##
///
///  * int16 x_mm
///  * int16 y_mm
///  * int16 z_mm
///  * int16 w (scaled to -1.0 -> 1.0)
///  * int16 x (scaled to -1.0 -> 1.0)
///  * int16 y (scaled to -1.0 -> 1.0)
///  * int16 z (scaled to -1.0 -> 1.0)
///
/// ## Slot 2 - v_mm_S_R, w_LR ##
///
///  * int16 v_mm_s_R.x
///  * int16 v_mm_s_R.y
///  * int16 w_LR.z (scaled to -+ 2 pi)
///
/// # Transmitted telemetry #
///
/// The following protocol is used to transmit telemetry.
///
/// ## Slot 0 - Primary State ##
///
///  * int8_t mode (from QuadrupedCommand::Mode)
///
/// ## Slot 1 - Movement State ##
///
///  * int16 v_mm_s_R.x
///  * int16 v_mm_s_R.y
///  * int16 w_LR.z (scaled to -+ 2 pi)
///
/// ## Slot 4 - Joints 1-4 ##
///
///  * 4x of the following struct
///    * int8_t temp_C
///    * int8_t fault
///    * int8_t mode
///
/// ## Slot 5 - Joints 5-8 ##
///
/// Same as slot 4
///
/// ## Slot 6 - Joints 9-12 ##
///
/// Same as slot 4
///
/// ## Slot 7 - Joint Summary ##
///
///  * int8_t min_voltage
///  * int8_t max_voltage
///  * int8_t min_temp_C
///  * int8_t max_temp_C
///  * int8_t fault
///
/// ## Slot 14 - Fault ##
///
///  * char[15] - truncated text string


#include "mech/rf_control.h"

#include "mjlib/io/now.h"

namespace pl = std::placeholders;

namespace mjmech {
namespace mech {

struct SlotData {
  boost::posix_time::ptime timestamp;

  struct Slot {
    boost::posix_time::ptime timestamp;
    uint8_t size = 0;
    std::array<uint8_t, 15> data = {};

    template <typename Archive>
    void Serialize(Archive* a) {
      a->Visit(MJ_NVP(timestamp));
      a->Visit(MJ_NVP(size));
      a->Visit(MJ_NVP(data));
    }
  };

  std::array<Slot, 15> rx = {};

  template <typename Archive>
  void Serialize(Archive* a) {
    a->Visit(MJ_NVP(timestamp));
    a->Visit(MJ_NVP(rx));
  }
};

class RfControl::Impl {
 public:
  Impl(const base::Context& context,
       QuadrupedControl* quadruped_control,
       RfGetter rf_getter)
      : executor_(context.executor),
        quadruped_control_(quadruped_control),
        rf_getter_(rf_getter) {
    context.telemetry_registry->Register("slotrf", &slotrf_signal_);
  }

  void AsyncStart(mjlib::io::ErrorCallback callback) {
    rf_ = rf_getter_();
    BOOST_ASSERT(!rf_);

    StartRead();

    boost::asio::post(
        executor_,
        std::bind(std::move(callback), mjlib::base::error_code()));
  }

 private:
  void StartRead() {
    rf_->AsyncWaitForSlot(
        &bitfield_, std::bind(&Impl::HandleSlot, this, pl::_1));
  }

  void HandleSlot(const mjlib::base::error_code& ec) {
    mjlib::base::FailIf(ec);

    slot_data_.timestamp = mjlib::io::Now(executor_.context());

    for (int i = 0; i < 15; i++) {
      if (!(bitfield_ & (1 << i))) { continue; }

      const auto slot = rf_->rx_slot(i);
      slot_data_.rx[i].timestamp = slot.timestamp;
      slot_data_.rx[i].size = slot.size;
      std::memcpy(&slot_data_.rx[i].data[0], &slot.data[0], slot.size);
      std::memset(&slot_data_.rx[i].data[slot.size], 0, 15 - slot.size);
    }

    slotrf_signal_(&slot_data_);

    // We only command when we receive a mode slot.
    if (bitfield_ & 0x01) {
      SendCommand();
    }

    StartRead();
  }

  void SendCommand() {
    // We just got a new command.  Formulate our command structure and
    // send it on.
    QuadrupedCommand command;

    command.mode = [&]() {
      switch (slot_data_.rx[0].data[0]) {
        case 1: {
          return QuadrupedCommand::Mode::kStopped;
        }
        case 3: {
          return QuadrupedCommand::Mode::kZeroVelocity;
        }
        case 7: {
          return QuadrupedCommand::Mode::kRest;
        }
        case 8: {
          QuadrupedCommand::Jump jump;
          jump.repeat = slot_data_.rx[0].data[1] != 0;
          uint16_t acceleration_mm_s2 = {};
          std::memcpy(&acceleration_mm_s2, &slot_data_.rx[0].data[2], 2);
          jump.acceleration_mm_s2 = acceleration_mm_s2;
          command.jump = jump;
          return QuadrupedCommand::Mode::kJump;
        }
        case 9: {
          command.walk = QuadrupedCommand::Walk();
          return QuadrupedCommand::Mode::kWalk;
        }
        default: {
          return QuadrupedCommand::Mode::kZeroVelocity;
        }

        mjlib::base::AssertNotReached();
      }
    }();

    auto read_int16 = [&](int slot, int pos) {
      int16_t value = 0;
      std::memcpy(&value, &slot_data_.rx[slot].data[pos], 2);
      return value;
    };

    Eigen::Quaterniond quat{
      read_int16(1, 6) / 32767.0,
          read_int16(1, 8) / 32767.0,
          read_int16(1, 10) / 32767.0,
          read_int16(1, 12) / 32767.0
          };
    if (quat.norm() == 0.0) {
      quat = Eigen::Quaterniond::Identity();
    }

    command.pose_mm_RB = Sophus::SE3d(
        quat,
        Eigen::Vector3d(
            read_int16(1, 0),
            read_int16(1, 2),
            read_int16(1, 4)));

    command.v_mm_s_R = {
      read_int16(2, 0) * 1.0,
      read_int16(2, 2) * 1.0,
      0.0
    };

    command.w_LR = {
      0.0,
      0.0,
      2.0 * M_PI * read_int16(2, 4) / 32767.0
    };

    // RF control takes precendence over everything if we're seeing
    // it.
    command.priority = 99;

    quadruped_control_->Command(command);
  }

  boost::asio::executor executor_;
  QuadrupedControl* const quadruped_control_;
  RfGetter rf_getter_;

  RfClient* rf_ = nullptr;

  uint16_t bitfield_ = 0;

  SlotData slot_data_;
  boost::signals2::signal<void (const SlotData*)> slotrf_signal_;
};

RfControl::RfControl(const base::Context& context,
                     QuadrupedControl* quadruped_control,
                     RfGetter rf_getter)
    : impl_(std::make_unique<Impl>(context, quadruped_control, rf_getter)) {}

RfControl::~RfControl() {}

void RfControl::AsyncStart(mjlib::io::ErrorCallback callback) {
  impl_->AsyncStart(std::move(callback));
}

clipp::group RfControl::program_options() {
  return {};
}

}
}