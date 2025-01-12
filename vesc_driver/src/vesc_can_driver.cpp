// Copyright 2020 F1TENTH Foundation
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright
//     notice, this list of conditions and the following disclaimer.
//
//   * Redistributions in binary form must reproduce the above copyright
//     notice, this list of conditions and the following disclaimer in the
//     documentation and/or other materials provided with the distribution.
//
//   * Neither the name of the {copyright_holder} nor the names of its
//     contributors may be used to endorse or promote products derived from
//     this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

// -*- mode:c++; fill-column: 100; -*-

#include "vesc_driver/vesc_can_driver.hpp"

#include <vesc_msgs/msg/vesc_state.hpp>
#include <vesc_msgs/msg/vesc_state_stamped.hpp>

#include <cassert>
#include <chrono>
#include <cmath>
#include <memory>
#include <sstream>
#include <string>

namespace vesc_driver
{

using namespace std::chrono_literals;
using std::placeholders::_1;
using std_msgs::msg::Float64;
using vesc_msgs::msg::VescStateStamped;
using sensor_msgs::msg::Imu;

VescCanDriver::VescCanDriver()
: rclcpp::Node("vesc_can_driver"),
  duty_cycle_limit_(this, "duty_cycle", -1.0, 1.0),
  current_limit_(this, "current"),
  brake_limit_(this, "brake"),
  speed_limit_(this, "speed"),
  position_limit_(this, "position"),
  servo_limit_(this, "servo", 0.0, 1.0),
  driver_mode_(MODE_INITIALIZING),
  fw_version_major_(-1),
  fw_version_minor_(-1)
{
  // get vesc serial port address
  std::string port = declare_parameter<std::string>("port", "can0");
  vesc_.SetStateUpdatedCallback(std::bind(&VescCanDriver::VescStateUpdatedCallback, this, 
        std::placeholders::_1));

  controller_id_ = declare_parameter<uint8_t>("vesc_id", 0x68);

  // create vesc state (telemetry) publisher
  state_pub_ = create_publisher<VescStateStamped>("sensors/core", rclcpp::QoS{10});
  imu_pub_ = create_publisher<VescImuStamped>("sensors/imu", rclcpp::QoS{10});
  imu_std_pub_ = create_publisher<Imu>("sensors/imu/raw", rclcpp::QoS{10});

  // since vesc state does not include the servo position, publish the commanded
  // servo position as a "sensor"
  servo_sensor_pub_ = create_publisher<Float64>(
    "sensors/servo_position_command", rclcpp::QoS{10});

  // subscribe to motor and servo command topics
  duty_cycle_sub_ = create_subscription<Float64>(
    "commands/motor/duty_cycle", rclcpp::QoS{10}, std::bind(
      &VescCanDriver::dutyCycleCallback, this,
      _1));
  current_sub_ = create_subscription<Float64>(
    "commands/motor/current", rclcpp::QoS{10}, std::bind(&VescCanDriver::currentCallback, this, _1));
  brake_sub_ = create_subscription<Float64>(
    "commands/motor/brake", rclcpp::QoS{10}, std::bind(&VescCanDriver::brakeCallback, this, _1));
  speed_sub_ = create_subscription<Float64>(
    "commands/motor/speed", rclcpp::QoS{10}, std::bind(&VescCanDriver::speedCallback, this, _1));
  position_sub_ = create_subscription<Float64>(
    "commands/motor/position", rclcpp::QoS{10}, std::bind(&VescCanDriver::positionCallback, this, _1));
  servo_sub_ = create_subscription<Float64>(
    "commands/servo/position", rclcpp::QoS{10}, std::bind(&VescCanDriver::servoCallback, this, _1));

  // attempt to connect to the serial port
  try {
    vesc_.Connect(port, controller_id_);
  } catch (...) {
    RCLCPP_FATAL(get_logger(), "Failed to connect to the VESC %x @ %s.", controller_id_, port.c_str());
    rclcpp::shutdown();
    return;
  }

  // create a 50Hz timer, used for state machine & polling VESC telemetry
  timer_ = create_wall_timer(20ms, std::bind(&VescCanDriver::timerCallback, this));

  RCLCPP_INFO(get_logger(), "VESC driver started, listening to node 0x%x @ %s.", controller_id_, port.c_str());
}

/* TODO or TO-THINKABOUT LIST
  - what should we do on startup? send brake or zero command?
  - what to do if the vesc interface gives an error?
  - check version number against know compatable?
  - should we wait until we receive telemetry before sending commands?
  - should we track the last motor command
  - what to do if no motor command received recently?
  - what to do if no servo command received recently?
  - what is the motor safe off state (0 current?)
  - what to do if a command parameter is out of range, ignore?
  - try to predict vesc bounds (from vesc config) and command detect bounds errors
*/

void VescCanDriver::timerCallback()
{
//   // VESC interface should not unexpectedly disconnect, but test for it anyway
//   if (!vesc_.isConnected()) {
//     RCLCPP_FATAL(get_logger(), "Unexpectedly disconnected from serial port.");
//     rclcpp::shutdown();
//     return;
//   }

  /*
   * Driver state machine, modes:
   *  INITIALIZING - request and wait for vesc version
   *  OPERATING - receiving commands from subscriber topics
   */
  if (driver_mode_ == MODE_INITIALIZING) {
        if(state_msg_received_) {
            driver_mode_ = MODE_OPERATING;
            RCLCPP_INFO(get_logger(), "VESC driver initialized.");
        }
  } else if (driver_mode_ == MODE_OPERATING) {
  } else {
    // unknown mode, how did that happen?
    assert(false && "unknown driver mode");
  }
}

void VescCanDriver::VescStateUpdatedCallback(const robosw::StampedVescState &msg) {
    auto state_msg = VescStateStamped();
    state_msg.header.stamp = now();

    if(!state_msg_received_) {
        state_msg_received_ = true;
    }

    state_msg.state.voltage_input = msg.state.voltage_input;
    state_msg.state.current_motor = msg.state.current_motor;
    state_msg.state.current_input = msg.state.current_input;
    state_msg.state.avg_id = 0;
    state_msg.state.avg_iq = 0;
    state_msg.state.duty_cycle = msg.state.duty_cycle;
    state_msg.state.speed = msg.state.speed;

    state_msg.state.charge_drawn = msg.state.charge_drawn;
    state_msg.state.charge_regen = msg.state.charge_regen;
    state_msg.state.energy_drawn = msg.state.energy_drawn;
    state_msg.state.energy_regen = msg.state.energy_regen;
    state_msg.state.displacement = msg.state.displacement;
    state_msg.state.distance_traveled = msg.state.distance_traveled;
    state_msg.state.fault_code = msg.state.fault_code;

    state_msg.state.pid_pos_now = 0;
    state_msg.state.controller_id = 0x68;

    state_msg.state.ntc_temp_mos1 = msg.state.temperature_pcb;
    state_msg.state.ntc_temp_mos2 = msg.state.temperature_pcb;
    state_msg.state.ntc_temp_mos3 = msg.state.temperature_pcb;
    state_msg.state.avg_vd = 0;
    state_msg.state.avg_vq = 0;

    state_pub_->publish(state_msg);
}

/**
 * @param duty_cycle Commanded VESC duty cycle. Valid range for this driver is -1 to +1. However,
 *                   note that the VESC may impose a more restrictive bounds on the range depending
 *                   on its configuration, e.g. absolute value is between 0.05 and 0.95.
 */
void VescCanDriver::dutyCycleCallback(const Float64::SharedPtr duty_cycle)
{
  if (driver_mode_ = MODE_OPERATING) {
    vesc_.SetDutyCycle(duty_cycle_limit_.clip(duty_cycle->data));
  }
}

/**
 * @param current Commanded VESC current in Amps. Any value is accepted by this driver. However,
 *                note that the VESC may impose a more restrictive bounds on the range depending on
 *                its configuration.
 */
void VescCanDriver::currentCallback(const Float64::SharedPtr current)
{
  if (driver_mode_ = MODE_OPERATING) {
    vesc_.SetCurrent(current_limit_.clip(current->data));
  }
}

/**
 * @param brake Commanded VESC braking current in Amps. Any value is accepted by this driver.
 *              However, note that the VESC may impose a more restrictive bounds on the range
 *              depending on its configuration.
 */
void VescCanDriver::brakeCallback(const Float64::SharedPtr brake)
{
  if (driver_mode_ = MODE_OPERATING) {
    vesc_.SetBrake(brake_limit_.clip(brake->data));
  }
}

/**
 * @param speed Commanded VESC speed in electrical RPM. Electrical RPM is the mechanical RPM
 *              multiplied by the number of motor poles. Any value is accepted by this
 *              driver. However, note that the VESC may impose a more restrictive bounds on the
 *              range depending on its configuration.
 */
void VescCanDriver::speedCallback(const Float64::SharedPtr speed)
{
  if (driver_mode_ = MODE_OPERATING) {
    vesc_.SetSpeed(speed_limit_.clip(speed->data));
//     RCLCPP_INFO(get_logger(), "rpm cmd %f, %f.", speed->data, speed_limit_.clip(speed->data));
  }
}

/**
 * @param position Commanded VESC motor position in radians. Any value is accepted by this driver.
 *                 Note that the VESC must be in encoder mode for this command to have an effect.
 */
void VescCanDriver::positionCallback(const Float64::SharedPtr position)
{
  if (driver_mode_ = MODE_OPERATING) {
    // ROS uses radians but VESC seems to use degrees. Convert to degrees.
    double position_deg = position_limit_.clip(position->data) * 180.0 / M_PI;
    vesc_.SetPosition(position_deg);
  }
}

/**
 * @param servo Commanded VESC servo output position. Valid range is 0 to 1.
 */
void VescCanDriver::servoCallback(const Float64::SharedPtr servo)
{
  if (driver_mode_ = MODE_OPERATING) {
    double servo_clipped(servo_limit_.clip(servo->data));
    vesc_.SetServo(servo_clipped);
//     RCLCPP_INFO(get_logger(), "servo cmd %f.", servo->data);

    // publish clipped servo value as a "sensor"
    auto servo_sensor_msg = Float64();
    servo_sensor_msg.data = servo_clipped;
    servo_sensor_pub_->publish(servo_sensor_msg);
  }
}

VescCanDriver::CommandLimit::CommandLimit(
  rclcpp::Node * node_ptr,
  const std::string & str,
  const std::experimental::optional<double> & min_lower,
  const std::experimental::optional<double> & max_upper)
: node_ptr(node_ptr),
  logger(node_ptr->get_logger()),
  name(str)
{
  // check if user's minimum value is outside of the range min_lower to max_upper
  auto param_min =
    node_ptr->declare_parameter(name + "_min", rclcpp::ParameterValue(0.0));

  if (param_min.get_type() != rclcpp::ParameterType::PARAMETER_NOT_SET) {
    if (min_lower && param_min.get<double>() < *min_lower) {
      lower = *min_lower;
      RCLCPP_WARN_STREAM(
        logger, "Parameter " << name << "_min (" << param_min.get<double>() <<
          ") is less than the feasible minimum (" << *min_lower << ").");
    } else if (max_upper && param_min.get<double>() > *max_upper) {
      lower = *max_upper;
      RCLCPP_WARN_STREAM(
        logger, "Parameter " << name << "_min (" << param_min.get<double>() <<
          ") is greater than the feasible maximum (" << *max_upper << ").");
    } else {
      lower = param_min.get<double>();
    }
  } else if (min_lower) {
    lower = *min_lower;
  }

  // check if the uers' maximum value is outside of the range min_lower to max_upper
  auto param_max =
    node_ptr->declare_parameter(name + "_max", rclcpp::ParameterValue(0.0));

  if (param_max.get_type() != rclcpp::ParameterType::PARAMETER_NOT_SET) {
    if (min_lower && param_max.get<double>() < *min_lower) {
      upper = *min_lower;
      RCLCPP_WARN_STREAM(
        logger, "Parameter " << name << "_max (" << param_max.get<double>() <<
          ") is less than the feasible minimum (" << *min_lower << ").");
    } else if (max_upper && param_max.get<double>() > *max_upper) {
      upper = *max_upper;
      RCLCPP_WARN_STREAM(
        logger, "Parameter " << name << "_max (" << param_max.get<double>() <<
          ") is greater than the feasible maximum (" << *max_upper << ").");
    } else {
      upper = param_max.get<double>();
    }
  } else if (max_upper) {
    upper = *max_upper;
  }

  // check for min > max
  if (upper && lower && *lower > *upper) {
    RCLCPP_WARN_STREAM(
      logger, "Parameter " << name << "_max (" << *upper <<
        ") is less than parameter " << name << "_min (" << *lower << ").");
    double temp(*lower);
    lower = *upper;
    upper = temp;
  }

  std::ostringstream oss;
  oss << "  " << name << " limit: ";

  if (lower) {
    oss << *lower << " ";
  } else {
    oss << "(none) ";
  }

  if (upper) {
    oss << *upper;
  } else {
    oss << "(none)";
  }

  RCLCPP_DEBUG_STREAM(logger, oss.str());
}

double VescCanDriver::CommandLimit::clip(double value)
{
  auto clock = rclcpp::Clock(RCL_ROS_TIME);

  if (lower && value < lower) {
    RCLCPP_INFO_THROTTLE(
      logger, clock, 10, "%s command value (%f) below minimum limit (%f), clipping.",
      name.c_str(), value, *lower);
    return *lower;
  }
  if (upper && value > upper) {
    RCLCPP_INFO_THROTTLE(
      logger, clock, 10, "%s command value (%f) above maximum limit (%f), clipping.",
      name.c_str(), value, *upper);
    return *upper;
  }
  return value;
}

}  // namespace vesc_driver

// #include "rclcpp_components/register_node_macro.hpp"  // NOLINT

// RCLCPP_COMPONENTS_REGISTER_NODE(vesc_driver::VescCanDriver)
