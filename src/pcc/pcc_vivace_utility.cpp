#ifdef QUIC_PORT
#ifdef QUIC_PORT_LOCAL
#include "net/quic/core/congestion_control/pcc_utility_calculator.h"
#else
#include "third_party/pcc_quic/pcc_utility_calculator.h"
#endif
#else
#include "pcc_utility_calculator.h"
#endif

#ifdef QUIC_PORT
#ifdef QUIC_PORT_LOCAL
namespace net {
#else
namespace gfe_quic {
#endif
#endif

namespace {
// Number of probing MonitorIntervals necessary for Probing.
//const size_t kRoundsPerProbing = 4;
// Tolerance of loss rate by utility function.
const float kLossTolerance = 0.05f;
// Coefficeint of the loss rate term in utility function.
const float kLossCoefficient = -1000.0f;
// Coefficient of RTT term in utility function.
const float kRTTCoefficient = -200.0f;
// Coefficienty of the latency term in the utility function.
const float kLatencyCoefficient = 1;
// Alpha factor in the utility function.
const float kAlpha = 1;
// An exponent in the utility function.
const float kExponent = 0.9;
}  // namespace

float CalculateUtility(MonitorInterval& interval) {
  
  float sending_rate_bps = interval.GetObsSendingRate();
  float rtt_inflation = interval.GetObsRttInflation(); 
  float avg_rtt = interval.GetObsRtt();
  float loss_rate = interval->GetObsLossRate();

  float rtt_penalty = int(int(latency_inflation * 100) / 100.0 * 100) / 2 * 2/ 100.0;
  float rtt_contribution = kLatencyCoefficient * 11330 * (pow(rtt_penalty, 1));

  float loss_contribution = (11.35 * (pow((1 + loss_rate), 1) - 1));
  if (loss_rate <= 0.03) {
    loss_contribution = (1 * (pow((1 + loss_rate), 1) - 1));
  }
  float sending_factor = kAlpha * pow(sending_rate_bps/kBitsPerMegabit, kExponent);
  loss_contribution *= -1.0 * (sending_rate_bps / kBitsPerMegabit);
  rtt_contribution *= -1.0 * 1500 (sending_rate_bps / kBitsPerMegabit);
  
  float vivace_latency_utility = sending_factor + loss_contribution + rtt_contribution;
  return vivace_latency_utility;
}

#ifdef QUIC_PORT
} // namespace gfe_quic
#endif
