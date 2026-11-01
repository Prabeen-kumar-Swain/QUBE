#include "engine/Engine.hpp"

#include <algorithm>  // std::sort, std::max, std::min
#include <cmath>      // std::sqrt

namespace qube {

// A small helper (not part of the Engine): keeps a number between low and high.
static int limit(int value, int low, int high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

// ===================================================================== creating and resetting
// In this file every function name starts with "Engine::" - it tells the compiler
// "this function belongs to the Engine class".

Engine::Engine() {
    reset();
}

Engine::Engine(Config config) {
    config_ = config;
    reset();
}

void Engine::reset() {
    now_ = 0.0;
    customers_.clear();
    for (int i = 0; i < 3; i++) {
        lanes_[i].clear();
    }

    int count = config_.counterCount;
    if (count < 1) count = 1;
    config_.homeClasses.resize(count, 1);   // one home class per counter (new ones default to P1)

    counters_.clear();
    for (int i = 0; i < count; i++) {
        Counter counter;
        counter.id = i;
        counter.homeClass = limit(config_.homeClasses[i], 1, 3);
        counters_.push_back(counter);
    }
}

// ===================================================================== customers

int Engine::addCustomer(int priority, double serviceTime) {
    Customer c;
    c.id = (int)customers_.size() + 1;
    c.priority = limit(priority, 1, 3);
    c.serviceTime = std::max(0.5, serviceTime);
    c.arrival = now_;
    customers_.push_back(c);

    // The customer joins the back of the lane for their priority and stays there until called.
    lanes_[c.priority - 1].push_back(c.id);
    return c.id;
}

const Customer& Engine::customer(int id) const {
    return customers_.at(id - 1);          // ids start at 1, the list starts at 0
}

const std::vector<int>& Engine::lane(int cls) const {
    return lanes_[limit(cls, 1, 3) - 1];
}

// ===================================================================== settings

void Engine::setAgingOverrideWait(double seconds) {
    if (seconds < 0.0) seconds = 0.0;
    config_.agingOverrideWait = seconds;
}

void Engine::setAgingInterval(double seconds) {
    if (seconds < 1.0) seconds = 1.0;
    config_.agingInterval = seconds;
}

void Engine::setStarvationThreshold(double seconds) {
    if (seconds < 1.0) seconds = 1.0;
    config_.starvationThreshold = seconds;
}

void Engine::setApproachTime(double seconds) {
    if (seconds < 0.5) seconds = 0.5;
    config_.approachTime = seconds;
}

void Engine::setCounterClass(int counterIndex, int cls) {
    Counter& counter = counters_.at(counterIndex);
    counter.homeClass = limit(cls, 1, 3);
    config_.homeClasses[counterIndex] = counter.homeClass;   // remembered even after reset()
}

void Engine::setCounterCount(int count) {
    config_.counterCount = limit(count, 1, 8);
    config_.homeClasses.resize(config_.counterCount, 1);
    reset();
}

// ===================================================================== small helpers

int Engine::frontOfLane(int cls) const {
    const std::vector<int>& l = lanes_[cls - 1];
    if (l.empty()) return 0;
    return l[0];
}

double Engine::waitOf(int id) const {
    return now_ - customer(id).arrival;
}

// Aging score = priority + waited / agingInterval.
// Everyone gains score at the same speed while waiting, so a P1 who has waited long enough
// can eventually beat a P3 who has just arrived.
double Engine::agingScore(int id) const {
    double interval = config_.agingInterval;
    if (interval <= 0.0) interval = 1.0;
    return customer(id).priority + waitOf(id) / interval;
}

bool Engine::anyWaiting() const {
    for (int i = 0; i < 3; i++) {
        if (!lanes_[i].empty()) return true;
    }
    return false;
}

void Engine::removeFromLane(int id) {
    std::vector<int>& l = lanes_[customer(id).priority - 1];
    for (int i = 0; i < (int)l.size(); i++) {
        if (l[i] == id) {
            l.erase(l.begin() + i);
            return;
        }
    }
}

// ===================================================================== the selection rules
// Every rule answers one question: "WHICH waiting customer should this counter take?"
// They only read data (const) and return a customer id, or 0 if nobody is waiting.

int Engine::peekNext(int counterIndex) const {
    return pickFor(counters_.at(counterIndex));
}

int Engine::pickFor(const Counter& counter) const {
    if (config_.policy == Policy::FCFS) return pickOldestFront();
    if (config_.policy == Policy::SJF) return pickShortestJob();
    if (config_.policy == Policy::Priority) return pickByPriority(counter);
    return pickByAging(counter);           // Policy::Aging
}

// FCFS: whoever arrived first, looking at the three lane fronts.
int Engine::pickOldestFront() const {
    int best = 0;
    for (int cls = 1; cls <= 3; cls++) {
        int front = frontOfLane(cls);
        if (front == 0) continue;
        if (best == 0) {
            best = front;
        } else if (customer(front).arrival < customer(best).arrival) {
            best = front;
        } else if (customer(front).arrival == customer(best).arrival && front < best) {
            best = front;                  // same arrival time: the smaller id wins
        }
    }
    return best;
}

// SJF: the shortest service time among EVERYONE waiting (not only the fronts).
int Engine::pickShortestJob() const {
    int best = 0;
    for (int cls = 1; cls <= 3; cls++) {
        const std::vector<int>& l = lanes_[cls - 1];
        for (int i = 0; i < (int)l.size(); i++) {
            int id = l[i];
            if (best == 0) {
                best = id;
            } else if (customer(id).serviceTime < customer(best).serviceTime) {
                best = id;
            } else if (customer(id).serviceTime == customer(best).serviceTime && id < best) {
                best = id;
            }
        }
    }
    return best;
}

// Priority: own lane first; if it is empty, the highest class that has someone waiting.
int Engine::pickByPriority(const Counter& counter) const {
    int homeFront = frontOfLane(limit(counter.homeClass, 1, 3));
    if (homeFront != 0) return homeFront;

    for (int cls = 3; cls >= 1; cls--) {
        int front = frontOfLane(cls);
        if (front != 0) return front;
    }
    return 0;
}

// Aging (the default policy) has three stages. The first one that finds someone wins.
int Engine::pickByAging(const Counter& counter) const {
    int best = 0;
    double bestScore = -1e18;

    // Stage 1 - OVERRIDE: a lane front that has already waited long enough may be taken
    // by ANY counter. If several qualify, the one with the highest aging score goes first.
    for (int cls = 3; cls >= 1; cls--) {
        int front = frontOfLane(cls);
        if (front == 0) continue;
        if (waitOf(front) < config_.agingOverrideWait) continue;   // has not waited long enough
        double score = agingScore(front);
        if (score > bestScore) {
            bestScore = score;
            best = front;
        }
    }
    if (best != 0) return best;

    // Stage 2 - HOME LANE: take the front customer of this counter's own lane.
    int homeFront = frontOfLane(limit(counter.homeClass, 1, 3));
    if (homeFront != 0) return homeFront;

    // Stage 3 - BORROW: the home lane is empty, so help another lane: best aging score wins.
    for (int cls = 3; cls >= 1; cls--) {
        int front = frontOfLane(cls);
        if (front == 0) continue;
        double score = agingScore(front);
        if (score > bestScore) {
            bestScore = score;
            best = front;
        }
    }
    return best;
}

// ===================================================================== moving time forward

void Engine::step(double seconds) {
    // Time is cut into slices of at most 0.05 s, so the result does not depend on frame rate.
    const double maxSlice = 0.05;
    while (seconds > 1e-9) {
        double slice = std::min(seconds, maxSlice);
        advance(slice);
        seconds -= slice;
    }
}

void Engine::advance(double seconds) {
    now_ += seconds;
    for (int i = 0; i < (int)counters_.size(); i++) {
        updateCounter(counters_[i], seconds);
    }
    dispatch();                            // idle counters may now take new customers
}

// Counts down one counter's timer and moves it to its next phase when the timer reaches zero.
void Engine::updateCounter(Counter& counter, double seconds) {
    if (counter.phase == CounterPhase::Idle) return;

    counter.occupiedTime += seconds;
    counter.timer -= seconds;
    if (counter.timer > 1e-9) return;      // phase not finished yet (tiny tolerance for rounding)

    Customer& c = customers_[counter.customerId - 1];
    if (counter.phase == CounterPhase::Approaching) {
        // The customer reached the desk: service begins.
        counter.phase = CounterPhase::Serving;
        counter.timer = c.serviceTime;
        c.state = CustomerState::Serving;
        c.startedAt = now_;
    } else {
        // Service is over: the customer is done and the counter is free again.
        c.state = CustomerState::Done;
        c.finishedAt = now_;
        counter.phase = CounterPhase::Idle;
        counter.customerId = 0;
        counter.timer = 0.0;
        counter.served++;
        if (c.priority != counter.homeClass) counter.servedForeign++;   // it "borrowed" this customer
    }
}

// ===================================================================== assigning customers

// Which idle counter acts next?
//  - With Priority and Aging, a counter whose own lane has someone waiting goes first.
//  - Otherwise (and between equals) the counter that has worked the least goes first,
//    so the work is shared evenly.
int Engine::chooseIdleCounter() const {
    bool homeAware = (config_.policy == Policy::Priority || config_.policy == Policy::Aging);
    int best = -1;
    bool bestHasHomeWork = false;

    for (int i = 0; i < (int)counters_.size(); i++) {
        const Counter& counter = counters_[i];
        if (counter.phase != CounterPhase::Idle) continue;

        bool hasHomeWork = homeAware && !lanes_[counter.homeClass - 1].empty();
        bool better = false;
        if (best == -1) {
            better = true;
        } else if (hasHomeWork && !bestHasHomeWork) {
            better = true;
        } else if (hasHomeWork == bestHasHomeWork &&
                   counter.occupiedTime < counters_[best].occupiedTime - 1e-9) {
            better = true;
        }
        if (better) {
            best = i;
            bestHasHomeWork = hasHomeWork;
        }
    }
    return best;
}

void Engine::dispatch() {
    while (anyWaiting()) {
        int counterIndex = chooseIdleCounter();
        if (counterIndex == -1) break;                 // every counter is busy

        Counter& counter = counters_[counterIndex];
        int id = pickFor(counter);
        if (id == 0) break;

        removeFromLane(id);
        Customer& c = customers_[id - 1];
        c.state = CustomerState::Approaching;
        c.calledAt = now_;
        c.counter = counter.id;

        counter.phase = CounterPhase::Approaching;
        counter.customerId = id;
        counter.timer = config_.approachTime;          // time to walk from the lane to the desk
    }
}

// ===================================================================== statistics

Stats Engine::stats() const {
    Stats s;
    s.total = (int)customers_.size();
    s.elapsed = now_;

    double waitSum = 0.0;                  // waits of customers who were already called
    int waitCount = 0;
    double priorityWaitSum[3] = {0.0, 0.0, 0.0};
    std::vector<double> waits;             // one wait per customer (for the fairness numbers)

    for (int i = 0; i < (int)customers_.size(); i++) {
        const Customer& c = customers_[i];

        // A customer's wait = time from arrival until called. If still waiting: the wait so far.
        double wait = 0.0;
        if (c.state == CustomerState::Waiting) {
            s.waiting++;
            wait = now_ - c.arrival;
        } else if (c.state == CustomerState::Done) {
            s.completed++;
        } else {
            s.inService++;
        }
        if (c.calledAt >= 0.0) {
            wait = c.calledAt - c.arrival;
            waitSum += wait;
            waitCount++;
        }

        if (wait > s.longestWait) s.longestWait = wait;
        waits.push_back(wait);

        int p = limit(c.priority, 1, 3) - 1;               // 0, 1 or 2
        if (wait > config_.starvationThreshold) {
            s.starved++;
            s.starvedByPriority[p]++;
        }
        priorityWaitSum[p] += wait;
        s.countByPriority[p]++;
    }

    if (waitCount > 0) s.avgWait = waitSum / waitCount;
    for (int p = 0; p < 3; p++) {
        if (s.countByPriority[p] > 0) {
            s.avgWaitByPriority[p] = priorityWaitSum[p] / s.countByPriority[p];
        }
    }
    if (now_ > 1.0) s.throughputPerMin = s.completed / (now_ / 60.0);

    double utilizationSum = 0.0;
    for (int i = 0; i < (int)counters_.size(); i++) {
        utilizationSum += utilization(i);
    }
    if (!counters_.empty()) s.avgUtilization = utilizationSum / counters_.size();

    fillFairness(s, waits);
    return s;
}

// Jain's fairness index:  J = (sum of x)^2 / (n * sum of x^2).   1.0 means "all values are equal".
void Engine::fillFairness(Stats& s, std::vector<double> waits) const {
    if (!waits.empty()) {
        double sum = 0.0;
        double sumOfSquares = 0.0;
        for (int i = 0; i < (int)waits.size(); i++) {
            sum += waits[i];
            sumOfSquares += waits[i] * waits[i];
        }
        double n = (double)waits.size();
        if (sumOfSquares > 1e-9) s.jainIndex = (sum * sum) / (n * sumOfSquares);

        double variance = sumOfSquares / n - (sum / n) * (sum / n);
        if (variance < 0.0) variance = 0.0;
        s.waitStdDev = std::sqrt(variance);

        std::sort(waits.begin(), waits.end());              // smallest first
        int index = (int)(0.9 * (waits.size() - 1));
        s.waitP90 = waits[index];
    }

    // How much longer do normal (P1) customers wait than urgent (P3) ones?
    if (s.countByPriority[0] > 0 && s.countByPriority[2] > 0 && s.avgWaitByPriority[2] > 0.05) {
        s.priorityGap = s.avgWaitByPriority[0] / s.avgWaitByPriority[2];
    }

    // Is the work spread evenly over the counters? (same formula, applied to busy time)
    double sum = 0.0;
    double sumOfSquares = 0.0;
    for (int i = 0; i < (int)counters_.size(); i++) {
        sum += counters_[i].occupiedTime;
        sumOfSquares += counters_[i].occupiedTime * counters_[i].occupiedTime;
    }
    if (sumOfSquares > 1e-9) {
        s.counterBalance = (sum * sum) / (counters_.size() * sumOfSquares);
    }
}

double Engine::utilization(int counterIndex) const {
    if (now_ < 1e-6) return 0.0;
    const Counter& counter = counters_.at(counterIndex);
    return std::min(1.0, counter.occupiedTime / now_);
}

} // namespace qube
