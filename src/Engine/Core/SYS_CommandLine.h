#ifndef _SYS_COMMANDLINE_H_
#define _SYS_COMMANDLINE_H_

#include <vector>
extern std::vector<const char *> sysCommandLineArguments;

extern int sysArgc;
extern const char **sysArgv;

void SYS_SetCommandLineArguments(int argc, const char **argv);
int SYS_GetArgc();
const char **SYS_GetArgv();

#endif
