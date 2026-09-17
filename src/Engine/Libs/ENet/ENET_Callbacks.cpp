// CAPABILITY GUARD (MT_ENABLE_ENET). The file stays in all three build lists and
// compiles to nothing when the capability is off -- the guard-the-body,
// keep-the-file idiom, which exists because an Xcode PBXBuildFile cannot be
// conditioned on a build setting the way a CMake or MSBuild item can.
// MT_CAP_NET_TRANSPORT declared this removable and nothing enforced it: MT_ENABLE_ENET appeared in no source, so the code shipped whatever the manifest said.
#if MT_ENABLE_ENET

/** 
 @file callbacks.c
 @brief ENet callback functions
*/
#define ENET_BUILDING_LIB 1
#include "enet.h"

static ENetCallbacks callbacks = { malloc, free, abort };

int
enet_initialize_with_callbacks (ENetVersion version, const ENetCallbacks * inits)
{
   if (version < ENET_VERSION_CREATE (1, 3, 0))
     return -1;

   if (inits -> malloc != NULL || inits -> free != NULL)
   {
      if (inits -> malloc == NULL || inits -> free == NULL)
        return -1;

      callbacks.malloc = inits -> malloc;
      callbacks.free = inits -> free;
   }
      
   if (inits -> no_memory != NULL)
     callbacks.no_memory = inits -> no_memory;

   return enet_initialize ();
}

ENetVersion
enet_linked_version (void)
{
    return ENET_VERSION;
}
           
void *
enet_malloc (size_t size)
{
   void * memory = callbacks.malloc (size);

   if (memory == NULL)
     callbacks.no_memory ();

   return memory;
}

void
enet_free (void * memory)
{
   callbacks.free (memory);
}

#endif // MT_ENABLE_ENET
