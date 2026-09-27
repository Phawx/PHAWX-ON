/* The version: phawx.h, the resource script, the Makefile and the release workflow
   all read it from here. res/phawx.manifest repeats it by hand (assemblyIdentity
   version="x.y.z.0"), and make dist fails when the two differ. Keep it macros only
   (windres reads it). */
#ifndef PH_VERSION_H
#define PH_VERSION_H
#define PH_VER_MAJOR 1
#define PH_VER_MINOR 1
#define PH_VER_PATCH 0
#define PH_VERSION   "1.1.0"
#define PH_VERSION_W L"1.1.0"
#endif
