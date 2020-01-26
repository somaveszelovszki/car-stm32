#include <micro/task/common.hpp>
#include <micro/utils/units.hpp>
#include <micro/utils/time.hpp>
#include <micro/utils/log.hpp>
#include <micro/utils/Line.hpp>
#include <micro/utils/trajectory.hpp>

#include <LinePattern.hpp>
#include <LabyrinthGraph.hpp>
#include <DetectedLines.hpp>
#include <ControlData.hpp>
#include <cfg_board.h>
#include <cfg_car.hpp>
#include <cfg_track.hpp>
#include <globals.hpp>

#include <stm32f4xx_hal.h>
#include <stm32f4xx_hal_uart.h>

#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>

using namespace micro;

extern QueueHandle_t detectedLinesQueue;
extern QueueHandle_t controlQueue;

#define LOG_WAIT_DEBUG(...) { LOG_DEBUG(__VA_ARGS__); vTaskDelay(10); }
#define LOG_WAIT_ERROR(...) { LOG_ERROR(__VA_ARGS__); vTaskDelay(10); }

namespace {

vec<Segment, 5 * cfg::MAX_NUM_LAB_SEGMENTS> segments;               // The segments.
vec<Junction, 5 * cfg::MAX_NUM_LAB_SEGMENTS> junctions;             // The junctions - at most the number of segments.
vec<Connection, 5 * cfg::MAX_NUM_LAB_SEGMENTS * 2> connections;     // The connections - 2 times the number of junctions.

Segment *currentSeg = nullptr;
Connection *prevConn = nullptr;

struct {
    Segment *seg          = nullptr;
    Junction *lastJunc    = nullptr;
    Maneuver lastManeuver = { radian_t(0), Direction::CENTER };
    Trajectory trajectory = Trajectory(cfg::CAR_OPTO_CENTER_DIST);
} laneChange;

enum class Y_turnState {
    INACTIVE,
    FWD_LEFT1,
    PREPARE_BWD_RIGHT,
    BWD_RIGHT,
    PREPARE_FWD_LEFT2,
    FWD_LEFT2
};

struct {
    Y_turnState state = Y_turnState::INACTIVE;
    radian_t startOrientation;
    millisecond_t stateStartTime;
} y_turn;

millisecond_t endTime;

struct Route {
    static constexpr uint32_t MAX_LENGTH = cfg::MAX_NUM_LAB_SEGMENTS * 2;
    Segment *startSeg;
    Segment *lastSeg;
    vec<Connection*, MAX_LENGTH> connections;

    Route()
        : startSeg(nullptr)
        , lastSeg(nullptr) {}

    void append(Connection *c) {
        this->connections.push_back(c);
        this->lastSeg = c->getOtherSegment(this->lastSeg);
    }

    Connection* nextConnection() {
        Connection *conn = nullptr;
        if (this->connections.size()) {
            conn = this->connections[0];
            this->startSeg = conn->getOtherSegment(this->startSeg);
            this->connections.erase(this->connections.begin());
        }
        return conn;
    }

    Connection* lastConnection() const {
        return this->connections.size() > 0 ? this->connections[this->connections.size() - 1] : nullptr;
    }

    void reset() {
        this->startSeg = this->lastSeg = currentSeg;
        this->connections.clear();
    }
};

constexpr uint32_t MAX_NUM_ROUTES = 64;
vec<Route, MAX_NUM_ROUTES> routes;
Route plannedRoute;  // Planned route, when there is a given destination - e.g. given floating segment or the segment where the lane-change will happen

template <typename T, uint32_t N>
T* getNew(vec<T, N>& vec) {
    T *elem = nullptr;
    if (vec.push_back(T())) {
        elem = vec.back();
    } else {
        LOG_WAIT_ERROR("Pool empty, cannot get new element");
    }
    return elem;
}

Junction* findExistingJunction(const point2m& pos, radian_t inOri, radian_t outOri, uint8_t numInSegments, Direction inSegmentDir, uint8_t numOutSegments, bool& isValid) {
    constexpr meter_t MIN_JUNCTION_POS_ACCURACY = centimeter_t(100);

    Junction *result = nullptr;

    struct JunctionDist {
        Junction *junc = nullptr;
        meter_t dist = meter_t::infinity();
    };

    // FIRST:  closest junction to current position
    // SECOND: closest junction to current position with the correct topology
    std::pair<JunctionDist, JunctionDist> closest = {};

    for(Junction& j : junctions) {
        const meter_t dist = pos.distance(j.pos);

        if (dist < closest.first.dist) {
            closest.first.junc = &j;
            closest.first.dist = dist;
        }

        if (dist < closest.second.dist) {
            const Junction::segment_map::const_iterator inSegments = j.getSideSegments(inOri);

            if (inSegments != j.segments.end()) {
                LOG_WAIT_DEBUG("%fdeg (in) -> %fdeg: %d segments (current: %d)",
                    static_cast<degree_t>(inOri).get(), static_cast<degree_t>(inSegments->first).get(), (int32_t)inSegments->second.size(), (int32_t)numInSegments);
            } else {
                LOG_WAIT_ERROR("No segments found in direction %fdeg (in)", static_cast<degree_t>(inOri).get());
            }

            const Junction::segment_map::const_iterator outSegments = j.getSideSegments(outOri);

            if (outSegments != j.segments.end()) {
                LOG_WAIT_DEBUG("%fdeg (out) -> %fdeg: %d segments (current: %d)",
                    static_cast<degree_t>(outOri).get(), static_cast<degree_t>(outSegments->first).get(), (int32_t)outSegments->second.size(), (int32_t)numOutSegments);
            } else {
                LOG_WAIT_ERROR("No segments found in direction %fdeg (out)", static_cast<degree_t>(outOri).get());
            }

            if (inSegments != j.segments.end() && inSegments->second.size() == numInSegments) {
                if (outSegments != j.segments.end() && outSegments->second.size() == numOutSegments) {
                    closest.second.junc = &j;
                    closest.second.dist = dist;
                }
            }
        }
    }

    if (closest.second.dist < centimeter_t(120)) {
        // a junction at the right position and the correct topology has been found
        result = closest.second.junc;
        isValid = true;
    } else if (closest.first.dist < centimeter_t(80)) {
        // a junction at the right position but with incorrect topology has been found
        result = closest.first.junc;
        isValid = false;
    } else {
        // the junction has not been found
        result = nullptr;
        isValid = false;
    }

    return result;
}

bool getRoute(Route& result, const Segment *dest, const Junction *lastRouteJunc = nullptr, const Maneuver lastRouteManeuver = Maneuver()) {
    routes.clear();
    Route * const initialRoute = getNew(routes);
    initialRoute->startSeg = initialRoute->lastSeg = currentSeg;
    uint8_t depth = 0;
    Route *shortestRoute = nullptr;

    do {
        while (routes.size() > 0 && !shortestRoute) {
            Route *currentRoute = &routes[0];
            const Connection *lastRouteConn = currentRoute->lastConnection();
            if (!lastRouteConn) {
                lastRouteConn = prevConn;
            }

            const Maneuver lastManeuver = lastRouteConn ? lastRouteConn->getManeuver(currentRoute->lastSeg) : Maneuver();

            for (Connection *c : currentRoute->lastSeg->edges) {

                // does not permit going backwards or navigating through dead-end segments
                if ((!lastRouteConn ||
                    c->junction != lastRouteConn->junction ||
                    c->getManeuver(currentRoute->lastSeg) != lastManeuver) ||
                    currentRoute->lastSeg->isDeadEnd) {

                    if (routes.size() >= MAX_NUM_ROUTES) {
                        LOG_WAIT_ERROR("routes.size() >= MAX_NUM_ROUTES");
                        break;
                    }
                    Route *newRoute = getNew(routes);
                    *newRoute = *currentRoute;
                    newRoute->append(c);

                    // if we reached the destination, the shortest route has been found
                    if (newRoute->lastSeg == dest && (!lastRouteJunc ||
                        (lastRouteJunc == newRoute->lastConnection()->junction &&
                        lastRouteManeuver == newRoute->lastConnection()->getManeuver(newRoute->lastSeg)))) {
                        shortestRoute = newRoute;
                        break;
                    }
                }
            }

            routes.erase(routes.begin());
        }
    } while(!shortestRoute && routes.size() > 0 && ++depth < Route::MAX_LENGTH);

    if (shortestRoute) {
        result = *shortestRoute;
    }

    return !!shortestRoute;
}

vec<Segment*, cfg::MAX_NUM_LAB_SEGMENTS> getFloatingSegments() {
    vec<Segment*, cfg::MAX_NUM_LAB_SEGMENTS> floatingSegments;
    for (Segment& seg : segments) {
        if (seg.isActive && seg.isFloating()) {
            floatingSegments.push_back(&seg);
        }
    }
    return floatingSegments;
}

bool getRoute() {
    bool success = false;
    plannedRoute.reset();

    vec<Segment*, cfg::MAX_NUM_LAB_SEGMENTS> floatingSegments = getFloatingSegments();

    if (floatingSegments.size()) {    // if there are still floating segments in the graph, chooses the closest one as destination
        Route route;
        for (Segment *seg : floatingSegments) {
            if ((success |= getRoute(route, seg)) && (!plannedRoute.connections.size() || (plannedRoute.connections.size() > route.connections.size()))) {
                plannedRoute = route;
            }
        }
    } else if (laneChange.seg && laneChange.lastJunc) { // no floating segments in the graph, new destination will be the lane change segment
        success = getRoute(plannedRoute, laneChange.seg, laneChange.lastJunc, laneChange.lastManeuver);
    }

    return success;
}

Segment* createNewSegment() {
    Segment *seg = getNew(segments);
    if (seg) {
        seg->name      = 'A' + static_cast<char>(segments.size() - 1);
        seg->length    = meter_t(0);
        seg->isDeadEnd = false;
        seg->isActive  = true;
    }
    return seg;
}

void addSegments(Junction * const junc, radian_t inOri, radian_t outOri, uint8_t numInSegments, Direction inSegmentDir, uint8_t numOutSegments) {

    junc->addSegment(currentSeg, inOri, inSegmentDir);

    switch (numInSegments) {
    case 2:
        junc->addSegment(createNewSegment(), inOri, inSegmentDir == Direction::LEFT ? Direction::RIGHT : Direction::LEFT);
        break;
    case 3:
        switch (inSegmentDir) {
        case Direction::LEFT:
            junc->addSegment(createNewSegment(), inOri, Direction::RIGHT);
            junc->addSegment(createNewSegment(), inOri, Direction::CENTER);
            break;
        case Direction::CENTER:
            junc->addSegment(createNewSegment(), inOri, Direction::RIGHT);
            junc->addSegment(createNewSegment(), inOri, Direction::LEFT);
            break;
        case Direction::RIGHT:
            junc->addSegment(createNewSegment(), inOri, Direction::CENTER);
            junc->addSegment(createNewSegment(), inOri, Direction::LEFT);
            break;
        }
        break;
    }

    switch (numOutSegments) {
    case 1:
        junc->addSegment(createNewSegment(), outOri, Direction::CENTER);
        break;
    case 2:
        junc->addSegment(createNewSegment(), outOri, Direction::RIGHT);
        junc->addSegment(createNewSegment(), outOri, Direction::LEFT);
        break;
    case 3:
        junc->addSegment(createNewSegment(), outOri, Direction::RIGHT);
        junc->addSegment(createNewSegment(), outOri, Direction::CENTER);
        junc->addSegment(createNewSegment(), outOri, Direction::LEFT);
        break;
    }
}

void addConnection(Connection * const conn, Segment * const seg1, const Maneuver& maneuver1, Segment * const seg2, const Maneuver& maneuver2) {
    conn->node1 = seg1;
    conn->node2 = seg2;
    conn->maneuver1 = maneuver1;
    conn->maneuver2 = maneuver2;
    seg1->edges.push_back(conn);
    seg2->edges.push_back(conn);
}

vec<Connection*, 2> getConnections(Segment *seg1, Segment *seg2) {
    vec<Connection*, 2> connections;
    for (Connection *c : seg1->edges) {
        if (c->getOtherSegment(seg1) == seg2) {
            connections.push_back(c);
        }
    }
    return connections;
}

Junction* onNewJunction(const point2m& pos, radian_t inOri, radian_t outOri, uint8_t numInSegments, Direction inSegmentDir, uint8_t numOutSegments) {
    LOG_WAIT_DEBUG("New junction | currentSeg: %c", currentSeg->name);
    Junction * const junc = getNew(junctions);
    if (junc) {
        junc->idx = junctions.size();
        junc->pos = pos;
        addSegments(junc, inOri, outOri, numInSegments, inSegmentDir, numOutSegments);

        Junction::segment_map::iterator inSegments  = junc->getSideSegments(inOri);
        Junction::segment_map::iterator outSegments = junc->getSideSegments(outOri);

        for (Junction::side_segment_map::iterator in = inSegments->second.begin(); in != inSegments->second.end(); ++in) {
            for (Junction::side_segment_map::iterator out = outSegments->second.begin(); out != outSegments->second.end(); ++out) {
                Connection *conn = getNew(connections);
                conn->junction = junc;
                addConnection(conn, in->second, { inSegments->first, in->first }, out->second, { outSegments->first, out->first });
            }
        }

        endTime += second_t(10);

    } else {
        LOG_WAIT_ERROR("Junction pool empty");
    }

    return junc;
}

Status mergeSegments(Segment *oldSeg, Segment *newSeg) {
    Status result = Status::INVALID_DATA;

    if (oldSeg != newSeg) {
        if (newSeg->isFloating()) {
            // if both segments are floating, merges them
            if (oldSeg->isFloating()) {
                // adds all the connections of this segment to the previously defined floating segment (updates segment pointers)
                for (Connection *c : oldSeg->edges) {
                    c->updateSegment(oldSeg, newSeg);
                    newSeg->edges.push_back(c);
                    c->junction->updateSegment(oldSeg, newSeg);
                }

                if (laneChange.seg == oldSeg) {
                    laneChange.seg = newSeg;
                }

                oldSeg->isActive = false;
                result = Status::OK;
                LOG_WAIT_DEBUG("Merged: %c -> %c", oldSeg->name, newSeg->name);
            } else {
                LOG_WAIT_ERROR("Old segment not floating (NOT OK)");
            }
        } else {
            LOG_WAIT_ERROR("New segment not floating (NOT OK)");
        }
    }

    return result;
}

Connection* onExistingJunction(Junction *junc, bool isValid, const point2m& pos, radian_t inOri, Direction inSegmentDir) {

    LOG_WAIT_DEBUG("Junction: %d | currentSeg: %c", junc->idx, currentSeg->name);

    vTaskSuspendAll();
    const point2m prevCarPos = globals::car.pose.pos;
    globals::car.pose.pos += (junc->pos - pos);
    xTaskResumeAll();

    LOG_WAIT_DEBUG("Car pos updated: (%f, %f) -> (%f, %f)", prevCarPos.X.get(), prevCarPos.Y.get(), globals::car.pose.pos.X.get(), globals::car.pose.pos.Y.get());

    Connection *nextConn = nullptr;

    if (isValid) {
        Segment * const junctionSeg = junc->getSegment(inOri, inSegmentDir);

        if (junctionSeg) {
            LOG_WAIT_DEBUG("junctionSeg: %c", junctionSeg->name);

            if (currentSeg != junctionSeg) {
                if (!isOk(mergeSegments(currentSeg, junctionSeg))) {
                    plannedRoute.reset();
                }
                currentSeg = junctionSeg;
            }

            // If there is no planned route, creates a new route.
            // The destination of the route will be the closest floating segment.
            // If there are no floating segments, it means the labyrinth has been completed.
            // In this case the destination will be the lane change segment.
            if (plannedRoute.connections.size() || getRoute()) {
                nextConn = plannedRoute.nextConnection();
            } else {
                LOG_WAIT_ERROR("createNewRoute() failed");
            }
        } else {
            LOG_WAIT_ERROR("Junction segment with orientation [%fdeg] and direction [%s] not found", static_cast<degree_t>(inOri).get(), to_string(inSegmentDir));
        }
    } else {
        LOG_WAIT_ERROR("Unexpected junction topology, skips segment merge and route planning");
    }

    return nextConn;
}

Direction onJunctionDetected(const point2m& pos, radian_t inOri, radian_t outOri, uint8_t numInSegments, Direction inSegmentDir, uint8_t numOutSegments) {

    bool isJuncValid = false;
    Junction *junc = findExistingJunction(pos, inOri, outOri, numInSegments, inSegmentDir, numOutSegments, isJuncValid);

    if (currentSeg->isDeadEnd && prevConn && junc != prevConn->junction) {
        junc = prevConn->junction;
        isJuncValid = false;
        LOG_WAIT_ERROR("After a dead-end, detected junction is not the previous one.");
    }

    Connection *nextConn = nullptr;
    Maneuver nextManeuver = { outOri, Direction::CENTER };

    if (!junc) { // new junction found - creates new segments and adds connections
        LOG_WAIT_DEBUG("new junction");
        junc = onNewJunction(pos, inOri, outOri, numInSegments, inSegmentDir, numOutSegments);
    } else { // arrived at a previously found junction - the corresponding floating segment of the junction (if any) must be merged with the current segment
        LOG_WAIT_DEBUG("existing junction");
        nextConn = onExistingJunction(junc, isJuncValid, pos, inOri, inSegmentDir);
    }

    LOG_WAIT_DEBUG("junction handling done, choosing next maneuver");

    if (nextConn) {
        nextManeuver = nextConn->getManeuver(nextConn->getOtherSegment(currentSeg));
    }  else {
        LOG_WAIT_DEBUG("!nextConn");

        Junction::segment_map::iterator outSegments = junc->getSideSegments(nextManeuver.orientation);
        Segment **pNextSeg = outSegments->second.get((nextManeuver.direction = Direction::RIGHT));
        if (!pNextSeg) {
            pNextSeg = outSegments->second.get((nextManeuver.direction = Direction::CENTER));
        }

        if (pNextSeg) {
            for (Connection *c : getConnections(currentSeg, *pNextSeg)) {
                if (c->junction == junc && c->getManeuver(*pNextSeg) == nextManeuver) {
                    nextConn = c;
                    break;
                }
            }
        } else {
            LOG_WAIT_ERROR("pNextSeg should not be nullptr at this point");
        }
    }

    if (nextConn) {

        // If LANE_CHANGE pattern has been found, but last junction before lane change has not been filled,
        // it means that the LANE_CHANGE pattern has been detected with NEGATIVE sign,
        // therefore the current junction (the one detected after the LANE_CHANGE pattern) will be
        // the junction before the lane change when planning the trajectory.
        // This way the LANE_CHANGE pattern will be detected with POSITIVE sign during lane change.
        if (laneChange.seg && !laneChange.lastJunc) {
            laneChange.lastJunc = junc;
            laneChange.lastManeuver = nextConn->getManeuver(currentSeg);
        }

        Segment *nextSeg = nextConn->getOtherSegment(currentSeg);
        currentSeg = nextSeg;
        prevConn = nextConn;
        LOG_WAIT_DEBUG("Next: %c (%s)", nextSeg->name, micro::to_string(nextManeuver.direction));
    } else {
        LOG_WAIT_ERROR("No next connection found");
    }

    return nextManeuver.direction;
}

struct TestCase {
    Pose pose;
    LinePattern pattern;
};

const TestCase testCase1[] = {
    { { { meter_t(0), meter_t(0) }, PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 1
    { { { meter_t(0), meter_t(2) }, PI_2 }, { LinePattern::Type::JUNCTION_1,  Sign::NEGATIVE, Direction::CENTER } },
    { { { meter_t(0), meter_t(2) }, PI_2 }, { LinePattern::Type::JUNCTION_2,  Sign::POSITIVE, Direction::RIGHT  } },
    { { { meter_t(0), meter_t(2) }, PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 2
    { { { meter_t(3), meter_t(4) }, PI_2 }, { LinePattern::Type::JUNCTION_3,  Sign::NEGATIVE, Direction::RIGHT  } },
    { { { meter_t(3), meter_t(4) }, PI_2 }, { LinePattern::Type::JUNCTION_2,  Sign::POSITIVE, Direction::RIGHT  } },
    { { { meter_t(3), meter_t(4) }, PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 3
    { { { meter_t(0), meter_t(5) }, -PI_2 }, { LinePattern::Type::JUNCTION_2,  Sign::NEGATIVE, Direction::LEFT   } },
    { { { meter_t(0), meter_t(5) }, -PI_2 }, { LinePattern::Type::JUNCTION_1,  Sign::POSITIVE, Direction::CENTER } },
    { { { meter_t(0), meter_t(5) }, -PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 1
    { { { meter_t(0), meter_t(2) }, -PI_2 }, { LinePattern::Type::JUNCTION_2,  Sign::NEGATIVE, Direction::LEFT   } },
    { { { meter_t(0), meter_t(2) }, -PI_2 }, { LinePattern::Type::JUNCTION_1,  Sign::POSITIVE, Direction::CENTER } },
    { { { meter_t(0), meter_t(2) }, -PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 4
    { { { meter_t(0), meter_t(-2) }, -PI_2 }, { LinePattern::Type::JUNCTION_1,  Sign::NEGATIVE, Direction::CENTER } },
    { { { meter_t(0), meter_t(-2) }, -PI_2 }, { LinePattern::Type::JUNCTION_2,  Sign::POSITIVE, Direction::RIGHT  } },
    { { { meter_t(0), meter_t(-2) }, -PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // LANE_CHANGE
    { { { meter_t(-2), meter_t(-4) }, radian_t(0) }, { LinePattern::Type::LANE_CHANGE, Sign::POSITIVE, Direction::RIGHT  } },
    { { { meter_t(-1), meter_t(-4) }, radian_t(0) }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 4
    { { { meter_t(0), meter_t(-2) }, PI_2 }, { LinePattern::Type::JUNCTION_2,  Sign::NEGATIVE, Direction::LEFT   } },
    { { { meter_t(0), meter_t(-2) }, PI_2 }, { LinePattern::Type::JUNCTION_1,  Sign::POSITIVE, Direction::CENTER } },
    { { { meter_t(0), meter_t(-2) }, PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 1
    { { { meter_t(0), meter_t(2) }, PI_2 }, { LinePattern::Type::JUNCTION_1,  Sign::NEGATIVE, Direction::CENTER } },
    { { { meter_t(0), meter_t(2) }, PI_2 }, { LinePattern::Type::JUNCTION_2,  Sign::POSITIVE, Direction::RIGHT  } },
    { { { meter_t(0), meter_t(2) }, PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 2
    { { { meter_t(3), meter_t(4) }, PI_2 }, { LinePattern::Type::JUNCTION_3,  Sign::NEGATIVE, Direction::RIGHT  } },
    { { { meter_t(3), meter_t(4) }, PI_2 }, { LinePattern::Type::JUNCTION_2,  Sign::POSITIVE, Direction::RIGHT  } },
    { { { meter_t(3), meter_t(4) }, PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 3
    { { { meter_t(0), meter_t(5) }, -PI_2 }, { LinePattern::Type::JUNCTION_2,  Sign::NEGATIVE, Direction::RIGHT  } },
    { { { meter_t(0), meter_t(5) }, -PI_2 }, { LinePattern::Type::JUNCTION_1,  Sign::POSITIVE, Direction::CENTER } },
    { { { meter_t(0), meter_t(5) }, -PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 1
    { { { meter_t(0), meter_t(2) }, -PI_2 }, { LinePattern::Type::JUNCTION_2,  Sign::NEGATIVE, Direction::LEFT   } },
    { { { meter_t(0), meter_t(2) }, -PI_2 }, { LinePattern::Type::JUNCTION_1,  Sign::POSITIVE, Direction::CENTER } },
    { { { meter_t(0), meter_t(2) }, -PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 4
    { { { meter_t(0), meter_t(-2) }, -PI_2 }, { LinePattern::Type::JUNCTION_1,  Sign::NEGATIVE, Direction::CENTER } },
    { { { meter_t(0), meter_t(-2) }, -PI_2 }, { LinePattern::Type::JUNCTION_2,  Sign::POSITIVE, Direction::RIGHT  } },
    { { { meter_t(0), meter_t(-2) }, -PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // LANE_CHANGE
    { { { meter_t(-2), meter_t(-4) }, radian_t(0) }, { LinePattern::Type::LANE_CHANGE, Sign::POSITIVE, Direction::RIGHT  } },
    { { { meter_t(-1), meter_t(-4) }, radian_t(0) }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 4
    { { { meter_t(0), meter_t(-2) }, PI_2 }, { LinePattern::Type::JUNCTION_2,  Sign::NEGATIVE, Direction::LEFT   } },
    { { { meter_t(0), meter_t(-2) }, PI_2 }, { LinePattern::Type::JUNCTION_1,  Sign::POSITIVE, Direction::CENTER } },
    { { { meter_t(0), meter_t(-2) }, PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 1
    { { { meter_t(0), meter_t(2) }, PI_2 }, { LinePattern::Type::JUNCTION_1,  Sign::NEGATIVE, Direction::CENTER } },
    { { { meter_t(0), meter_t(2) }, PI_2 }, { LinePattern::Type::JUNCTION_2,  Sign::POSITIVE, Direction::RIGHT  } },
    { { { meter_t(0), meter_t(2) }, PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 3
    { { { meter_t(0), meter_t(5) }, PI_2 }, { LinePattern::Type::JUNCTION_1,  Sign::NEGATIVE, Direction::CENTER } },
    { { { meter_t(0), meter_t(5) }, PI_2 }, { LinePattern::Type::JUNCTION_2,  Sign::POSITIVE, Direction::LEFT   } },
    { { { meter_t(0), meter_t(5) }, PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 2
    { { { meter_t(3), meter_t(4) }, -PI_2 }, { LinePattern::Type::JUNCTION_2,  Sign::NEGATIVE, Direction::RIGHT  } },
    { { { meter_t(3), meter_t(4) }, -PI_2 }, { LinePattern::Type::JUNCTION_3,  Sign::POSITIVE, Direction::CENTER } },
    { { { meter_t(3), meter_t(4) }, -PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 5
    { { { meter_t(5), meter_t(2) }, PI_2 }, { LinePattern::Type::JUNCTION_1,  Sign::NEGATIVE, Direction::CENTER } },
    { { { meter_t(5), meter_t(2) }, PI_2 }, { LinePattern::Type::JUNCTION_2,  Sign::POSITIVE, Direction::RIGHT  } },
    { { { meter_t(5), meter_t(2) }, PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // DEAD_END
    { { { meter_t(5), meter_t(5) }, PI_2 }, { LinePattern::Type::DEAD_END,    Sign::NEUTRAL, Direction::CENTER } },
    { { { meter_t(5), meter_t(5) }, PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL, Direction::CENTER } },

    // 5
    { { { meter_t(5), meter_t(2) }, -PI_2 }, { LinePattern::Type::JUNCTION_2,  Sign::NEGATIVE, Direction::RIGHT  } },
    { { { meter_t(5), meter_t(2) }, -PI_2 }, { LinePattern::Type::JUNCTION_1,  Sign::POSITIVE, Direction::CENTER } },
    { { { meter_t(5), meter_t(2) }, -PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 2
    { { { meter_t(3), meter_t(4) }, PI_2 }, { LinePattern::Type::JUNCTION_3,  Sign::NEGATIVE, Direction::CENTER } },
    { { { meter_t(3), meter_t(4) }, PI_2 }, { LinePattern::Type::JUNCTION_2,  Sign::POSITIVE, Direction::LEFT   } },
    { { { meter_t(3), meter_t(4) }, PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 3
    { { { meter_t(0), meter_t(5) }, -PI_2 }, { LinePattern::Type::JUNCTION_2,  Sign::NEGATIVE, Direction::LEFT   } },
    { { { meter_t(0), meter_t(5) }, -PI_2 }, { LinePattern::Type::JUNCTION_1,  Sign::POSITIVE, Direction::CENTER } },
    { { { meter_t(0), meter_t(5) }, -PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 1
    { { { meter_t(0), meter_t(2) }, -PI_2 }, { LinePattern::Type::JUNCTION_2,  Sign::NEGATIVE, Direction::LEFT   } },
    { { { meter_t(0), meter_t(2) }, -PI_2 }, { LinePattern::Type::JUNCTION_1,  Sign::POSITIVE, Direction::CENTER } },
    { { { meter_t(0), meter_t(2) }, -PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 4
    { { { meter_t(0), meter_t(-2) }, -PI_2 }, { LinePattern::Type::JUNCTION_1,  Sign::NEGATIVE, Direction::CENTER } },
    { { { meter_t(0), meter_t(-2) }, -PI_2 }, { LinePattern::Type::JUNCTION_2,  Sign::POSITIVE, Direction::RIGHT  } },
    { { { meter_t(0), meter_t(-2) }, -PI_2 }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // LANE_CHANGE
    { { { meter_t(-2), meter_t(-4) }, radian_t(0) }, { LinePattern::Type::LANE_CHANGE, Sign::POSITIVE, Direction::RIGHT  } },
    { { { meter_t(-1), meter_t(-4) }, radian_t(0) }, { LinePattern::Type::NONE,        Sign::NEUTRAL,  Direction::CENTER } }
};

const TestCase testCase2[] = {
    { { { meter_t(0), meter_t(0) }, radian_t(0) }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 1
    { { { meter_t(0), meter_t(2) }, radian_t(0) }, { LinePattern::Type::JUNCTION_1,  Sign::NEGATIVE, Direction::CENTER } },
    { { { meter_t(0), meter_t(2) }, radian_t(0) }, { LinePattern::Type::JUNCTION_3,  Sign::POSITIVE, Direction::CENTER } },
    { { { meter_t(0), meter_t(2) }, radian_t(0) }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } },

    // 1 - wrong pattern
    { { { meter_t(0), meter_t(2) }, PI }, { LinePattern::Type::JUNCTION_3,  Sign::NEGATIVE, Direction::CENTER } },
    { { { meter_t(0), meter_t(2) }, PI }, { LinePattern::Type::JUNCTION_1,  Sign::POSITIVE, Direction::CENTER } },
    { { { meter_t(0), meter_t(2) }, PI }, { LinePattern::Type::SINGLE_LINE, Sign::NEUTRAL,  Direction::CENTER } }
};

bool turnAround(const DetectedLines& detectedLines, ControlData& controlData) {

    static constexpr radian_t TURN_WHEEL_ANGLE = degree_t(22);
    static constexpr millisecond_t PREPARE_TIME = millisecond_t(200);

    const radian_t angleDiff = normalizePM180(globals::car.pose.angle - y_turn.startOrientation);
    const Y_turnState prevState = y_turn.state;
    controlData.directControl = true;

    switch (y_turn.state) {
    case Y_turnState::INACTIVE:
        globals::lineDetectionEnabled = false;
        y_turn.startOrientation = normalizePM180(globals::car.pose.angle);
        controlData.speed = globals::speed_TURN_AROUND;
        controlData.frontWheelAngle = radian_t(0);
        controlData.rearWheelAngle = radian_t(0);
        y_turn.state = Y_turnState::FWD_LEFT1;
        break;

    case Y_turnState::FWD_LEFT1:
        controlData.speed = globals::speed_TURN_AROUND;
        controlData.frontWheelAngle = TURN_WHEEL_ANGLE;
        controlData.rearWheelAngle = -TURN_WHEEL_ANGLE;
        if (angleDiff >= degree_t(60)) {
            y_turn.state = Y_turnState::PREPARE_BWD_RIGHT;
        }
        break;

    case Y_turnState::PREPARE_BWD_RIGHT:
        controlData.speed = m_per_sec_t(0);
        controlData.frontWheelAngle = -TURN_WHEEL_ANGLE;
        controlData.rearWheelAngle = TURN_WHEEL_ANGLE;
        if (getTime() - y_turn.stateStartTime >= PREPARE_TIME) {
            y_turn.state = Y_turnState::BWD_RIGHT;
        }
        break;

    case Y_turnState::BWD_RIGHT:
        controlData.speed = -globals::speed_TURN_AROUND;
        controlData.frontWheelAngle = -TURN_WHEEL_ANGLE;
        controlData.rearWheelAngle = TURN_WHEEL_ANGLE;
        if (angleDiff >= degree_t(120)) {
            y_turn.state = Y_turnState::PREPARE_FWD_LEFT2;
        }
        break;

    case Y_turnState::PREPARE_FWD_LEFT2:
        controlData.speed = m_per_sec_t(0);
        controlData.frontWheelAngle = TURN_WHEEL_ANGLE;
        controlData.rearWheelAngle = -TURN_WHEEL_ANGLE;
        if (getTime() - y_turn.stateStartTime >= PREPARE_TIME) {
            globals::lineDetectionEnabled = true;
            y_turn.state = Y_turnState::FWD_LEFT2;
        }
        break;

    case Y_turnState::FWD_LEFT2:
        controlData.speed = globals::speed_TURN_AROUND;
        controlData.frontWheelAngle = TURN_WHEEL_ANGLE;
        controlData.rearWheelAngle = -TURN_WHEEL_ANGLE;
        break;
    }

    if (y_turn.state != prevState) {
        y_turn.stateStartTime = getTime();
    }

    const bool finished = Y_turnState::FWD_LEFT2 == y_turn.state && abs(angleDiff) >= degree_t(145) && LinePattern::NONE != detectedLines.pattern.type;
    if (finished) {
        y_turn.state = Y_turnState::INACTIVE;
    }
    return finished;
}

bool navigateLabyrinth(const DetectedLines& prevDetectedLines, const DetectedLines& detectedLines, ControlData& controlData) {

//    // TODO only for testing -------------------------------------------
//
//#define TEST_CASE testCase2
//    static const TestCase *testCase = TEST_CASE;
//    static const uint32_t numTestCasePoints = ARRAY_SIZE(TEST_CASE);
//    static uint32_t testCasePointIdx = 0;
//#undef TEST_CASE
//
//    if (testCasePointIdx < numTestCasePoints) {
//        globals::car.pose = testCase[testCasePointIdx].pose;
//        const_cast<DetectedLines&>(detectedLines).pattern = testCase[testCasePointIdx].pattern;
//        testCasePointIdx++;
//    }
//
//    // -----------------------------------------------------------------

    static uint8_t numInSegments;
    static Direction inSegmentDir;
    static point2m inJunctionPos;
    static bool turnAroundActive = false;

    bool finished = false;
    controlData.speed = globals::speed_LAB_FWD;

    if (turnAroundActive) {
        if (turnAround(detectedLines, controlData)) {
            turnAroundActive = false;
        }
    } else if (detectedLines.pattern != prevDetectedLines.pattern) {

        switch (detectedLines.pattern.type) {
        case LinePattern::JUNCTION_1:
        case LinePattern::JUNCTION_2:
        case LinePattern::JUNCTION_3:
        {
            const uint8_t numSegments = LinePattern::JUNCTION_1 == detectedLines.pattern.type ? 1 :
                LinePattern::JUNCTION_2 == detectedLines.pattern.type ? 2 : 3;

            if (Sign::NEGATIVE == detectedLines.pattern.dir) {
                numInSegments = numSegments;
                inJunctionPos = globals::car.pose.pos;

                // Line pattern direction indicates on which side of the current line the OTHER lines are,
                // so if the current line is the leftmost line of three lines, pattern direction will be RIGHT.
                // Segment direction in a junction indicates which side the car should steer in order to follow a segment,
                // when leaving a junction.
                // Currently the car is entering a junction, therefore if the current line is the leftmost,
                // the car will need to steer to the right when leaving the junction.
                // So the segment direction is the same as the line pattern direction (in this example, RIGHT).
                inSegmentDir = detectedLines.pattern.side;

                // if there are 3 detected lines, follows center line, otherwise follows default main line
                if (3 == detectedLines.lines.size()) {
                    controlData.baseline = detectedLines.lines[1];
                }

            } else if (Sign::POSITIVE == detectedLines.pattern.dir) {
                const point2m junctionPos = avg(inJunctionPos, globals::car.pose.pos);

                const radian_t carOri = globals::car.pose.angle;
                const radian_t inOri  = normalize360(carOri + PI);
                const radian_t outOri = normalize360(carOri);

                const Direction steeringDir = onJunctionDetected(junctionPos, inOri, outOri, numInSegments, inSegmentDir, numSegments);

                // updates the main line (if an error occurs, does not change the default line)
                switch (steeringDir) {
                case Direction::LEFT:
                    if (detectedLines.lines.size() >= 2) {
                        controlData.baseline = detectedLines.lines[0];
                    }
                    break;
                case Direction::CENTER:
                    if (detectedLines.lines.size() == 1) {
                        controlData.baseline = detectedLines.lines[0];
                    } else if (detectedLines.lines.size() == 3) {
                        controlData.baseline = detectedLines.lines[1];
                    }
                    break;
                case Direction::RIGHT:
                    if (detectedLines.lines.size() >= 2) {
                        controlData.baseline = *detectedLines.lines.back();
                    }
                    break;
                }
            }
            break;
        }

        case LinePattern::DEAD_END:
            currentSeg->isDeadEnd = true;
            turnAroundActive = true;
            break;

        case LinePattern::LANE_CHANGE:
            if (getFloatingSegments().size()) {
                laneChange.seg = currentSeg;
                if (Sign::POSITIVE == detectedLines.pattern.dir && prevConn) {
                    laneChange.lastJunc = prevConn->junction;
                    laneChange.lastManeuver = prevConn->getManeuver(currentSeg);
                }
            } else {
                finished = true;
            }
            break;

        default:
            break;
        }
    }

    return finished;
}

bool changeLane(const DetectedLines& detectedLines, ControlData& controlData) {

    static constexpr meter_t LANE_DISTANCE = centimeter_t(60);

    if (laneChange.trajectory.length() == meter_t(0)) {
        globals::lineDetectionEnabled = false;

        laneChange.trajectory.setStartConfig(Trajectory::config_t{
            globals::car.pose.pos + vec2m(cfg::CAR_OPTO_CENTER_DIST, centimeter_t(0)).rotate(globals::car.pose.angle),
            globals::speed_LANE_CHANGE
        });

        laneChange.trajectory.appendSineArc(Trajectory::config_t{
            laneChange.trajectory.lastConfig().pos + vec2m(centimeter_t(80), -LANE_DISTANCE).rotate(globals::car.pose.angle),
            globals::speed_LANE_CHANGE
        }, globals::car.pose.angle, 30);
    }

    controlData = laneChange.trajectory.update(globals::car);

    if (laneChange.trajectory.length() - laneChange.trajectory.coveredDistance() < centimeter_t(50)) {
        globals::lineDetectionEnabled = true;
    }

    const bool finished = laneChange.trajectory.length() - laneChange.trajectory.coveredDistance() < centimeter_t(40) && LinePattern::NONE != detectedLines.pattern.type;
    if (finished) {
        laneChange.trajectory.clear();
    }
    return finished;
}

} // namespace

extern "C" void runProgLabyrinthTask(void const *argument) {

    vTaskDelay(100); // gives time to other tasks and panels to wake up

    DetectedLines prevDetectedLines, detectedLines;
    ControlData controlData;

    currentSeg = createNewSegment();
    endTime = getTime() + second_t(20);

    while (true) {
        switch (getActiveTask(globals::programState)) {
            case ProgramTask::Labyrinth:
                globals::distServoEnabled = false;
                globals::distSensorEnabled = false;

                xQueuePeek(detectedLinesQueue, &detectedLines, 0);

                controlData.directControl = false;
                LineCalculator::updateMainLine(detectedLines.lines, controlData.baseline);
                controlData.angle = degree_t(0);
                controlData.offset = millimeter_t(0);

                switch (globals::programState) {
                case ProgramState::NavigateLabyrinth:
                    if (navigateLabyrinth(prevDetectedLines, detectedLines, controlData)) {
                        globals::programState = ProgramState::LaneChange;
                    }
                    break;

                case ProgramState::LaneChange:
                    if (changeLane(detectedLines, controlData)) {
                        globals::programState = ProgramState::ReachSafetyCar;
                    }
                    break;
                default:
                    break;
                }

                xQueueOverwrite(controlQueue, &controlData);
                prevDetectedLines = detectedLines;
                break;

            default:
                vTaskDelay(20);
                break;
        }

        vTaskDelay(2);
    }

    vTaskDelete(nullptr);
}
