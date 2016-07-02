#ifndef __PCC_H__
#define __PCC_H__

#define _USE_MATH_DEFINES

#include <udt.h>
#include <ccc.h>
#include<iostream>
#include<cmath>
#include <deque>
#include <time.h>
#include <map>
#include <memory>
#include <deque>
#include <mutex>
#include <thread>
#include <stdlib.h>
//#define DEBUG_PRINT

using namespace std;

bool kInTimeout = false;

enum MeasurementType {
	FIRST,
	SECOND
};

class Measurement {
	public:
		Measurement(double base_rate, int other_monitor, double test_rate, MeasurementType t, int monitor_number): utility_(0), base_rate_(base_rate), other_monitor_(other_monitor), set_(false), rtt_(0), loss_(0), loss_panelty_(0), rtt_panelty_(0), test_rate_(test_rate), type_(t), monitor_number_(monitor_number) {}

		Measurement* copy() const {
			Measurement* ret = new Measurement(base_rate_, other_monitor_, test_rate_, type_, monitor_number_);
			ret->utility_ = utility_;
			ret->rtt_ = rtt_;
			ret->loss_ = loss_;
			ret->loss_panelty_ = loss_panelty_;
			ret->actual_packets_sent_rate_ = actual_packets_sent_rate_;
			return ret;
		}

		long double utility_;
		double base_rate_;
		int other_monitor_;
		bool set_;
		double rtt_;
		double loss_;
		double loss_panelty_;
		double rtt_panelty_;
		double actual_packets_sent_rate_;
		double test_rate_;
		MeasurementType type_;
		int monitor_number_;
};

struct GuessStat {
      int monitor;
      double rate;
      long double utility;
      bool ready;
      bool isup;
};


struct MoveStat {
      double rate;
      double next_rate;
      double change;
      long double utility;
      int target_monitor;
      bool bootstrapping;
      bool isup;
};

class PCC : public CCC {
public:
	virtual ~PCC() {}

	long double avg_utility() {
		if (measurement_intervals_ > 0) {
			return utility_sum_ / measurement_intervals_;
		}
		return 0;
	}

	virtual void onLoss(const int32_t*, const int&) {}
	virtual bool onTimeout(int total, int loss, double in_time, int current, int endMonitor, double rtt){
		lock_guard<mutex> lck(monitor_mutex_);
		//cerr << "handling timeout in PCC! for monitor " << monitor << endl;
//		if (state_ != START) {
//			if (start_measurment_map_.find(endMonitor) == start_measurment_map_.end() && end_measurment_map_.find(endMonitor) == end_measurment_map_.end()) {
//				#ifdef DEBUG_PRINT
//					cerr << "NOT IN START: monitor " << endMonitor << " already gone!" << endl;
//				#endif
//				return false;
//			}
//		} /*else if (monitor_in_start_phase_ != monitor) {
//			cerr << "START: monitor " << monitor << " already gone! current monitor: " << monitor_in_start_phase_ << endl;
//			return false;
//		}*/
//
//
//		kInTimeout = true;
//		long double curr_utility = utility(total, 0, in_time, rtt, NULL);
//		if (curr_utility > last_utility_) {
//			last_utility_ = curr_utility;
//			return true;
//		}
//		//#ifdef DEBUG_PRINT
//		cerr << "computing utility: total = " << total << ", loss = " << loss << " in_time = " << in_time << ", rtt = " << rtt << endl;
//		cerr << "current utility = " << curr_utility << " and previous utility = " << last_utility_ << endl;
//		cerr << "current rate " << rate() << " --> ";
//		//#endif
//		decide(last_utility_, curr_utility, true);
//
//
//
//		//setRate(0.75 * rate());
//		//base_rate_ = rate();
//		double r = rate();
//		//#ifdef DEBUG_PRINT
//			cerr << "timeout! new rate is " << r << endl;
//		//#endif
//		restart();
//		if (r > 1.01 * kMinRateMbps) {
//			cerr << "going to SEARCH rate = " << rate() << ". Thresh = " << 1.01 * kMinRateMbps << endl;
//			state_ = SEARCH;
//		} else {
//			cerr << "going to "<< kMinRateMbpsSlowStart << "mbps" << endl;
//			base_rate_ = kMinRateMbpsSlowStart;
//			restart();
//			slow_start_factor_ = 2;
//			state_ = START;
//			setRate(base_rate_);
//		}
//		//clear_state();
//		//start_measurment_map_.clear();
//		//end_measurment_map_.clear();
//		kInTimeout = false;
//		//cerr << "new rate: " << rate() << base_rate_ << endl;
//		return false;
	}
	virtual void onACK(const int& ack){}

	void keep_last_measurement(Measurement* measurement) {
		if (measurement->type_ == FIRST) {
			start_measurment_map_.insert(pair<int,shared_ptr<Measurement> >(measurement->monitor_number_, shared_ptr<Measurement>(measurement)));
			current_start_monitor_ = measurement->monitor_number_;
			start_measurement_ = false;
		} else {
			end_measurment_map_.insert(pair<int,shared_ptr<Measurement> >(measurement->monitor_number_, shared_ptr<Measurement>(measurement)));
			on_next_start_bind_to_end_ = measurement->monitor_number_;
			start_measurement_ = true;
		}
	}

	virtual void onMonitorStart(int current_monitor) {
		lock_guard<mutex> lck(monitor_mutex_);
        ConnectionState old_state;
        do {
            old_state = state_;
            switch (state_) {
                case START:
			        if (monitor_in_start_phase_ != -1) {
			        	return;
			        }
			        monitor_in_start_phase_ = current_monitor;
			        setRate(rate() * slow_start_factor_);
                    cerr<<"slow starting of monitor"<<current_monitor<<endl;
                    break;
                case SEARCH:
                    cerr<<"Monitor "<<current_monitor<<"is in search state"<<endl;
                    state_ = RECORDING;
			        search(current_monitor);
                    guess_time_ = 0;
                    break;
                case RECORDING:
                    if(guess_time_ != number_of_probes_) {
                        cerr<<"Monitor "<<current_monitor<<"is in recording state "<<guess_time_<<"th trial with rate of"<<guess_measurement_bucket[guess_time_].rate<<endl;
                        setRate(guess_measurement_bucket[guess_time_].rate);
                        guess_time_ ++;
                    } else {
                        cerr<<"Monitor "<<current_monitor<<"is in recording state, waiting result for recording to come back"<<endl;
                        setRate(base_rate_);
                    }
                    break;
                case MOVING:
                    // TODO: should handle how we move and how we record utility as well
                    cerr<<"monitor "<<current_monitor<<"is in moving state setting rate to"<<move_stat.next_rate<<endl;
                    setRate(move_stat.next_rate);
                    break;
            }

        } while(old_state != state_);
	}

	virtual void onMonitorEnds(int total, int loss, double in_time, int current, int endMonitor, double rtt) {
		lock_guard<mutex> lck(monitor_mutex_);
		rtt /= (1000 * 1000);
		if (rtt == 0) rtt = 0.0001;
		long double curr_utility = utility(total, loss, in_time, rtt, NULL);
		last_utility_ = curr_utility;
		utility_sum_ += curr_utility;
		measurement_intervals_++;
        ConnectionState old_state;
        // TODO we should keep track of all monitors and closely mointoring RTT
        // and utility change between monitors
        do {
            old_state = state_;
            switch (state_) {
                case START:
                    // TODO to aid debuggin as we change code architecture, we will not
                    // have slow start here, we will immediately transit to SEARCH state
                    state_ = SEARCH;
                    break;
                case SEARCH:
                    // When doing search (calculating the results and stuff), onmonitorends should do nothing
                    // and ignore the monitor that ended
                    cerr<<"monitor"<<current_monitor<< "ends in search state, this should not happen often"<<endl;
                    break;
                case RECORDING:
                    // onMoniitorEnd will check if all search results have come back
                    // and decide where to move the rate
                    // TODO: it should enter the MOVING state here, but I will just keep it simple to make it enter
                    // search state again. To first switch the architecture
                    bool all_ready;
                    all_ready = true;
                    cerr<<"checking if all recording ready at monitor"<<current_monitor<<endl;
                    for (int i=0; i<number_of_probes_; i++) {
                        if (guess_measurement_bucket[i].monitor == endMonitor) {
                            cerr<<"found matching monitor"<<endMonitor<<endl;
                            guess_measurement_bucket[i].utility = curr_utility;
                            guess_measurement_bucket[i].ready = true;
                        }

                        if(guess_measurement_bucket[i].ready == false) {
                            all_ready = false;
                        }
                    }

                    if (all_ready) {
                        double utility_down=0, utility_up=0;
                        double rate_up = 0, rate_down = 0;
                        for (int i=0; i<number_of_probes_; i++) {
                            if(guess_measurement_bucket[i].isup) {
                                utility_up += guess_measurement_bucket[i].utility;
                                rate_up = guess_measurement_bucket[i].rate;
                            } else {
                                utility_down += guess_measurement_bucket[i].utility;
                                rate_down = guess_measurement_bucket[i].rate;
                            }
                        }
                        int factor = number_of_probes_/2;
                        // Sanity check maybe needed here, but not sure
                        // but watch out for huge jump is needed
                        // maybe this will work, if this does not, need to revisit sanity check
                        double change = decide(utility_down/factor, utility_up/factor, rate_down, rate_up, false);
                        cerr<<"all record is acquired and ready to change by "<<change<<endl;
		                base_rate_ += change;
                        setRate(base_rate_);
                        state_ = MOVING;
                        move_stat.bootstrapping = true;
                        move_stat.target_monitor = (current +1) % 100;
                        move_stat.next_rate = base_rate_;
                        move_stat.rate = base_rate_;
                        move_stat.change = change;
                        guess_measurement_bucket.clear();
                    }
                    break;
                case MOVING:
                    if(endMonitor == move_stat.target_monitor) {
                        cerr<<"find the right monitor"<<endMonitor<<endl;
                        if(move_stat.bootstrapping) {
                            cerr<<"bootstrapping move operations"<<endl;
                            move_stat.bootstrapping = false;
                            move_stat.utility = curr_utility;
                            // change stay the same
                            move_stat.target_monitor = (current + 1) % 100;
                            cerr<<"target monitor is "<<(current + 1) % 100;
                            move_stat.next_rate = move_stat.next_rate + move_stat.change;
                            base_rate_ = move_stat.next_rate;
                            setRate(base_rate_);
                        } else {
                            // see if the change direction is wrong and is reversed
                            double change = decide(move_stat.utility, curr_utility, move_stat.next_rate - move_stat.change, move_stat.next_rate, false);
                            cerr<<"change for move is "<<change<<endl;
                            if (change * move_stat.change < 0) {
                                cerr<<"direction changed"<<endl;
                                cerr<<"change is "<<change<<" old change is "<<move_stat.change<<endl;
                            // the direction is different, need to move to old rate start to re-guess
                                if (abs(change) > abs(move_stat.change)) {
                                    base_rate_ = move_stat.next_rate + change;
                                } else {
                                    base_rate_ = move_stat.next_rate - move_stat.change;
                                }
                                setRate(base_rate_);
                                state_ = SEARCH;
                            } else {
                                cerr<<"direction same, keep moving with change of "<<change<<endl;
                                move_stat.target_monitor = (current + 1) % 100;
                                move_stat.utility = curr_utility;
                                move_stat.next_rate = move_stat.change + move_stat.next_rate;
                                base_rate_ = move_stat.next_rate;
                                setRate(base_rate_);
                            }
                        }
                    }
		            prev_utility_ = curr_utility;
                    // should add target monitor
                    // decide based on prev_utility_
                    // and prev_rate_
                    break;
            }

        } while(old_state != state_);

	}

	static void set_utility_params(double alpha = 4, double beta = 54, double exponent = 1.5, bool polyUtility = true) {
		kAlpha = alpha;
		kBeta = beta;
		kExponent = exponent;
		kPolyUtility = polyUtility;
	}

protected:

	static double kAlpha, kBeta, kExponent;
	static bool kPolyUtility;

    long double search_monitor_utility[2];
    int search_monitor_number[2];
    bool start_measurement_;
	double base_rate_;
	bool kPrint;
	double prev_change_;
	static constexpr double kMinRateMbps = 0.5;
	static constexpr double kMinRateMbpsSlowStart = 0.1;
	static constexpr double kMaxRateMbps = 1024.0;

	enum ConnectionState {
		START,
		SEARCH,
        RECORDING,
        MOVING
	} state_;


	virtual void search(int current_monitor) = 0;
	virtual double decide(long double start_utility, long double end_utility, double old_rate, double new_rate,  bool force_change) = 0;

	virtual void clear_state() {
		continue_slow_start_ = true;
		start_measurement_ = true;
		slow_start_factor_ = 1.1;
		start_measurment_map_.clear();
		end_measurment_map_.clear();
		state_ = SEARCH;
		monitor_in_start_phase_ = -1;
		prev_utility_ = -10000000;
		kPrint = false;
	}

	virtual void restart() {
		continue_slow_start_ = true;
		start_measurement_ = true;
		slow_start_factor_ = 1.2;
		start_measurment_map_.clear();
		end_measurment_map_.clear();
		monitor_in_start_phase_ = -1;
		setRate(base_rate_);
		kPrint = false;
		state_ = SEARCH;
		prev_utility_ = -10000000;
	}

	PCC() : start_measurement_(true), base_rate_(0.6), kPrint(false), state_(START), monitor_in_start_phase_(-1), slow_start_factor_(2), number_of_probes_(2), guess_time_(0),
			alpha_(kAlpha), beta_(kBeta), exponent_(kExponent), poly_utlity_(kPolyUtility), rate_(0.5), monitor_in_prog_(-1), utility_sum_(0), measurement_intervals_(0), prev_utility_(-10000000), continue_slow_start_(true), last_utility_(0) {
		m_dPktSndPeriod = 10000;
		m_dCWndSize = 100000.0;
        prev_change_ = 0;

		setRTO(100000000);
		srand(time(NULL));
		cerr << "new Code!!!" << endl;
		cerr << "configuration: alpha = " << alpha_ << ", beta = " << beta_   << ", exponent = " << exponent_ << " poly utility = " << poly_utlity_ << endl;

		/*
		if (!latency_mode) {
			beta_ = 0;
		} else {
			beta_ = 50;
		}
		*/
	}

	virtual double getMinChange(){
		if (base_rate_ > kMinRateMbps) {
			return kMinRateMbps;
		} else if (base_rate_ > kMinRateMbps / 2) {
			return kMinRateMbps / 2;
		} else {
			return 2 * kMinRateMbpsSlowStart;
		}
	}
	virtual void setRate(double mbps) {
		cerr << "set rate: " << rate_ << " --> " << mbps << endl;
		if (state_ == START) {
			if (mbps < kMinRateMbpsSlowStart){
				#ifdef DEBUG_PRINT
					cerr << "rate is mimimal at slow start, changing to " << kMinRateMbpsSlowStart << " instead" << endl;
				#endif
				mbps = kMinRateMbpsSlowStart;
			}
		} else if (mbps < kMinRateMbps){
			#ifdef DEBUG_PRINT
				cerr << "rate is mimimal, changing to " << kMinRateMbps << " instead" << endl;
			#endif
			mbps = kMinRateMbps;
		}

		if (mbps > kMaxRateMbps) {
			mbps = kMaxRateMbps;
			cerr << "rate is maximal, changing to " << kMaxRateMbps << " instead" << endl;
		}
		rate_ = mbps;
		m_dPktSndPeriod = (m_iMSS * 8.0) / mbps;
		//cerr << "setting rate: mbps = " << mbps << endl;
	}

	double rate() const { return rate_; }

public:
	static double get_rtt(double rtt) {
		double conv_diff = (double)(((long) (rtt * 1000 * 1000)) % kMillisecondsDigit);
		return conv_diff / (1000.0 * 1000.0);
	}

    double isAllSearchResultBack(int current_monitor) {
		if ((start_measurment_map_.find(current_monitor) != start_measurment_map_.end()) && (start_measurment_map_.at(current_monitor)->set_)) {
			int other_monitor = start_measurment_map_.at(current_monitor)->other_monitor_;
			return ((end_measurment_map_.find(other_monitor) != end_measurment_map_.end()) && (end_measurment_map_.at(other_monitor)->set_));
		} else if ((end_measurment_map_.find(current_monitor) != end_measurment_map_.end()) && (end_measurment_map_.at(current_monitor)->set_)) {
			int other_monitor = end_measurment_map_.at(current_monitor)->other_monitor_;
			return ((start_measurment_map_.find(other_monitor) != start_measurment_map_.end()) && (start_measurment_map_.at(other_monitor)->set_));
		}
		return false;
    }

	double get_min_rtt(double curr_rtt) {
		double min = curr_rtt;
		if ((rtt_history_.size()) == 0) {
			min = curr_rtt;
		} else {
			min = *rtt_history_.cbegin();
			for (deque<double>::const_iterator it = rtt_history_.cbegin(); it!=rtt_history_.cend(); ++it) {
				if (min > *it) {
					min = *it;
				}
			}
		}

		rtt_history_.push_front(curr_rtt);
		if (rtt_history_.size() > kHistorySize) {
			rtt_history_.pop_back();
		}

		return min;
	}

	bool sanety_check(Measurement* start, Measurement* end) {

		if (end->test_rate_ < start->test_rate_) {
			//cerr << "swapping. Rates: " << start->test_rate_ << ", " << end->test_rate_ << endl;
			Measurement* swap_temp = start;
			start = end;
			end = swap_temp;
		}

		if (start->loss_panelty_ < end->loss_panelty_) {
			//cerr << "failed on loss. Start = " << start->loss_panelty_ << ". End = " << end->loss_panelty_ << endl;
			return false;
		}
		if (start->rtt_panelty_ < end->rtt_panelty_) {
			//cerr << "failed on rtt" << endl;
			return false;
		}
		if (start->actual_packets_sent_rate_ < end->actual_packets_sent_rate_) {
			//cerr << "failed on packets sent" << endl;
			return false;
		}
		return true;
	}

	virtual long double utility(unsigned long total, unsigned long loss, double time, double rtt, Measurement* out_measurement) {
		static long double last_measurement_interval = 1;

		long double norm_measurement_interval = last_measurement_interval;

		if (!kInTimeout) {
			norm_measurement_interval = time / rtt;
			last_measurement_interval = norm_measurement_interval;
		}


		// convert to milliseconds
		double rtt_penalty = rtt / get_min_rtt(rtt);
                cerr<<"RTT penalty is"<<rtt_penalty<<endl;
                cerr<<"rtt is"<<rtt<<endl;
		//if (rtt_penalty > 2) rtt_penalty  = 2;
		//if (rtt_penalty < -2) rtt_penalty  = -2;
		exponent_ = 2.5;

		long double loss_contribution = total * (long double) (alpha_* (pow((1+((long double)((double) loss/(double) total))), exponent_)-1));
		long double rtt_contribution = 1.8 * total*(pow(rtt_penalty,2) - 1);
                long double rtt_factor = rtt;
                //TODO We should also consider adding just rtt into the utility function, because it is not just change that matters
                // This may turn out to be extremely helpful during LTE environment
		long double utility = ((long double)total - loss_contribution - rtt_contribution)/norm_measurement_interval/rtt;
		//long double utility = ((long double)total - loss_contribution - rtt_contribution)/norm_measurement_interval;

		if (out_measurement != NULL) {
			out_measurement->loss_panelty_ = loss_contribution / norm_measurement_interval;
			out_measurement->rtt_panelty_ = rtt_contribution / norm_measurement_interval;
			out_measurement->actual_packets_sent_rate_ = total / norm_measurement_interval;
		}
		return utility;
	}

	static const long kMillisecondsDigit = 10 * 1000;

	int monitor_in_start_phase_;
	double slow_start_factor_;
	double alpha_;
	double beta_;
	double exponent_;
	bool poly_utlity_;
	double rate_;
	int monitor_in_prog_;
	long double utility_sum_;
	size_t measurement_intervals_;
	long double prev_utility_;
	bool continue_slow_start_;
	map<int, shared_ptr<Measurement> > start_measurment_map_;
	map<int, shared_ptr<Measurement> > end_measurment_map_;
    int number_of_probes_;
	vector<GuessStat> guess_measurement_bucket;
	MoveStat move_stat;
    int guess_time_;
	int current_start_monitor_;
	long double last_utility_;
	deque<double> rtt_history_;
	static constexpr size_t kHistorySize = 20;
	mutex monitor_mutex_;
	int on_next_start_bind_to_end_;
};

#endif
