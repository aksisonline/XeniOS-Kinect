#!/bin/sh
# Builds the native tracker (Swift + Vision + AppKit).
cd "$(dirname "$0")" && swiftc -O -swift-version 5 main.swift -o kinect_tracker_native
