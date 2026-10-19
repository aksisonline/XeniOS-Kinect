// Harness shim for xenia/base/logging.h
#pragma once
#include <cstdio>
#include <format>
#define XE_SHIM_LOG(tag, ...) std::fprintf(stderr, "[%s] %s\n", tag, std::format(__VA_ARGS__).c_str())
#define XELOGI(...) XE_SHIM_LOG("I", __VA_ARGS__)
#define XELOGW(...) XE_SHIM_LOG("W", __VA_ARGS__)
#define XELOGE(...) XE_SHIM_LOG("E", __VA_ARGS__)
