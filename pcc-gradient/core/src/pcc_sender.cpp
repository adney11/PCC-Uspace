#include "pcc_sender.h"
#include "core.h"

#include <stdio.h>
#include <algorithm>

#define DEBUG_RATE_CONTROL

namespace {
// Minimum sending rate of the connection.
const float kMinSendingRate = 2.0f;
// Step size for rate change in PROBING mode.
const float kProbingStepSize = 0.05f;
// Base step size for rate change in DECISION_MADE mode.
const float kDecisionMadeStepSize = 0.02f;
// Maximum step size for rate change in DECISION_MADE mode.
const float kMaxDecisionMadeStepSize = 0.10f;
// Groups of useful monitor intervals each time in PROBING mode.
const size_t kNumIntervalGroupsInProbing = 2;
// Number of bits per byte.
const size_t kBitsPerByte = 8;
// Number of bits per Mbit.
const size_t kMegabit = 1024 * 1024;
// An inital RTT value to use (10ms)
const size_t kInitialRttMicroseconds = 1 * 1000;
// Rtt moving average weight.
const float kAverageRttWeight = 0.1f;
// Idk, something tho.
const size_t kDefaultTCPMSS = 1400;
// Minimum number of packers per interval.
const size_t kMinimumPacketsPerInterval = 10;
// Number of gradients to average.
const size_t kAvgGradientSampleSize = 2;
// The factor that converts average utility gradient to a rate change (in Mbps).
float kUtilityGradientToRateChangeFactor = 1.0f;//2.0f;
// The smallest amount that the rate can be changed by at a time.
float kMinimumRateChange = 0.5f;
// The initial maximum proportional rate change.
float kInitialMaximumProportionalChange = 0.05f;//0.1f;
// The additional maximum proportional change each time it is incremented.
float kMaximumProportionalChangeStepSize = 0.06f;//0.07f;
}  // namespace

double ComputeMonitorDuration(double sending_rate_mbps, double rtt_us) {
    if (1.5 * rtt_us < kMinimumPacketsPerInterval * kBitsPerByte * 1456 / sending_rate_mbps) {
        //std::cerr << "Duration = " << 5.0 * kBitsPerByte * 1456 << " / " << sending_rate_mbps << ", rtt_us = " << rtt_us << std::endl;
        return kMinimumPacketsPerInterval * kBitsPerByte * 1456 / sending_rate_mbps;
    }
    //std::cerr << "Duration = 1.5 * " << rtt_us << std::endl;
    return 1.5 * rtt_us;
}

PccSender::PccSender(CUDT* cudt, int32_t initial_congestion_window,
                     int32_t max_congestion_window)
    : mode_(STARTING),
      monitor_duration_(0),
      direction_(INCREASE),
      rounds_(1),
      interval_queue_(/*delegate=*/this),
      avg_rtt_(0),
      last_rtt_(0),
      time_last_rtt_received_(0),
      avg_gradient_(0),
      swing_buffer_(0),
      rate_change_amplifier_(0),
      rate_change_proportion_allowance_(0),
      previous_change_(0),
      max_cwnd_bits_(max_congestion_window * kDefaultTCPMSS * kBitsPerByte) {
  sending_rate_mbps_ =
      std::max(static_cast<float>(initial_congestion_window * kDefaultTCPMSS *
                                  kBitsPerByte) /
                   (kMegabit * static_cast<float>(kInitialRttMicroseconds)),
               kMinSendingRate);
  latest_utility_info_.utility = 0.0f;
  latest_utility_info_.sending_rate_mbps = 0.0f;
  this->cudt = cudt;
  SetRate(sending_rate_mbps_);
}

bool PccSender::OnPacketSent(uint64_t sent_time,
                             int32_t packet_number,
                             int32_t bytes) {
  //std::cerr << "OnPacketSent(" << packet_number << ", " << bytes << ")" << std::endl;
  // Start a new monitor interval if (1) there is no useful interval in the
  // queue, or (2) it has been more than monitor_duration since the last
  // interval starts.
  if (interval_queue_.num_useful_intervals() == 0 ||
      sent_time - interval_queue_.current().first_packet_sent_time >
          monitor_duration_) {
    //std::cerr << "Num useful intervals = " << interval_queue_.num_useful_intervals() << std::endl;
    MaybeSetSendingRate();
    // Set the monitor duration to be 1.5 of avg_rtt_.
    monitor_duration_ = ComputeMonitorDuration(sending_rate_mbps_, avg_rtt_); //1.5 * avg_rtt_;
	
    //std::cerr << "Monitor duration: " << monitor_duration_ << std::endl;
    interval_queue_.EnqueueNewMonitorInterval(
        sending_rate_mbps_, CreateUsefulInterval(),
        avg_rtt_, sent_time + monitor_duration_);
    //std::cerr << "New num useful intervals = " << interval_queue_.num_useful_intervals() << std::endl;
  }
  interval_queue_.OnPacketSent(sent_time, packet_number, bytes);

  return true;
}

void PccSender::OnCongestionEvent(uint64_t event_time, uint64_t rtt,
                                  const CongestionVector& acked_packets,
                                  const CongestionVector& lost_packets) {
  if (acked_packets.size() > 0) {
      //std::cerr << "Updating RTT" << std::endl;
      if (avg_rtt_ == 0) {
        avg_rtt_ = rtt;
      } else {
          avg_rtt_ = (1 - kAverageRttWeight) * avg_rtt_ +
                        kAverageRttWeight * last_rtt_;
      }
      last_rtt_ = rtt;
      //std::cerr << "RTT = " << rtt << " avg. = " << avg_rtt_ << std::endl;
  }
  time_last_rtt_received_ = event_time;

  interval_queue_.OnCongestionEvent(acked_packets, lost_packets,
                                    avg_rtt_, event_time);
}

void PccSender::SetRate(double mbps) {
    #ifdef DEBUG_RATE_CONTROL
        std::cerr << "SetRate(" << mbps << ")" << std::endl;
    #endif
    cudt->m_pCC->m_dPktSndPeriod = (cudt->m_iMSS * 8.0) / mbps;
    cudt->m_ullInterval = (uint64_t)(cudt->m_iMSS * 8.0 * cudt->m_ullCPUFrequency) / mbps;
}

float PccSender::ComputeRateChange(
    const UtilityInfo& utility_sample_1, 
    const UtilityInfo& utility_sample_2) {

  if (utility_sample_1.sending_rate_mbps == utility_sample_2.sending_rate_mbps) {
    return kMinimumRateChange;
  }
  
  float utility_gradient = 
      (utility_sample_1.utility - utility_sample_2.utility) / 
      (utility_sample_1.sending_rate_mbps - 
          utility_sample_2.sending_rate_mbps);
  
  UpdateAverageGradient(utility_gradient);
  float change = avg_gradient_ * kUtilityGradientToRateChangeFactor;

  if (change * previous_change_ < 0) {
    rate_change_amplifier_ = 0;
    rate_change_proportion_allowance_ = 0;
    if (swing_buffer_ < 2) {
      ++swing_buffer_;
    }
  }

  if (rate_change_amplifier_ < 3) {
    change *= rate_change_amplifier_ + 1;
  } else if (rate_change_amplifier_ < 6) {
    change *= 2 * rate_change_amplifier_ - 2;
  } else if (rate_change_amplifier_ < 9) {
    change *= 4 * rate_change_amplifier_ - 14;
  } else {
    change *= 9 * rate_change_amplifier_ - 50;
  }

  if (change * previous_change_ > 0) {
    if (swing_buffer_ == 0) {
      if (rate_change_amplifier_ < 3) {
        rate_change_amplifier_ += 0.5;
      } else {
        ++rate_change_amplifier_;
      }
    }
    if (swing_buffer_ > 0) {
      --swing_buffer_;
    }
  }

  float max_allowed_change_ratio = 
    kInitialMaximumProportionalChange + 
    rate_change_proportion_allowance_ * kMaximumProportionalChangeStepSize;
    
  float change_ratio = change / sending_rate_mbps_;
  change_ratio = change_ratio > 0 ? change_ratio : -1 * change_ratio;

  if (change_ratio > max_allowed_change_ratio) {
    ++rate_change_proportion_allowance_;
    if (change < 0) {
      change = -1 * max_allowed_change_ratio * sending_rate_mbps_;
    } else {
      change = max_allowed_change_ratio * sending_rate_mbps_;
    }
  } else {
    if (rate_change_proportion_allowance_ > 0) {
      --rate_change_proportion_allowance_;
    }
  }

  if (change * previous_change_ < 0) {
    rate_change_amplifier_ = 0;
    rate_change_proportion_allowance_ = 0;
  }

  if (change < 0 && change > -1 * kMinimumRateChange) {
    change = -1 * kMinimumRateChange;
  } else if (change > 0 && change < kMinimumRateChange) {
    change = kMinimumRateChange;
  }

  #ifdef DEBUG_RATE_CONTROL
    std::cerr << "CalculateRateChange:" << std::endl;
    std::cerr << "\tUtility 1    = " << utility_sample_1.utility << std::endl;
    std::cerr << "\tRate 1       = " << utility_sample_1.sending_rate_mbps << "mbps" << std::endl;
    std::cerr << "\tUtility 2    = " << utility_sample_2.utility << std::endl;
    std::cerr << "\tRate 2       = " << utility_sample_2.sending_rate_mbps << "mbps" << std::endl;
    std::cerr << "\tGradient     = " << utility_gradient << std::endl;
    std::cerr << "\tAvg Gradient = " << avg_gradient_ << std::endl;
    std::cerr << "\tRate Change  = " << change << "mbps" << std::endl;
  #endif
  return change;
}

void PccSender::UpdateAverageGradient(float new_gradient) {
  if (gradient_samples_.empty()) {
    avg_gradient_ = new_gradient;
  } else if (gradient_samples_.size() < kAvgGradientSampleSize) {
    avg_gradient_ *= gradient_samples_.size();
    avg_gradient_ += new_gradient;
    avg_gradient_ /= gradient_samples_.size() + 1;
  } else {
    float oldest_gradient = gradient_samples_.front();
    avg_gradient_ -= oldest_gradient / kAvgGradientSampleSize;
    avg_gradient_ += new_gradient / kAvgGradientSampleSize;
    gradient_samples_.pop();
  }
  gradient_samples_.push(new_gradient);
}

void PccSender::OnUtilityAvailable(
    const std::vector<UtilityInfo>& utility_info) {
  #ifdef DEBUG_RATE_CONTROL
      std::cerr << "OnUtilityAvailable" << std::endl;
  #endif
  switch (mode_) {
    case STARTING:
      if (utility_info[0].utility > latest_utility_info_.utility) {
        // Stay in STARTING mode. Double the sending rate and update
        // latest_utility.
        sending_rate_mbps_ *= 2;
        SetRate(sending_rate_mbps_);
        latest_utility_info_ = utility_info[0];
        ++rounds_;
      } else {
        // Enter PROBING mode if utility decreases.
        EnterProbing();
      }
      break;
    case PROBING:
      if (CanMakeDecision(utility_info)) {
        // Enter DECISION_MADE mode if a decision is made.
        direction_ = (utility_info[0].utility > utility_info[1].utility)
                         ? ((utility_info[0].sending_rate_mbps >
                             utility_info[1].sending_rate_mbps)
                                ? INCREASE
                                : DECREASE)
                         : ((utility_info[0].sending_rate_mbps >
                             utility_info[1].sending_rate_mbps)
                                ? DECREASE
                                : INCREASE);
        latest_utility_info_ = 
            utility_info[2 * kNumIntervalGroupsInProbing - 2].utility >
            utility_info[2 * kNumIntervalGroupsInProbing - 1].utility ?
            utility_info[2 * kNumIntervalGroupsInProbing - 2] :
            utility_info[2 * kNumIntervalGroupsInProbing - 1];

        float rate_change = ComputeRateChange(utility_info[0], utility_info[1]);
        if (sending_rate_mbps_ + rate_change < kMinSendingRate) {
            rate_change = kMinSendingRate - sending_rate_mbps_;
        }
        previous_change_ = rate_change;
        EnterDecisionMade(sending_rate_mbps_ + rate_change);
      } else {
        //std::cerr << "Staying in probing mode" << std::endl;
        // Stays in PROBING mode.
        EnterProbing();
      }
      break;
    case DECISION_MADE:
      float rate_change = 
          ComputeRateChange(utility_info[0], latest_utility_info_);
      if (sending_rate_mbps_ + rate_change < kMinSendingRate) {
        rate_change = kMinSendingRate - sending_rate_mbps_;
      }
      // Test if we are adjusting sending rate in the same direction.
      if (rate_change * previous_change_ > 0) {
        // Remain in DECISION_MADE mode. Keep increasing or decreasing the
        // sending rate.
        previous_change_ = rate_change;
        sending_rate_mbps_ += rate_change;
        SetRate(sending_rate_mbps_);
        latest_utility_info_ = utility_info[0];
      } else {
        // Enter PROBING if our old rate change is no longer best.
        EnterProbing();
      }
      break;
  }
}

bool PccSender::CreateUsefulInterval() const {
  //std::cerr << "CreateUsefulInterval()" << std::endl;
  if (avg_rtt_ == 0) {
    // Create non useful intervals upon starting a connection, until there is
    // valid rtt stats.
    return false;
  }
  // In STARTING and DECISION_MADE mode, there should be at most one useful
  // intervals in the queue; while in PROBING mode, there should be at most
  // 2 * kNumIntervalGroupsInProbing.
  size_t max_num_useful =
      (mode_ == PROBING) ? 2 * kNumIntervalGroupsInProbing : 1;
  return interval_queue_.num_useful_intervals() < max_num_useful;
}

void PccSender::MaybeSetSendingRate() {
  //std::cerr << "MaybeSetSendingRate()" << std::endl;
  //std::cerr << "mode = " << (mode_ == PROBING ? "probing" : "not probing") << std::endl;
  if (mode_ != PROBING || (interval_queue_.num_useful_intervals() ==
                               2 * kNumIntervalGroupsInProbing &&
                           !interval_queue_.current().is_useful)) {
    // Do not change sending rate when (1) current mode is STARTING or
    // DECISION_MADE (since sending rate is already changed in
    // OnUtilityAvailable), or (2) more than 2 * kNumIntervalGroupsInProbing
    // intervals have been created in PROBING mode.
    return;
  }

  if (interval_queue_.num_useful_intervals() != 0) {
    // Restore central sending rate.
    if (direction_ == INCREASE) {
      sending_rate_mbps_ /= (1 + kProbingStepSize);
    } else {
      sending_rate_mbps_ /= (1 - kProbingStepSize);
    }

    SetRate(sending_rate_mbps_);
    if (interval_queue_.num_useful_intervals() ==
        2 * kNumIntervalGroupsInProbing) {
      // This is the first not useful monitor interval, its sending rate is the
      // central rate.
      return;
    }
  }

  // Sender creates several groups of monitor intervals. Each group comprises an
  // interval with increased sending rate and an interval with decreased sending
  // rate. Which interval goes first is randomly decided.
  if (interval_queue_.num_useful_intervals() % 2 == 0) {
    direction_ = (rand() % 2 == 1) ? INCREASE : DECREASE;
  } else {
    direction_ = (direction_ == INCREASE) ? DECREASE : INCREASE;
  }
  if (direction_ == INCREASE) {
    sending_rate_mbps_ *= (1 + kProbingStepSize);
  } else {
    sending_rate_mbps_ *= (1 - kProbingStepSize);
  }
  SetRate(sending_rate_mbps_);
}

bool PccSender::CanMakeDecision(
    const std::vector<UtilityInfo>& utility_info) const {
  //std::cerr << "CanMakeDecision()" << std::endl;

  // Determine whether increased or decreased probing rate has better utility.
  // Cannot make decision if number of utilities are less than
  // 2 * kNumIntervalGroupsInProbing. This happens when sender does not have
  // enough data to send.
  if (utility_info.size() < 2 * kNumIntervalGroupsInProbing) {
    return false;
  }

  for (UtilityInfo u: utility_info) {
    //std::cerr << "Utility info: u=" << u.utility << ", s=" << u.sending_rate_mbps << std::endl;
  }

  bool increase = false;
  // All the probing groups should have consistent decision. If not, directly
  // return false.
  for (size_t i = 0; i < kNumIntervalGroupsInProbing; ++i) {
    bool increase_i =
        utility_info[2 * i].utility > utility_info[2 * i + 1].utility
            ? utility_info[2 * i].sending_rate_mbps >
                  utility_info[2 * i + 1].sending_rate_mbps
            : utility_info[2 * i].sending_rate_mbps <
                  utility_info[2 * i + 1].sending_rate_mbps;

    if (i == 0) {
      increase = increase_i;
    }
    // Cannot make decision if groups have inconsistent results.
    if (increase_i != increase) {
      return false;
    }
  }

  return true;
}

void PccSender::EnterProbing() {
  //std::cerr << "EnterProbing()" << std::endl;
  switch (mode_) {
    case STARTING:
      // Use half sending_rate_ as central probing rate.
      sending_rate_mbps_ /= 2;
      break;
    case DECISION_MADE:
      // Use sending rate right before utility decreases as central probing
      // rate.
      if (direction_ == INCREASE) {
        sending_rate_mbps_ /= (1 + std::min(rounds_ * kDecisionMadeStepSize,
                                            kMaxDecisionMadeStepSize));
      } else {
        sending_rate_mbps_ /= (1 - std::min(rounds_ * kDecisionMadeStepSize,
                                            kMaxDecisionMadeStepSize));
      }
      break;
    case PROBING:
      // Reset sending rate to central rate when sender does not have enough
      // data to send more than 2 * kNumIntervalGroupsInProbing intervals.
      if (interval_queue_.current().is_useful) {
        if (direction_ == INCREASE) {
          sending_rate_mbps_ /= (1 + kProbingStepSize);
        } else {
          sending_rate_mbps_ /= (1 - kProbingStepSize);
        }
      }
      break;
  }
  SetRate(sending_rate_mbps_);

  if (mode_ == PROBING) {
    ++rounds_;
    return;
  }

  mode_ = PROBING;
  rounds_ = 1;
}

void PccSender::EnterDecisionMade(float new_rate) {
  sending_rate_mbps_ = new_rate;
  SetRate(new_rate);
  mode_ = DECISION_MADE;
  rounds_ = 1;
}