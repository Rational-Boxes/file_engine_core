#!/bin/bash
cd /home/telendry/code/file_projects/file_engine_core/build/core
gdb -ex "set environment LD_LIBRARY_PATH=." -ex "run" -ex "bt" -ex "thread apply all bt" --args ./fileengine_server