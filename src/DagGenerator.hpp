#ifndef DAGGENERATOR_HPP
#define DAGGENERATOR_HPP

#include "MemAllocHeap.hpp"

struct lua_State;



bool GenerateDag(const char *build_file, const char *dag_fn);

bool GenerateIdeIntegrationFiles(const char *build_file, int argc, const char **argv);



#endif
