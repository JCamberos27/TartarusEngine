#pragma once

// #173 - runs the headless unit tests (see UnitTests.cpp). Returns the number of failed checks;
// `TartarusEngine.exe --unit-tests` exits with it, so 0 = pass.
int RunUnitTests();
