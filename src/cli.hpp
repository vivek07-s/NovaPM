#pragma once
// Command-line interface. argv[0] = program, argv[1] = command.
bool isCliCommand(const char* word);          // true for top, watch, history, alerts, ...
int  runCli(int argc, char** argv);           // returns the process exit code
