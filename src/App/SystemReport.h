#pragma once

#include <string>
#include <vector>

struct GLFWwindow;

// One-shot rig dump: OS, CPU, RAM, GPU, driver, display, build. Collected into a block for Preferences > About - no
// longer spammed line-by-line to the Console (it lives in About now). Needs the GL context current.
std::vector<std::string> BuildSystemReport(GLFWwindow* window);
