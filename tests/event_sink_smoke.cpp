// Copyright (C) 2026 James Hickman
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

// Standalone smoke test for the Redis event sink: publishes one event of each
// type to a stream, then prints emitted/dropped counts. Read the stream back to
// confirm the JSON envelopes (see the accompanying python reader).
#include "fileengine/redis_event_sink.h"
#include "fileengine/event.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace fileengine;

int main(int argc, char** argv) {
    RedisEventSink::Options o;
    o.host = "localhost";
    o.port = 6379;
    o.stream = (argc > 1) ? argv[1] : "fe_smoke3";
    o.outbox_capacity = 100;
    if (const char* p = std::getenv("FILEENGINE_REDIS_PASSWORD")) o.password = p;
    else if (const char* q = std::getenv("REDDIS_PASSWORD")) o.password = q;

    RedisEventSink sink(o);
    sink.start();

    struct { FileEventType t; const char* tag; } cases[] = {
        {FileEventType::DirCreated,   "dir"},
        {FileEventType::FileCreated,  "file"},
        {FileEventType::FileUpdated,  "file"},
        {FileEventType::FileMoved,    "file"},
        {FileEventType::FileRenamed,  "file"},
        {FileEventType::FileDeleted,  "file"},
        {FileEventType::FileRestored, "file"},
        {FileEventType::AclChanged,   "acl"},
    };

    int i = 0;
    for (const auto& c : cases) {
        FileEvent e;
        e.event_id = "evt-" + std::to_string(i);
        e.type = c.t;
        e.tenant = (i % 2) ? "tenant_a" : "default";   // prove single-stream + tenant field
        e.file_uid = "uid-" + std::to_string(i);
        e.parent_uid = "parent-" + std::to_string(i);
        e.name = std::string(c.tag) + std::to_string(i);
        e.actor = "tester@rationalboxes.com";
        e.ts = "2026-06-23T21:00:0" + std::to_string(i) + "Z";
        if (c.t == FileEventType::AclChanged) { e.principal = "alice"; e.permissions = 7; }
        sink.publish(e);
        ++i;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    std::cout << "stream=" << o.stream
              << " emitted=" << sink.emitted()
              << " dropped=" << sink.dropped() << std::endl;
    sink.stop();
    return 0;
}
