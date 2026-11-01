// Minimal self-checking tests. Run: build/qube_tests  (exit code 0 = all passed)
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "engine/Engine.hpp"

using namespace qube;

static int g_failed = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);     \
            ++g_failed;                                                      \
        }                                                                    \
    } while (0)

static void testFcfsOrder() {
    Config cfg; cfg.counterCount = 1; cfg.approachTime = 1.0;
    Engine e(cfg);
    int a = e.addCustomer(1, 3.0), b = e.addCustomer(3, 1.0), c = e.addCustomer(2, 2.0);
    CHECK(e.peekNext(0) == a); // oldest overall, ignoring lanes
    e.step(60.0);
    // FCFS ignores priority and service time: served strictly in arrival order
    CHECK(e.customer(a).calledAt < e.customer(b).calledAt);
    CHECK(e.customer(b).calledAt < e.customer(c).calledAt);
    CHECK(e.stats().completed == 3);
}

static void testParallelCounters() {
    Config cfg; cfg.counterCount = 2; cfg.approachTime = 1.0;
    Engine e(cfg);
    for (int i = 0; i < 4; ++i) e.addCustomer(1, 5.0);
    e.step(0.1);
    CHECK(e.counters()[0].phase == CounterPhase::Approaching);
    CHECK(e.counters()[1].phase == CounterPhase::Approaching);
    CHECK(e.customer(1).state == CustomerState::Approaching);
    CHECK(e.customer(2).state == CustomerState::Approaching);
    CHECK(e.customer(3).state == CustomerState::Waiting);
    CHECK(e.stats().waiting == 2 && e.stats().inService == 2);
}

static void testTiming() {
    Config cfg; cfg.counterCount = 1; cfg.approachTime = 2.0;
    Engine e(cfg);
    int id = e.addCustomer(1, 5.0);
    e.step(30.0);
    const Customer& c = e.customer(id);
    CHECK(c.state == CustomerState::Done);
    CHECK(std::fabs((c.finishedAt - c.startedAt) - 5.0) < 0.06);
    CHECK(std::fabs((c.startedAt - c.calledAt) - 2.0) < 0.06);
}

static void testStatsAndReset() {
    Config cfg; cfg.counterCount = 1; cfg.approachTime = 1.0;
    Engine e(cfg);
    e.addCustomer(1, 4.0);
    e.addCustomer(1, 4.0);
    e.step(10.0); // first done at ~5s, second called at ~5s
    Stats s = e.stats();
    CHECK(s.completed == 1);
    CHECK(s.longestWait >= 4.9);
    CHECK(e.utilization(0) > 0.95);
    e.reset();
    CHECK(e.customers().empty() && e.now() == 0.0 && e.stats().total == 0);
}

static void testStepSizeIndependence() {
    Config cfg; cfg.counterCount = 2; cfg.approachTime = 1.5;
    Engine a(cfg), b(cfg);
    for (int i = 0; i < 6; ++i) { a.addCustomer(1, 3.0 + i); b.addCustomer(1, 3.0 + i); }
    a.step(40.0);
    for (int i = 0; i < 4000; ++i) b.step(0.01);
    CHECK(a.stats().completed == b.stats().completed);
    CHECK(std::fabs(a.stats().avgWait - b.stats().avgWait) < 0.1);
}


static Config oneCounter(Policy p) {
    Config cfg; cfg.counterCount = 1; cfg.approachTime = 1.0; cfg.policy = p;
    return cfg;
}

static void testPriorityOrder() {
    Engine e(oneCounter(Policy::Priority));
    int p1a = e.addCustomer(1, 3.0), p3 = e.addCustomer(3, 3.0), p2 = e.addCustomer(2, 3.0), p1b = e.addCustomer(1, 3.0);
    CHECK(e.peekNext(0) == p3); // counter 0 is a P3 counter: home lane first
    e.step(60.0);
    CHECK(e.customer(p3).calledAt < e.customer(p2).calledAt);
    CHECK(e.customer(p2).calledAt < e.customer(p1a).calledAt);
    CHECK(e.customer(p1a).calledAt < e.customer(p1b).calledAt);
}

static void testSjfOrder() {
    Engine e(oneCounter(Policy::SJF));
    int longJob = e.addCustomer(3, 9.0), mid = e.addCustomer(1, 5.0), quick = e.addCustomer(1, 2.0);
    CHECK(e.peekNext(0) == quick);
    e.step(60.0);
    CHECK(e.customer(quick).calledAt < e.customer(mid).calledAt);
    CHECK(e.customer(mid).calledAt < e.customer(longJob).calledAt);
}

static void testPolicySwitchReorders() {
    Engine e(oneCounter(Policy::FCFS));
    int a = e.addCustomer(1, 5.0), b = e.addCustomer(3, 5.0);
    CHECK(e.peekNext(0) == a);
    e.setPolicy(Policy::Priority);
    CHECK(e.peekNext(0) == b);
}

// A steady stream of P3 customers keeps one P3 always waiting. Pure priority never
// serves the early P1; aging does.
static void feedUrgentStream(Engine& e) {
    e.addCustomer(1, 2.0);                 // id 1: the P1 under test
    for (int i = 0; i < 120; ++i) {        // 60 s in 0.5 s ticks
        if (i % 3 == 0) e.addCustomer(3, 2.0);
        e.step(0.5);
    }
}

static void testStarvationAndAging() {
    Config pc = oneCounter(Policy::Priority);
    pc.starvationThreshold = 40.0;
    Engine pr(pc);
    feedUrgentStream(pr);
    CHECK(pr.customer(1).state == CustomerState::Waiting); // starved
    CHECK(pr.stats().starved >= 1);

    Config ac = oneCounter(Policy::Aging);
    ac.agingInterval = 5.0;
    ac.agingOverrideWait = 30.0;
    Engine ag(ac);
    feedUrgentStream(ag);
    CHECK(ag.customer(1).state != CustomerState::Waiting); // aging rescued it
    // P1 outranks P3s that arrive >10 s after it, but the P3s already queued go first (~24 s)
    CHECK(ag.customer(1).calledAt > 10.0 && ag.customer(1).calledAt < 40.0);
}

static void testPriorityWaitsLessOnAverage() {
    // 2 counters, mixed load: under Priority, P3 customers must wait less than P1 on average.
    Config cfg; cfg.counterCount = 2; cfg.approachTime = 1.0; cfg.policy = Policy::Priority;
    Engine e(cfg);
    for (int i = 0; i < 90; ++i) {
        if (i % 2 == 0) e.addCustomer(1 + (i / 2) % 3, 4.0); // P1,P2,P3,P1,...
        e.step(1.0);
    }
    e.step(400.0);
    Stats s = e.stats();
    CHECK(s.avgWaitByPriority[2] < s.avgWaitByPriority[1]);
    CHECK(s.avgWaitByPriority[1] < s.avgWaitByPriority[0]);
    CHECK(s.priorityGap > 1.0);
}

static void testFairnessMetrics() {
    Config cfg; cfg.counterCount = 1; cfg.approachTime = 1.0; cfg.policy = Policy::FCFS;
    Engine e(cfg);
    for (int i = 0; i < 3; ++i) e.addCustomer(1, 4.0); // waits ~0, 5, 10
    e.step(60.0);
    Stats s = e.stats();
    CHECK(s.jainIndex > 0.58 && s.jainIndex < 0.63);   // (15^2) / (3 * 125) = 0.6
    CHECK(s.waitStdDev > 4.0 && s.waitStdDev < 4.2);

    Engine f(cfg);
    for (int i = 0; i < 11; ++i) f.addCustomer(1, 4.0); // waits ~0, 5, ... 50
    f.step(200.0);
    CHECK(f.stats().waitP90 > 44.9 && f.stats().waitP90 < 45.2);

    Engine g(Config{2, 1.0, Policy::FCFS});            // nobody waits -> perfectly fair
    g.addCustomer(1, 3.0);
    g.step(0.1);
    CHECK(g.stats().jainIndex > 0.99);
}

static void testStarvedByPriority() {
    Config pc = oneCounter(Policy::Priority);
    pc.starvationThreshold = 40.0;
    Engine pr(pc);
    feedUrgentStream(pr);
    CHECK(pr.stats().starvedByPriority[0] >= 1);
    CHECK(pr.stats().starvedByPriority[2] == 0 || pr.stats().starved >= pr.stats().starvedByPriority[0]);
}

static void testCounterLoadBalancing() {
    Config cfg; cfg.counterCount = 2; cfg.approachTime = 1.0;
    Engine e(cfg);
    for (int i = 0; i < 10; ++i) { // one customer at a time, so both counters are idle when called
        e.addCustomer(1, 3.0);
        e.step(10.0);
    }
    CHECK(e.counters()[0].served == 5 && e.counters()[1].served == 5);
    CHECK(e.stats().counterBalance > 0.99);
}

// ------------------------------------------------------------------ Stage 5: lanes + home counters
static Config fourCounters(Policy p) {
    Config cfg; cfg.counterCount = 4; cfg.approachTime = 1.0; cfg.policy = p; // homes {P3,P2,P1,P1}
    return cfg;
}

static void testLanesAreStable() {
    Engine e(fourCounters(Policy::Aging));
    int a = e.addCustomer(1, 5), b = e.addCustomer(3, 5), c = e.addCustomer(2, 5), d = e.addCustomer(1, 5);
    CHECK(e.lane(1).size() == 2 && e.lane(1)[0] == a && e.lane(1)[1] == d);
    CHECK(e.lane(2).size() == 1 && e.lane(2)[0] == c);
    CHECK(e.lane(3).size() == 1 && e.lane(3)[0] == b);
    const std::vector<int> p1Before = e.lane(1);
    e.addCustomer(3, 5); // a new urgent customer must NOT disturb the P1 lane
    CHECK(e.lane(1) == p1Before);
    CHECK(e.lane(3).size() == 2);
}

static void testHomeCountersTakeTheirOwnLane() {
    Engine e(fourCounters(Policy::Aging));
    int p1a = e.addCustomer(1, 20), p1b = e.addCustomer(1, 20), p2 = e.addCustomer(2, 20), p3 = e.addCustomer(3, 20);
    e.step(0.1);
    CHECK(e.customer(p3).counter == 0); // P3 counter
    CHECK(e.customer(p2).counter == 1); // P2 counter
    CHECK(e.customer(p1a).counter == 2 && e.customer(p1b).counter == 3); // the two P1 counters
    CHECK(e.lane(1).empty() && e.lane(2).empty() && e.lane(3).empty());
}

static void testIdleCountersBorrow() {
    Engine e(fourCounters(Policy::Aging));
    for (int i = 0; i < 3; ++i) e.addCustomer(1, 5); // only P1 customers
    e.step(0.1);
    CHECK(e.stats().waiting == 0);                    // nobody waits while a counter is idle
    CHECK(e.counters()[0].phase != CounterPhase::Idle || e.counters()[1].phase != CounterPhase::Idle);
    e.step(30.0);
    int foreign = 0;
    for (const Counter& k : e.counters()) foreign += k.servedForeign;
    CHECK(foreign == 1);                              // exactly one P1 customer was served by a P2/P3 counter
}

static void testBorrowUsesAgingScoreAndOverride() {
    Config cfg; cfg.counterCount = 1; cfg.approachTime = 1.0; cfg.policy = Policy::Aging;
    cfg.homeClasses = {3}; cfg.agingInterval = 15.0; cfg.agingOverrideWait = 30.0;
    Engine e(cfg);
    e.addCustomer(3, 500);          // keeps the only counter busy
    e.step(0.1);
    int p1 = e.addCustomer(1, 5);   // arrives at t = 0.1
    e.step(10.0);
    int p2 = e.addCustomer(2, 5);   // home lane (P3) is empty: borrow by score
    CHECK(e.peekNext(0) == p2);     // P2 score 2.0 beats P1 score 1 + 10/15 = 1.67
    e.step(25.0);                   // P1 has now waited ~35 s >= override wait
    CHECK(e.peekNext(0) == p1);     // aging override: the old P1 jumps ahead of the fresh P2
    e.setPolicy(Policy::Priority);
    CHECK(e.peekNext(0) == p2);     // plain priority would still prefer P2 (no aging)
}

// Every counter is a P3 counter and P3 customers keep arriving, so the P3 lane is never empty.
static void feedP3Overload(Engine& e) {
    e.addCustomer(1, 3.0);                 // id 1: the P1 under test
    e.addCustomer(3, 3.0);                 // keep the P3 lane non-empty from the very start
    e.addCustomer(3, 3.0);
    for (int i = 0; i < 120; ++i) {        // 60 s
        if (i % 2 == 0) e.addCustomer(3, 3.0);
        e.step(0.5);
    }
}

static void testAgingOverridePreventsLaneStarvation() {
    Config cfg; cfg.counterCount = 2; cfg.approachTime = 1.0; cfg.homeClasses = {3, 3};
    cfg.policy = Policy::Priority;
    Engine pr(cfg);
    feedP3Overload(pr);
    CHECK(pr.customer(1).state == CustomerState::Waiting); // starves without aging

    cfg.policy = Policy::Aging;
    cfg.agingOverrideWait = 30.0;
    Engine ag(cfg);
    feedP3Overload(ag);
    CHECK(ag.customer(1).state != CustomerState::Waiting);
    CHECK(ag.customer(1).calledAt > 29.0 && ag.customer(1).calledAt < 40.0); // right after the 30 s override
}

static void testCounterReassignmentPersists() {
    Engine e(fourCounters(Policy::Aging));
    CHECK(e.counters()[0].homeClass == 3 && e.counters()[3].homeClass == 1);
    e.setCounterClass(3, 2);
    CHECK(e.counters()[3].homeClass == 2);
    e.reset();
    CHECK(e.counters()[3].homeClass == 2); // survives reset
    e.addCustomer(2, 20);
    e.addCustomer(2, 20);
    e.step(0.1);
    CHECK(e.customer(1).counter == 1 && e.customer(2).counter == 3); // both P2 counters took a P2
}

static void testFcfsIgnoresHomeClasses() {
    Engine e(fourCounters(Policy::FCFS));
    int a = e.addCustomer(1, 20);
    e.step(0.1);
    e.step(0.1);
    // FCFS: the first free counter (least worked, lowest index) takes it regardless of class
    CHECK(e.customer(a).counter == 0);
}

static void testLanesGiveHigherClassesShorterWaits() {
    // default 4 counters {P3,P2,P1,P1}, Aging. Every 60 s a rush of 12 arrives: 6xP1, 4xP2, 2xP3.
    Config cfg; cfg.counterCount = 4; cfg.approachTime = 3.0; cfg.policy = Policy::Aging;
    Engine e(cfg);
    for (int round = 0; round < 30; ++round) {
        for (int i = 0; i < 12; ++i) e.addCustomer(i < 6 ? 1 : (i < 10 ? 2 : 3), 6.0 + (i % 4) * 1.5);
        e.step(60.0);
    }
    Stats s = e.stats();
    CHECK(s.avgWaitByPriority[2] < s.avgWaitByPriority[1]);
    CHECK(s.avgWaitByPriority[1] < s.avgWaitByPriority[0]);
    CHECK(s.starved == 0);
}

static void testLiveSettings() {
    // aging override wait changes who a counter picks, immediately
    Config cfg; cfg.counterCount = 1; cfg.approachTime = 1.0; cfg.policy = Policy::Aging;
    cfg.homeClasses = {3}; cfg.agingOverrideWait = 100.0;
    Engine e(cfg);
    e.addCustomer(3, 500);
    e.step(0.1);
    int p1 = e.addCustomer(1, 5);
    e.step(10.0);
    int p2 = e.addCustomer(2, 5);
    CHECK(e.peekNext(0) == p2);          // override far away: borrow by score picks P2
    e.setAgingOverrideWait(5.0);
    CHECK(e.peekNext(0) == p1);          // P1 has waited 10 s >= 5 s: override now picks it
    e.setAgingOverrideWait(-3.0);
    CHECK(e.config().agingOverrideWait == 0.0);
    e.setStarvationThreshold(5.0);
    e.step(1.0);
    CHECK(e.stats().starved >= 1);       // P1 waited > 5 s
    e.setApproachTime(0.0);
    CHECK(e.config().approachTime == 0.5);
}

static void testCounterCountChange() {
    Engine e(fourCounters(Policy::Aging));
    e.addCustomer(1, 5);
    e.setCounterCount(6);
    CHECK(e.counters().size() == 6 && e.customers().empty() && e.now() == 0.0); // restarted
    CHECK(e.counters()[0].homeClass == 3 && e.counters()[1].homeClass == 2 && e.counters()[5].homeClass == 1);
    e.setCounterCount(2);
    CHECK(e.counters().size() == 2 && e.counters()[0].homeClass == 3 && e.counters()[1].homeClass == 2);
    e.setCounterCount(99);
    CHECK(e.counters().size() == 8);
    e.setCounterCount(0);
    CHECK(e.counters().size() == 1);
}

int main() {
    testFcfsOrder();
    testParallelCounters();
    testTiming();
    testStatsAndReset();
    testStepSizeIndependence();
    testPriorityOrder();
    testSjfOrder();
    testPolicySwitchReorders();
    testStarvationAndAging();
    testPriorityWaitsLessOnAverage();
    testFairnessMetrics();
    testStarvedByPriority();
    testCounterLoadBalancing();
    testLanesAreStable();
    testHomeCountersTakeTheirOwnLane();
    testIdleCountersBorrow();
    testBorrowUsesAgingScoreAndOverride();
    testAgingOverridePreventsLaneStarvation();
    testCounterReassignmentPersists();
    testFcfsIgnoresHomeClasses();
    testLanesGiveHigherClassesShorterWaits();
    testLiveSettings();
    testCounterCountChange();
    if (g_failed == 0) std::printf("All engine tests passed.\n");
    return g_failed == 0 ? 0 : 1;
}
