// QUBE - Queue-Based Unified Balancing Engine
// The scheduling "brain". Plain C++ with no graphics, so it can be tested on its own.
//
// THE BIG IDEA
//   Waiting customers stand in three lanes, one per priority: P1, P2, P3.
//   A customer never changes lane. Every counter (desk) has a "home" class.
//   When a counter is free, the Engine looks ONLY at the front customer of each lane
//   and applies the selection rule of the chosen Policy to decide who goes next.
//
// HOW TO READ THIS FILE (beginner notes)
//   struct   = a bundle of related data (a Customer, a Counter ...).
//   class    = a bundle of data AND the functions that work on that data (the Engine).
//   public   = the outside world (graphics code, tests) may use it.
//   private  = internal helpers and data; only the Engine itself may touch them.
//   const    = written after a function name: "this function only reads, it never changes anything".
//   const X& = "look at X without copying it" (a read-only reference).
//   enum class = a named list of choices, written like  Policy::Aging.
#pragma once
#include <vector>

namespace qube {

// ---------------------------------------------------------------- choices

// How a free counter picks its next customer.
enum class Policy {
    FCFS,     // oldest front customer overall (ignores lanes and home classes)
    Priority, // home lane first; if empty, the highest class that has someone waiting
    SJF,      // shortest service time among EVERYONE waiting (ignores home classes)
    Aging     // home lane first, borrow by aging score, and a long wait overrides everything
};

enum class CustomerState { Waiting, Approaching, Serving, Done };
enum class CounterPhase { Idle, Approaching, Serving };

// ---------------------------------------------------------------- plain data

struct Customer {
    int id = 0;                // 1, 2, 3 ... in order of arrival
    int priority = 1;          // 1 = normal, 2 = high, 3 = urgent
    double arrival = 0.0;      // simulated time of arrival (seconds)
    double serviceTime = 0.0;  // how long the service itself takes (seconds)
    double calledAt = -1.0;    // when a counter called this customer (-1 = not yet)
    double startedAt = -1.0;   // when service started
    double finishedAt = -1.0;  // when service ended
    CustomerState state = CustomerState::Waiting;
    int counter = -1;          // index of the counter that took this customer
};

struct Counter {
    int id = 0;                          // position in the list: 0, 1, 2 ...
    CounterPhase phase = CounterPhase::Idle;
    int customerId = 0;                  // who is being handled (0 = nobody)
    double timer = 0.0;                  // seconds left in the current phase
    double occupiedTime = 0.0;           // walking + serving time so far (for utilization)
    int served = 0;                      // customers finished
    int servedForeign = 0;               // finished customers whose class was not this counter's home class
    int homeClass = 1;                   // this counter's main class (1, 2 or 3)
};

// Settings that can be changed by the user.
struct Config {
    int counterCount = 4;
    double approachTime = 3.0;           // seconds a called customer needs to walk to the desk
    Policy policy = Policy::FCFS;
    double agingInterval = 15.0;         // seconds of waiting that are worth +1 priority level
    double starvationThreshold = 60.0;   // a wait longer than this counts as starvation
    std::vector<int> homeClasses = {3, 2, 1, 1};  // home class of each counter
    double agingOverrideWait = 6.0;      // Aging policy: a lane front that waited this long may be taken by ANY counter
};

// Results that the user interface displays.
struct Stats {
    int waiting = 0;                     // people in the lanes
    int inService = 0;                   // walking to a desk or being served
    int completed = 0;
    int total = 0;
    double avgWait = 0.0;                // average time from arrival until a counter called them
    double longestWait = 0.0;
    double throughputPerMin = 0.0;
    double elapsed = 0.0;
    int starved = 0;                     // customers whose wait went over the threshold
    double avgUtilization = 0.0;         // 0..1 averaged over all counters

    // Index 0 = P1, 1 = P2, 2 = P3. A customer who is still waiting counts with their wait so far.
    double avgWaitByPriority[3] = {0.0, 0.0, 0.0};
    int countByPriority[3] = {0, 0, 0};
    int starvedByPriority[3] = {0, 0, 0};

    // Fairness numbers
    double jainIndex = 1.0;              // Jain's index of all waits: 1.0 = everyone waited equally long
    double waitStdDev = 0.0;             // how spread out the waits are
    double waitP90 = 0.0;                // 90 % of customers waited this long or less
    double priorityGap = 0.0;            // average P1 wait divided by average P3 wait (0 = not available)
    double counterBalance = 1.0;         // Jain's index of the counters' busy time: 1.0 = perfectly even
};

// ---------------------------------------------------------------- the Engine

class Engine {
public:
    // --- creating an engine
    Engine();                            // default settings
    Engine(Config config);               // your own settings

    // --- running the simulation
    void reset();                        // start over (keeps the settings)
    int addCustomer(int priority, double serviceTime);   // returns the new customer's id
    void step(double seconds);           // let simulated time pass

    // --- changing settings while running
    void setPolicy(Policy policy) { config_.policy = policy; }
    void setAgingOverrideWait(double seconds);
    void setAgingInterval(double seconds);
    void setStarvationThreshold(double seconds);
    void setApproachTime(double seconds);       // applies to customers called from now on
    void setCounterClass(int counterIndex, int cls);   // change a counter's home class (1..3)
    void setCounterCount(int count);            // 1..8 counters. This RESTARTS the simulation!

    // --- reading the current state
    double now() const { return now_; }
    const Config& config() const { return config_; }
    const std::vector<Customer>& customers() const { return customers_; }
    const std::vector<Counter>& counters() const { return counters_; }
    const Customer& customer(int id) const;            // look up a customer by id
    const std::vector<int>& lane(int cls) const;       // ids waiting in one lane (cls 1..3), oldest first
    int peekNext(int counterIndex) const;              // who WOULD this counter take now? (0 = nobody)
    Stats stats() const;
    double utilization(int counterIndex) const;        // 0..1

private:
    // --- time and assignment
    void advance(double seconds);                      // one small time slice
    void updateCounter(Counter& counter, double seconds);
    void dispatch();                                   // give waiting customers to idle counters
    int chooseIdleCounter() const;                     // which idle counter acts next? (-1 = none)

    // --- the selection rules: each returns a customer id, or 0 if nobody is waiting
    int pickFor(const Counter& counter) const;
    int pickOldestFront() const;                       // FCFS
    int pickShortestJob() const;                       // SJF
    int pickByPriority(const Counter& counter) const;  // Priority
    int pickByAging(const Counter& counter) const;     // Aging

    // --- small helpers
    int frontOfLane(int cls) const;                    // first id in a lane, or 0 if the lane is empty
    double waitOf(int id) const;                       // how long this customer has waited so far
    double agingScore(int id) const;                   // priority + waited / agingInterval
    bool anyWaiting() const;
    void removeFromLane(int id);
    void fillFairness(Stats& stats, std::vector<double> waits) const;

    // --- data
    Config config_;
    double now_ = 0.0;                                 // the simulated clock (seconds)
    std::vector<Customer> customers_;                  // every customer ever added; id = position + 1
    std::vector<Counter> counters_;
    std::vector<int> lanes_[3];                        // lanes_[0] = P1 lane, [1] = P2, [2] = P3 (ids only)
};

} // namespace qube
